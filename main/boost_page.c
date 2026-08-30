#include "boost_page.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_theme.h"

#ifdef ESP_PLATFORM
#include "boost_app_ble.h"
#include "boost_display.h"
#include "boost_model.h"
#include "boost_network.h"
#include "boost_obd.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
/* Host sim: the BLE links do not exist; the toggles page renders unchecked.
 * Calls are recorded so the sim can assert the deferred toggle was applied. */
int g_sim_app_ble_set_calls = 0;
int g_sim_obd_set_calls = 0;
bool g_sim_app_ble_state = false;
bool g_sim_obd_state = false;
static bool boost_obd_enabled(void) { return g_sim_obd_state; }
static bool boost_app_ble_enabled(void) { return g_sim_app_ble_state; }
static void boost_obd_set_enabled(bool e) { g_sim_obd_set_calls++; g_sim_obd_state = e; }
static void boost_app_ble_set_enabled(bool e) { g_sim_app_ble_set_calls++; g_sim_app_ble_state = e; }
#define BOOST_AP_PASSWORD "boost1234" 
#endif

#define PAGE_SIZE 466
#define TAP_SLOP_PX 12
#define SWIPE_MIN_PX 48
/* Calibrated from repeated on-glass tests rather than inferred from the panel
 * command timestamp. Keep this explicit so the physical feel stays intentional. */
#define HOLD_DIM_MS 1000
/* Two fingers held this long shows the AP-join QR, distinct from the 1 s
 * hold-to-dim (suppressed while both fingers are down). */
#define QR_HOLD_MS 2200
#define QR_POLL_MS 100

static const char *TAG = "boost_page";
static lv_obj_t *s_page_root[2];
static boost_page_id_t s_active = BOOST_PAGE_BOOST;
static lv_obj_t *s_screen;
static lv_indev_t *s_press_indev;
static lv_point_t s_start;
static int32_t s_max_dx;
static int32_t s_max_dy;
static bool s_press_active;
static bool s_hold_fired;
#ifdef ESP_PLATFORM
static int64_t s_lvgl_press_us;
#endif
static bool s_tpms_built;
static lv_obj_t *s_qr_overlay;
static bool s_qr_active;
static uint32_t s_qr_hold_start_ms;
static bool s_two_finger_seen;
/* Overlay page 0 = QR, page 1 = BLE toggles. A left swipe on the QR flips to
 * the toggles; a fresh tap still dismisses the whole overlay. */
static bool s_qr_toggles_shown;
static int32_t s_qr_press_x;
static bool s_qr_press_tracking;

static bool media_active(void);

static int32_t abs_i32(int32_t x) { return x < 0 ? -x : x; }

static bool media_active(void)
{
    return boost_gauge_media_active();
}

static void show_page(boost_page_id_t page);
static void hide_qr(void);
static void qr_click_cb(lv_event_t *event);
static void qr_pressing_cb(lv_event_t *event);
static void qr_flip_to(bool toggles);
static void qr_swipe_press_cb(lv_event_t *event);
static void qr_swipe_release_cb(lv_event_t *event);
static void qr_toggle_obd_cb(lv_event_t *event);
static void qr_toggle_app_cb(lv_event_t *event);
typedef struct {
    char ap_ssid[33];
    bool sta_connected;
    char sta_ip[16];
} qr_ap_info_t;

/* The one platform difference: where the AP identity comes from. */
static void qr_ap_info(qr_ap_info_t *out)
{
#ifdef ESP_PLATFORM
    boost_net_status_t net;
    boost_network_get_status(&net);
    strlcpy(out->ap_ssid, net.ap_ssid, sizeof(out->ap_ssid));
    out->sta_connected = net.sta_connected && net.sta_ip[0] != '\0';
    strlcpy(out->sta_ip, net.sta_ip, sizeof(out->sta_ip));
#else
    memset(out, 0, sizeof(*out));
    strlcpy(out->ap_ssid, "BoostGauge-SIM", sizeof(out->ap_ssid));
    /* Connected + IP so sim screenshots exercise the two-line SSID/IP label
     * (the branch a joined STA shows). Set false to check SSID-only. */
    out->sta_connected = true;
    strlcpy(out->sta_ip, "192.168.4.2", sizeof(out->sta_ip));
#endif
}

/* Full-screen QR overlay for joining the SoftAP. Dismissed by any fresh tap
 * (the overlay is CLICKABLE and covers the whole screen, so the release that
 * ended the two-finger hold does not count - its press target predates the
 * overlay). The opaque black cover hides the gauge underneath; boost_page_update
 * is gated on s_qr_active so the 16 ms path stops invalidating under it.
 * Page 0 is the QR; a left swipe rebuilds it as the connections-toggles page
 * (s_qr_toggles_shown). One shared widget tree - the host sim screenshots
 * verify the exact layout that reaches the glass. */
static void show_qr(void)
{
    if (s_qr_active) return;
    qr_ap_info_t ap;
    qr_ap_info(&ap);

    s_qr_overlay = lv_obj_create(s_screen);
    lv_obj_remove_style_all(s_qr_overlay);
    lv_obj_set_size(s_qr_overlay, PAGE_SIZE, PAGE_SIZE);
    lv_obj_set_pos(s_qr_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_qr_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_qr_overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_qr_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_qr_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_qr_overlay, qr_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_qr_overlay, qr_pressing_cb, LV_EVENT_PRESSING, NULL);
    s_qr_press_tracking = false;

    /* Page indicator + swipe hint render on BOTH pages so the two-page
     * structure is visible from either side. The dots are real objects, not
     * font glyphs (the -/X glyph pair read as mystery buttons); the active
     * page's dot is lit. Pure indicators - not clickable, a tap on one falls
     * through to the overlay's dismiss. */

    if (s_qr_toggles_shown) {
        lv_obj_t *title = lv_label_create(s_qr_overlay);
        lv_label_set_text(title, "Connections");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

        /* Each toggle row is one big tappable card: the WHOLE row flips the
         * switch, so a mistap on the label cannot fall through to the overlay
         * and dismiss the screen. */
        lv_obj_t *obd_row = lv_obj_create(s_qr_overlay);
        lv_obj_remove_style_all(obd_row);
        lv_obj_set_size(obd_row, PAGE_SIZE - 96, 96);
        lv_obj_align(obd_row, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_remove_flag(obd_row, LV_OBJ_FLAG_CLICKABLE);
        /* lv_obj_create() sets CLICKABLE by default (lv_obj.c constructor), so
         * the flag must be REMOVED explicitly: a clickable card would own every
         * press on it and swallow swipes. Non-clickable -> hit-test falls
         * through to the overlay's gesture tracker. Only the switch is
         * interactive. */
        lv_obj_set_style_bg_color(obd_row, lv_color_hex(0x14161a), 0);
        lv_obj_set_style_bg_opa(obd_row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(obd_row, 16, 0);

        lv_obj_t *obd_label = lv_label_create(obd_row);
        lv_obj_remove_flag(obd_label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(obd_label, "OBD2 link");
        lv_obj_set_style_text_color(obd_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(obd_label, &lv_font_montserrat_24, 0);
        lv_obj_align(obd_label, LV_ALIGN_LEFT_MID, 24, 0);

        lv_obj_t *obd_sw = lv_switch_create(obd_row);
        lv_obj_set_size(obd_sw, 88, 44);
        lv_obj_align(obd_sw, LV_ALIGN_RIGHT_MID, -24, 0);
        /* Bigger finger target via an INVISIBLE halo only - styling pad_* on
         * LV_PART_MAIN shrinks the indicator track (the skinny-line bug). */
        lv_obj_set_ext_click_area(obd_sw, 16);
        if (boost_obd_enabled()) lv_obj_add_state(obd_sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(obd_sw, qr_swipe_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(obd_sw, qr_swipe_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(obd_sw, qr_toggle_obd_cb, LV_EVENT_VALUE_CHANGED, NULL);

        lv_obj_t *app_row = lv_obj_create(s_qr_overlay);
        lv_obj_remove_style_all(app_row);
        lv_obj_set_size(app_row, PAGE_SIZE - 96, 96);
        lv_obj_align(app_row, LV_ALIGN_TOP_MID, 0, 266);
        lv_obj_remove_flag(app_row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(app_row, lv_color_hex(0x14161a), 0);
        lv_obj_set_style_bg_opa(app_row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(app_row, 16, 0);

        lv_obj_t *app_label = lv_label_create(app_row);
        lv_obj_remove_flag(app_label, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(app_label, "App link");
        lv_obj_set_style_text_color(app_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(app_label, &lv_font_montserrat_24, 0);
        lv_obj_align(app_label, LV_ALIGN_LEFT_MID, 24, 0);

        lv_obj_t *app_sw = lv_switch_create(app_row);
        lv_obj_set_size(app_sw, 88, 44);
        lv_obj_align(app_sw, LV_ALIGN_RIGHT_MID, -24, 0);
        lv_obj_set_ext_click_area(app_sw, 16);
        if (boost_app_ble_enabled()) lv_obj_add_state(app_sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(app_sw, qr_swipe_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(app_sw, qr_swipe_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(app_sw, qr_toggle_app_cb, LV_EVENT_VALUE_CHANGED, NULL);
    } else {
        lv_obj_t *qr = lv_qrcode_create(s_qr_overlay);
        lv_qrcode_set_size(qr, 320);
        lv_qrcode_set_dark_color(qr, lv_color_black());
        lv_qrcode_set_light_color(qr, lv_color_white());
        char payload[64];
        snprintf(payload, sizeof(payload), "WIFI:T:WPA;S:%s;P:%s;;", ap.ap_ssid, BOOST_AP_PASSWORD);
        lv_qrcode_update(qr, payload, (uint32_t)strlen(payload));
        /* Shifted up 24 px so the swipe hint has room underneath. */
        lv_obj_align(qr, LV_ALIGN_CENTER, 0, -24);

        lv_obj_t *label = lv_label_create(s_qr_overlay);
        if (ap.sta_connected) {
            char txt[64];
            snprintf(txt, sizeof(txt), "%s\n%s", ap.ap_ssid, ap.sta_ip);
            lv_label_set_text(label, txt);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -56);
        } else {
            lv_label_set_text(label, ap.ap_ssid);
            lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -68);
        }
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);

    }

    lv_obj_t *hint = lv_label_create(s_qr_overlay);
    lv_label_set_text(hint, LV_SYMBOL_LEFT " swipe " LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x9a9a9a), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_24, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -14);

    for (int i = 0; i < 2; ++i) {
        lv_obj_t *dot = lv_obj_create(s_qr_overlay);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            (i == (s_qr_toggles_shown ? 1 : 0)) ? lv_color_white() : lv_color_hex(0x5a5a5a), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, i == 0 ? -14 : 14, 18);
    }

    s_qr_active = true;
    /* Pause GIF playback so its direct panel push cannot overwrite the QR. */
    boost_gauge_media_pause();
    ESP_LOGI(TAG, "%s shown for AP %s", s_qr_toggles_shown ? "toggles" : "QR", ap.ap_ssid);
}

static void show_page(boost_page_id_t page)
{
    if (page == BOOST_PAGE_TPMS && !s_tpms_built && s_page_root[BOOST_PAGE_TPMS] != NULL) {
        boost_tpms_ui_create(s_page_root[BOOST_PAGE_TPMS]);
        s_tpms_built = true;
    }
    s_active = page;
    for (int i = 0; i < 2; ++i) {
        if (s_page_root[i] != NULL) {
            lv_obj_set_flag(s_page_root[i], LV_OBJ_FLAG_HIDDEN, i != (int)page);
        }
    }
    /* Page replacement is a full-face operation. Make that contract explicit
     * for the partial-refresh adapter so no pixels from the hidden page survive
     * until one of the destination page's narrow live regions changes. */
    if (s_screen != NULL) lv_obj_invalidate(s_screen);
}

static void hide_qr(void)
{
    if (s_qr_overlay != NULL) {
        lv_obj_delete(s_qr_overlay);
        s_qr_overlay = NULL;
    }
    s_qr_active = false;
    s_qr_toggles_shown = false;
    s_qr_press_tracking = false;
    /* Resume GIF playback (direct panel push) now that the overlay is gone. */
    boost_gauge_media_resume();
}

static void qr_click_cb(lv_event_t *event)
{
    (void)event;
    hide_qr();
}

/* Deferred-toggle plumbing: the BLE side effects (NVS write, NimBLE mount,
 * host start, task creation) each block for tens to hundreds of ms. Running
 * them inside the switch's VALUE_CHANGED callback stalled the LVGL task long
 * enough to black the panel and drop the gesture (observed on hardware), so
 * the callback only records the request; a one-shot lv_timer applies it after
 * the current LVGL cycle finishes rendering. */
static int32_t s_qr_toggle_req = -1;   /* 0=app off 1=app on 2=obd off 3=obd on */

static void qr_toggle_apply_cb(lv_timer_t *timer)
{
    const int32_t req = s_qr_toggle_req;
    s_qr_toggle_req = -1;
    lv_timer_del(timer);
    if (req < 0) return;
    if (req <= 1) {
        boost_app_ble_set_enabled(req == 1);
    } else {
        /* Persist through the theme store first (NVS "tpms_ble"), then drive
         * the live central - the exact order the web and BLE config routes
         * use. Calling boost_obd_set_enabled() alone flipped RAM only, so an
         * OBD2 link enabled from the panel vanished at the next reboot. */
        boost_theme_set_tpms_ble(req == 3);
        boost_obd_set_enabled(req == 3);
    }
}

static void qr_toggle_request(int32_t req)
{
    s_qr_toggle_req = req;
    lv_timer_t *t = lv_timer_create(qr_toggle_apply_cb, 0, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* A drag that starts on a switch is OWNED by the switch (the overlay's
 * PRESSING tracker never sees it), so a horizontal swipe beginning on the
 * toggle flips pages only if we classify it here. If the finger excursion
 * since PRESSED exceeds the page-swipe threshold, the gesture is a swipe:
 * flip the page and swallow the toggle (LV_EVENT_VALUE_CHANGED is suppressed
 * via s_qr_swipe_suppress, checked by the toggle callbacks). */
static bool s_qr_swipe_suppress;

static void qr_swipe_press_cb(lv_event_t *event)
{
    (void)event;
    /* A press STARTED on a switch: the overlay never saw PRESSED, so seed the
     * shared drag tracker HERE. From then on the overlay's PRESSING handler
     * (which receives events once the finger leaves the switch) measures the
     * excursion from the true touch-down point and flips the page. */
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;
    if (indev != NULL) { lv_indev_get_point(indev, &p); s_qr_press_x = p.x; }
    s_qr_press_tracking = true;
    s_qr_swipe_suppress = false;
}

static void qr_swipe_release_cb(lv_event_t *event)
{
    (void)event;
    /* Released back inside the switch without ever crossing the threshold:
     * clear the flag so a legitimate tap-toggle still fires. If a flip DID
     * happen mid-drag the switch was deleted with the overlay, so this cb
     * never runs for that case. */
    s_qr_swipe_suppress = false;
}

static void qr_toggle_obd_cb(lv_event_t *event)
{
    if (s_qr_swipe_suppress) { s_qr_swipe_suppress = false; return; }
    lv_obj_t *sw = lv_event_get_target(event);
    qr_toggle_request(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 3 : 2);
}

static void qr_toggle_app_cb(lv_event_t *event)
{
    if (s_qr_swipe_suppress) { s_qr_swipe_suppress = false; return; }
    lv_obj_t *sw = lv_event_get_target(event);
    qr_toggle_request(lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0);
}

/* Two-page overlay carousel with WRAPAROUND: a swipe of at least SWIPE_MIN_PX
 * in either direction flips to the other page, from either page. A fresh tap
 * (no drag) still dismisses. The overlay is torn down and rebuilt on a flip -
 * show_qr() early-returns while s_qr_active is set (caught by the sim:
 * setting the flag before show_qr() silently kept the old page). */
static void qr_flip_to(bool toggles)
{
    lv_obj_delete(s_qr_overlay);
    s_qr_overlay = NULL;
    s_qr_active = false;
    s_qr_toggles_shown = toggles;
    s_qr_press_tracking = false;
    boost_gauge_media_pause();   /* show_qr pauses again; keep state */
    show_qr();
}

static void qr_pressing_cb(lv_event_t *event)
{
    (void)event;
    if (!s_qr_active) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (indev == NULL) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (!s_qr_press_tracking) {
        s_qr_press_tracking = true;
        s_qr_press_x = p.x;
        return;
    }
    if (s_qr_press_x - p.x >= SWIPE_MIN_PX || p.x - s_qr_press_x >= SWIPE_MIN_PX) {
        qr_flip_to(!s_qr_toggles_shown);
    }
}

static void qr_hold_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (s_qr_active) return;

    bool holding = false;
#ifdef ESP_PLATFORM
    if (boost_display_touch_point_count() >= 2) {
        holding = true;
        s_two_finger_seen = true;
    }
#endif

    if (holding) {
        if (s_qr_hold_start_ms == 0) {
            s_qr_hold_start_ms = lv_tick_get();
        } else if (lv_tick_elaps(s_qr_hold_start_ms) >= QR_HOLD_MS) {
            s_qr_hold_start_ms = 0;
            show_qr();
        }
    } else {
        s_qr_hold_start_ms = 0;
    }
}

static void apply_theme_delta(int direction)
{
    if (s_active != BOOST_PAGE_BOOST || media_active()) return;
    const size_t count = boost_theme_count();
    const boost_theme_t *current = boost_theme_default();
#ifdef ESP_PLATFORM
    current = boost_model_active_theme();
#endif
    size_t index = 0;
    for (; index < count; ++index) {
        const boost_theme_t *candidate = boost_theme_at(index);
        if (candidate != NULL && current != NULL && strcmp(candidate->id, current->id) == 0) break;
    }
    if (index == count) return;
    size_t next = direction > 0 ? (index + 1u) % count : (index + count - 1u) % count;
    const boost_theme_t *theme = boost_theme_at(next);
    if (theme == NULL) return;
#ifdef ESP_PLATFORM
    if (boost_model_set_active_theme(theme->id) != ESP_OK) return;
    boost_gauge_apply_theme(boost_model_active_theme());
#else
    boost_gauge_apply_theme(theme);
#endif
}

static void update_press_motion(lv_indev_t *indev)
{
    if (!s_press_active || indev == NULL || indev != s_press_indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    const int32_t dx = point.x - s_start.x;
    const int32_t dy = point.y - s_start.y;
    if (abs_i32(dx) > abs_i32(s_max_dx)) s_max_dx = dx;
    if (abs_i32(dy) > abs_i32(s_max_dy)) s_max_dy = dy;
}

static void finish_press(bool released)
{
    const int32_t ax = abs_i32(s_max_dx);
    const int32_t ay = abs_i32(s_max_dy);
    const bool act = released && !s_hold_fired && !media_active() && !s_two_finger_seen;

    if (act && ax < TAP_SLOP_PX && ay < TAP_SLOP_PX) {
        if (s_active == BOOST_PAGE_BOOST) boost_gauge_reset_peak();
    } else if (act && ax >= SWIPE_MIN_PX && (int64_t)ax * 4 >= (int64_t)ay * 5) {
        if (s_active == BOOST_PAGE_BOOST && s_max_dx < 0) {
            show_page(BOOST_PAGE_TPMS);
        } else if (s_active == BOOST_PAGE_TPMS && s_max_dx > 0) {
            show_page(BOOST_PAGE_BOOST);
        }
    } else if (act && s_active == BOOST_PAGE_BOOST &&
               ay >= SWIPE_MIN_PX && (int64_t)ay * 4 >= (int64_t)ax * 5) {
        apply_theme_delta(s_max_dy < 0 ? 1 : -1);
    }

    ESP_LOGI(TAG, "%s dx=%ld dy=%ld hold=%d page=%d",
             released ? "released" : "press_lost",
             (long)s_max_dx, (long)s_max_dy, s_hold_fired, (int)s_active);
    s_press_active = false;
    s_hold_fired = false;
    s_press_indev = NULL;
}

/* Pointer-device events are independent of the hit-tested object. A new gauge
 * child or a transient target change can therefore no longer prevent the 1 s
 * hold from starting or firing. */
static void boost_page_indev_event(lv_event_t *event)
{
    if (event == NULL) return;
    if (s_qr_active) return;
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(event);
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        /* The press is tracked even while a GIF owns the screen so the 1 s
         * hold-to-dim still works over media; tap/theme/page actions stay
         * GIF-suppressed in finish_press() and apply_theme_delta(). */
        s_press_indev = indev;
        lv_indev_get_point(indev, &s_start);
        s_max_dx = s_max_dy = 0;
        s_press_active = true;
        s_hold_fired = false;
        s_two_finger_seen = false;
#ifdef ESP_PLATFORM
        s_lvgl_press_us = esp_timer_get_time();
        boost_touch_timing_t touch;
        boost_display_get_touch_timing(&touch);
        ESP_LOGI(TAG, "pressed x=%ld y=%ld hold=%dms contact_to_lvgl=%lldus irq_to_lvgl=%lldus",
                 (long)s_start.x, (long)s_start.y, HOLD_DIM_MS,
                 (long long)(s_lvgl_press_us - touch.contact_down_us),
                 (long long)(s_lvgl_press_us - touch.irq_us));
#else
        ESP_LOGI(TAG, "pressed x=%ld y=%ld hold=%dms",
                 (long)s_start.x, (long)s_start.y, HOLD_DIM_MS);
#endif
    } else if (code == LV_EVENT_LONG_PRESSED && s_press_active &&
               indev == s_press_indev && !s_hold_fired) {
        /* A second finger turns the hold into the two-finger QR gesture, not a
         * dim toggle - suppress the 1 s hold so the QR hold never dims first. */
#ifdef ESP_PLATFORM
        if (boost_display_touch_point_count() >= 2) return;
#endif
        update_press_motion(indev);
        s_hold_fired = true;
        boost_brightness_toggle_max_min_locked();
#ifdef ESP_PLATFORM
        boost_touch_timing_t touch;
        boost_display_get_touch_timing(&touch);
        const int64_t now_us = esp_timer_get_time();
        ESP_LOGI(TAG, "hold fired threshold=%dms lvgl_elapsed=%lldus contact_elapsed=%lldus irq_elapsed=%lldus",
                 HOLD_DIM_MS, (long long)(now_us - s_lvgl_press_us),
                 (long long)(now_us - touch.contact_down_us),
                 (long long)(now_us - touch.irq_us));
#else
        ESP_LOGI(TAG, "hold fired at %dms", HOLD_DIM_MS);
#endif
    } else if (code == LV_EVENT_RELEASED && s_press_active && indev == s_press_indev) {
        update_press_motion(indev);
        finish_press(true);
    }
}

static void boost_page_handle_event(lv_event_t *event)
{
    if (event == NULL) return;
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSING) {
        update_press_motion(lv_event_get_indev(event));
    } else if (code == LV_EVENT_PRESS_LOST && s_press_active) {
        finish_press(false);
    }
}

void boost_page_create(void)
{
    s_press_active = false;
    s_hold_fired = false;
    s_press_indev = NULL;
    s_qr_overlay = NULL;
    s_qr_active = false;
    s_two_finger_seen = false;
    s_qr_hold_start_ms = 0;
    s_qr_toggles_shown = false;
    s_qr_press_tracking = false;
    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, PAGE_SIZE, PAGE_SIZE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    /* The coordinator owns gesture classification; no object may start an LVGL
     * scroll that suppresses the pointer indev's long-press event. */
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_PRESS_LOST, NULL);

    int pointer_count = 0;
    for (lv_indev_t *indev = lv_indev_get_next(NULL); indev != NULL;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) continue;
        lv_indev_set_long_press_time(indev, HOLD_DIM_MS);
        lv_indev_add_event_cb(indev, boost_page_indev_event, LV_EVENT_PRESSED, NULL);
        lv_indev_add_event_cb(indev, boost_page_indev_event, LV_EVENT_LONG_PRESSED, NULL);
        lv_indev_add_event_cb(indev, boost_page_indev_event, LV_EVENT_RELEASED, NULL);
        pointer_count++;
    }
    if (pointer_count == 0) ESP_LOGW(TAG, "no pointer indev; hold-to-dim unavailable");
    else ESP_LOGI(TAG, "registered %d pointer indev, hold=%dms", pointer_count, HOLD_DIM_MS);

    for (int i = 0; i < 2; ++i) {
        s_page_root[i] = lv_obj_create(s_screen);
        lv_obj_remove_style_all(s_page_root[i]);
        lv_obj_set_size(s_page_root[i], PAGE_SIZE, PAGE_SIZE);
        lv_obj_set_pos(s_page_root[i], 0, 0);
        lv_obj_clear_flag(s_page_root[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    boost_gauge_create_in(s_page_root[BOOST_PAGE_BOOST]);
    /* TPMS page is lazy-built on first show: its ~30-object tree would
     * otherwise be traversed every LVGL tick even while hidden, adding
     * measurable overhead to the 16 ms gauge path. */
    s_tpms_built = false;
    lv_timer_create(qr_hold_timer_cb, QR_POLL_MS, NULL);
    show_page(BOOST_PAGE_BOOST);
}

void boost_page_update(const boost_sample_t *sample)
{
    if (s_qr_active) return;
    if (s_active == BOOST_PAGE_BOOST) boost_gauge_update(sample);
}

void boost_page_update_tpms(const boost_tpms_snapshot_t *snapshot)
{
    if (s_qr_active) return;
    if (s_active == BOOST_PAGE_TPMS) boost_tpms_ui_update(snapshot);
}

boost_page_id_t boost_page_active(void) { return s_active; }

void boost_page_show(boost_page_id_t page)
{
    if (page <= BOOST_PAGE_TPMS) show_page(page);
}

bool boost_page_qr_active(void) { return s_qr_active; }
bool boost_page_qr_toggles(void) { return s_qr_toggles_shown; }

void boost_page_qr_show(void)
{
    s_two_finger_seen = false;   /* hold path already finished in the sim */
    show_qr();
}

void boost_page_qr_swipe_left(void)
{
    /* Drives the same teardown-and-rebuild the real swipe handler performs
     * (show_qr() early-returns while the overlay is active). With wraparound
     * both directions flip to the other page. */
    if (!s_qr_active) return;
    qr_flip_to(!s_qr_toggles_shown);
}

void boost_page_qr_swipe_right(void)
{
    if (!s_qr_active) return;
    qr_flip_to(!s_qr_toggles_shown);
}

void boost_page_qr_dismiss(void) { hide_qr(); }

void boost_page_qr_tap_switch(int row)
{
    if (!s_qr_active || !s_qr_toggles_shown || s_qr_overlay == NULL) return;
    /* Find the Nth switch (class check) anywhere under the overlay: rows are
     * inert cards; only lv_switch objects toggle. */
    int seen = 0;
    uint32_t n = lv_obj_get_child_count(s_qr_overlay);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *ch = lv_obj_get_child(s_qr_overlay, i);
        if (ch == NULL || !lv_obj_is_valid(ch)) continue;
        if (lv_obj_check_type(ch, &lv_switch_class)) {
            if (seen == row) { lv_obj_send_event(ch, LV_EVENT_VALUE_CHANGED, NULL); return; }
            seen++;
        }
        /* switches live inside row cards - descend one level */
        uint32_t m = lv_obj_get_child_count(ch);
        for (uint32_t j = 0; j < m; ++j) {
            lv_obj_t *g = lv_obj_get_child(ch, j);
            if (g != NULL && lv_obj_is_valid(g) && lv_obj_check_type(g, &lv_switch_class)) {
                if (seen == row) { lv_obj_send_event(g, LV_EVENT_VALUE_CHANGED, NULL); return; }
                seen++;
            }
        }
    }
}

int boost_page_qr_pending_toggle(void) { return (int)s_qr_toggle_req; }

