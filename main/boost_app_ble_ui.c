#include "boost_app_ble_ui.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

#include "boost_app_ble.h"
#include "boost_display.h"
#include "boost_gauge.h"

/*
 * Passkey overlay lifecycle.
 *
 * The passkey and pair-result callbacks fire on the NimBLE host task, which
 * must never touch LVGL or block. They only record plain volatile scalars;
 * this module's LVGL poll timer (created on the LVGL worker task, the same
 * convention the AP-join QR hold timer in boost_page.c uses) consumes them
 * inside lv_timer_handler() while the adapter's recursive display lock is
 * held, so object creation/deletion always runs on the LVGL task under
 * boost_display_lock.
 */

static const char *TAG = "boost_app_ble_ui";

#define PASSKEY_POLL_MS     200u
#define PASSKEY_DISMISS_MS  60000u
#define PASSKEY_FONT        (&lv_font_montserrat_48)
#define HINT_FONT           (&lv_font_montserrat_16)
#define OVERLAY_BG_OPA      LV_OPA_70

static bool s_init_done;
static lv_obj_t *s_overlay;
static lv_obj_t *s_digits_label;
static uint32_t s_show_deadline_ms;

/* Written by the NimBLE host task callbacks, consumed by the LVGL poll timer.
 * Single-word volatile scalars (the same cross-task convention boost_app_ble
 * uses for its connection state); a passkey write happens-before its
 * s_show_pending store, and the consumer reads the flag before the value. */
static volatile bool s_show_pending;
static volatile uint32_t s_pending_passkey;
static volatile bool s_dismiss_pending;

static void overlay_remove(bool cancel_pending_show);

static void overlay_click_cb(lv_event_t *event)
{
    (void)event;
    /* Any fresh tap dismisses; a show request already queued for a newer
     * pairing is cancelled too so the overlay does not resurrect next tick. */
    overlay_remove(true);
}

static void overlay_remove(bool cancel_pending_show)
{
    if (s_overlay != NULL) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_digits_label = NULL;
        /* Resume GIF playback (direct panel push) now that the overlay is
         * gone, mirroring the QR overlay in boost_page.c. */
        boost_gauge_media_resume();
    }
    s_dismiss_pending = false;
    if (cancel_pending_show) {
        s_show_pending = false;
    }
}

static void overlay_show(uint32_t passkey)
{
    char digits[8];
    snprintf(digits, sizeof(digits), "%06lu", (unsigned long)passkey);

    if (s_overlay != NULL) {
        /* A newer passkey for a fresh pairing attempt: refresh in place, no
         * object churn, and restart the auto-dismiss window. */
        lv_label_set_text(s_digits_label, digits);
        s_show_deadline_ms = lv_tick_get() + PASSKEY_DISMISS_MS;
        s_show_pending = false;
        ESP_LOGI(TAG, "passkey refreshed: %s", digits);
        return;
    }

    /* Full-screen, dark translucent cover on the active screen, sized exactly
     * like the QR overlay. The screen object survives theme rebuilds (scene
     * children live under boost_page's page root, not the screen). */
    s_overlay = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, 466, 466);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, OVERLAY_BG_OPA, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);

    s_digits_label = lv_label_create(s_overlay);
    lv_label_set_text(s_digits_label, digits);
    lv_obj_set_style_text_color(s_digits_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_digits_label, PASSKEY_FONT, 0);
    lv_obj_align(s_digits_label, LV_ALIGN_CENTER, 0, -28);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "Enter on iPhone/phone to pair");
    lv_obj_set_style_text_color(hint, lv_color_white(), 0);
    lv_obj_set_style_text_font(hint, HINT_FONT, 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 44);

    s_show_deadline_ms = lv_tick_get() + PASSKEY_DISMISS_MS;
    s_show_pending = false;
    /* Pause GIF playback so its direct panel push cannot overwrite the
     * overlay, exactly like the QR overlay does. */
    boost_gauge_media_pause();
    ESP_LOGI(TAG, "passkey overlay shown: %s", digits);
}

static void passkey_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (s_show_pending) {
        overlay_show(s_pending_passkey);
        return;
    }
    if (s_dismiss_pending) {
        overlay_remove(false);
        return;
    }
    if (s_overlay != NULL && lv_tick_elaps(s_show_deadline_ms) >= PASSKEY_DISMISS_MS) {
        overlay_remove(false);
    }
}

static void passkey_cb(uint32_t passkey, void *ctx)
{
    (void)ctx;
    /* A fresh passkey supersedes any pending dismissal from an earlier
     * attempt; the timer consumes both in order. */
    s_dismiss_pending = false;
    s_pending_passkey = passkey;
    s_show_pending = true;
    ESP_LOGI(TAG, "passkey callback: %06lu", (unsigned long)passkey);
}

static void pair_result_cb(bool ok, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "pair result: %s", ok ? "paired" : "failed/repeat");
    s_dismiss_pending = true;
}

void boost_app_ble_ui_init(void)
{
    if (s_init_done) {
        return;
    }
    s_init_done = true;

    /* Register before creating the timer: if a passkey arrives in between the
     * pending flags are recorded and the first timer tick shows the overlay. */
    boost_app_ble_set_passkey_display_cb(passkey_cb, NULL);
    boost_app_ble_set_pair_result_cb(pair_result_cb, NULL);

    if (boost_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "display lock failed; passkey overlay timer not created");
        return;
    }
    lv_timer_t *timer = lv_timer_create(passkey_timer_cb, PASSKEY_POLL_MS, NULL);
    boost_display_unlock();
    if (timer == NULL) {
        ESP_LOGW(TAG, "passkey overlay timer create failed");
    }
}
