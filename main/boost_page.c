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
#include "esp_app_desc.h"
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
/* Host sim firmware version: the sim does not build boost_model.c, so the
 * toggles page's version readout gets a fixed stand-in (overridable via
 * SIM_FW_VERSION for screenshot tests). */
static const char *sim_fw_version(void)
{
    const char *v = getenv("SIM_FW_VERSION");
    return (v != NULL && v[0] != '\0') ? v : "v0.9.5-sim";
}
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

/* Connections page: three square buttons (2 up, 1 down). Order matches the
 * sim tap hook: 0 = OBD BLE, 1 = APP BLE, 2 = UNITS. */
#define QR_BTN_SIZE 130
static lv_obj_t *s_qr_btn[3];
static lv_obj_t *s_qr_btn_unit_label;

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
static void qr_tap_obd_cb(lv_event_t *event);
static void qr_tap_app_cb(lv_event_t *event);
static void qr_tap_units_cb(lv_event_t *event);
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
     * (the branch a joined STA shows). QR_NO_IP=1 drops the IP so the sim can
     * also verify the AP-only layout. Host-only; never compiled into firmware. */
    out->sta_connected = getenv("QR_NO_IP") == NULL;
    if (out->sta_connected) strlcpy(out->sta_ip, "192.168.4.2", sizeof(out->sta_ip));
#endif
}

/* One square toggle button: rounded corners, thin border, and a glow + status
 * LED when on. `accent` is the on colour (green for links, cyan for units). */
static lv_obj_t *qr_make_square(lv_obj_t *parent, int x, int y, bool active, uint32_t accent)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, QR_BTN_SIZE, QR_BTN_SIZE);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (active) {
        lv_obj_set_style_bg_color(b, lv_color_hex(0x14201C), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
        lv_obj_set_style_shadow_color(b, lv_color_hex(accent), 0);
        lv_obj_set_style_shadow_width(b, 22, 0);
        lv_obj_set_style_shadow_opa(b, 130, 0);
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(0x14161a), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x3a4048), 0);
    }
    /* Status LED, top-right: a second on/off cue beyond the glow. */
    lv_obj_t *dot = lv_obj_create(b);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(active ? accent : 0x3a4048), 0);
    lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static void qr_square_set_text(lv_obj_t *b, const char *primary, const char *secondary)
{
    lv_obj_t *p = lv_label_create(b);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(p, primary);
    lv_obj_set_style_text_color(p, lv_color_white(), 0);
    lv_obj_set_style_text_font(p, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(p, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(p, QR_BTN_SIZE - 12);
    lv_obj_align(p, LV_ALIGN_CENTER, 0, secondary != NULL ? -16 : 0);
    if (secondary != NULL) {
        lv_obj_t *s = lv_label_create(b);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_label_set_text(s, secondary);
        lv_obj_set_style_text_color(s, lv_color_hex(0xB6C0CC), 0);
        lv_obj_set_style_text_font(s, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(s, QR_BTN_SIZE - 12);
        lv_obj_align(s, LV_ALIGN_CENTER, 0, 22);
    }
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

    for (int i = 0; i < 3; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;

    if (s_qr_toggles_shown) {
        lv_obj_t *title = lv_label_create(s_qr_overlay);
        lv_label_set_text(title, "Connections");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

        /* Three square buttons, 2 up + 1 down. Each is one big tappable square
         * so a mistap on the label cannot fall through to the overlay and
         * dismiss the screen; the shared swipe press/release trackers let a
         * drag that starts on a button still flip pages. */
        s_qr_btn[0] = qr_make_square(s_qr_overlay, 52, 100, boost_obd_enabled(), 0x62D6A5);
        qr_square_set_text(s_qr_btn[0], "OBD BLE", boost_obd_enabled() ? "ON" : "OFF");
        lv_obj_add_event_cb(s_qr_btn[0], qr_swipe_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_qr_btn[0], qr_swipe_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_qr_btn[0], qr_tap_obd_cb, LV_EVENT_CLICKED, NULL);

        s_qr_btn[1] = qr_make_square(s_qr_overlay, PAGE_SIZE - 52 - QR_BTN_SIZE, 100,
                                     boost_app_ble_enabled(), 0x62D6A5);
        qr_square_set_text(s_qr_btn[1], "APP BLE", boost_app_ble_enabled() ? "ON" : "OFF");
        lv_obj_add_event_cb(s_qr_btn[1], qr_swipe_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_qr_btn[1], qr_swipe_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_qr_btn[1], qr_tap_app_cb, LV_EVENT_CLICKED, NULL);

        /* The units button's second line is the current selection; tapping it
         * cycles PSI -> bar -> kPa. */
        s_qr_btn[2] = qr_make_square(s_qr_overlay, (PAGE_SIZE - QR_BTN_SIZE) / 2, 256,
                                     true, 0x4DD2FF);
        qr_square_set_text(s_qr_btn[2], "UNITS", boost_units_label(boost_theme_pressure_unit()));
        s_qr_btn_unit_label =
            lv_obj_get_child(s_qr_btn[2], lv_obj_get_child_count(s_qr_btn[2]) - 1);
        lv_obj_add_event_cb(s_qr_btn[2], qr_swipe_press_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_qr_btn[2], qr_swipe_release_cb, LV_EVENT_RELEASED, NULL);
        lv_obj_add_event_cb(s_qr_btn[2], qr_tap_units_cb, LV_EVENT_CLICKED, NULL);

        /* Firmware version readout, bottom-anchored above the swipe hint -
         * the same slot pattern the QR page uses for the SSID/IP lines. The
         * version comes from the app description (git describe at build
         * time), so what is on glass is what is actually running. Muted so
         * it reads as metadata, not as a fourth button. */
        lv_obj_t *fw = lv_label_create(s_qr_overlay);
#ifdef ESP_PLATFORM
        const esp_app_desc_t *app_desc = esp_app_get_description();
        lv_label_set_text(fw, (app_desc != NULL) ? app_desc->version : "unknown");
#else
        lv_label_set_text(fw, sim_fw_version());
#endif
        lv_obj_set_style_text_color(fw, lv_color_hex(0x9a9a9a), 0);
        lv_obj_set_style_text_font(fw, &lv_font_montserrat_24, 0);
        lv_obj_align(fw, LV_ALIGN_BOTTOM_MID, 0, -48);
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

        /* AP name below the QR in its own slot; the IP line (when the gauge is
         * on the network) is a SEPARATE label stacked under it. The old single
         * two-line label was bottom-anchored, so the IP line grew the box
         * UPWARD into the QR (label y 356..409 vs the QR box ending at 368).
         * Splitting keeps the name at the same place with and without the IP. */
        lv_obj_t *label = lv_label_create(s_qr_overlay);
        lv_label_set_text(label, ap.ap_ssid);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -68);
        if (ap.sta_connected) {
            lv_obj_t *ip = lv_label_create(s_qr_overlay);
            lv_label_set_text(ip, ap.sta_ip);
            lv_obj_set_style_text_color(ip, lv_color_white(), 0);
            lv_obj_set_style_text_font(ip, &lv_font_montserrat_24, 0);
            lv_obj_align(ip, LV_ALIGN_BOTTOM_MID, 0, -41);
        }

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
    for (int i = 0; i < 3; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;
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
static int32_t s_qr_toggle_req = -1;   /* 0=app off 1=app on 2=obd off 3=obd on 4=cycle unit */

static void qr_toggle_apply_cb(lv_timer_t *timer)
{
    const int32_t req = s_qr_toggle_req;
    s_qr_toggle_req = -1;
    lv_timer_del(timer);
    if (req < 0) return;
    if (req == 4) {
        const boost_unit_t cur = boost_theme_pressure_unit();
        const boost_unit_t next = (cur == BOOST_UNIT_PSI) ? BOOST_UNIT_BAR
                                : (cur == BOOST_UNIT_BAR) ? BOOST_UNIT_KPA
                                                          : BOOST_UNIT_PSI;
        boost_theme_set_pressure_unit(next);
        if (s_qr_btn_unit_label != NULL) {
            lv_label_set_text(s_qr_btn_unit_label, boost_units_label(next));
        }
        /* Unit marks/readouts are baked at scene build, so rebuild the face. */
#ifdef ESP_PLATFORM
        boost_gauge_apply_theme(boost_model_active_theme());
#else
        boost_gauge_apply_theme(boost_theme_default());
#endif
        return;
    }
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

static void qr_tap_obd_cb(lv_event_t *event)
{
    (void)event;
    if (s_qr_swipe_suppress) { s_qr_swipe_suppress = false; return; }
    qr_toggle_request(boost_obd_enabled() ? 2 : 3);
}

static void qr_tap_app_cb(lv_event_t *event)
{
    (void)event;
    if (s_qr_swipe_suppress) { s_qr_swipe_suppress = false; return; }
    qr_toggle_request(boost_app_ble_enabled() ? 0 : 1);
}

static void qr_tap_units_cb(lv_event_t *event)
{
    (void)event;
    if (s_qr_swipe_suppress) { s_qr_swipe_suppress = false; return; }
    qr_toggle_request(4);
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
    for (int i = 0; i < 3; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;
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
    if (row < 0 || row >= 3 || s_qr_btn[row] == NULL) return;
    lv_obj_send_event(s_qr_btn[row], LV_EVENT_CLICKED, NULL);
}

int boost_page_qr_pending_toggle(void) { return (int)s_qr_toggle_req; }

