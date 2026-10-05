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
 * toggles page's version readout needs a value from somewhere. It used to be a
 * literal ("v0.9.5-sim") that outlived v0.9.5 by four releases. Read the
 * canonical version.txt - the same file ESP-IDF bakes into the device - so the
 * sim and the panel can never disagree. SIM_FW_VERSION still wins for
 * screenshot tests. */
static const char *sim_fw_version(void)
{
    static char buf[32];
    const char *v = getenv("SIM_FW_VERSION");
    if (v != NULL && v[0] != '\0') {
        return v;
    }
    if (buf[0] == '\0') {
        FILE *f = fopen("version.txt", "r");
        if (f == NULL) {
            f = fopen("../../version.txt", "r"); /* when run from sim/build/ */
        }
        if (f != NULL) {
            if (fgets(buf, sizeof(buf), f) != NULL) {
                size_t n = strlen(buf);
                while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
                    buf[--n] = '\0';
                }
            }
            fclose(f);
        }
        if (buf[0] == '\0') {
            snprintf(buf, sizeof(buf), "unknown");
        }
    }
    return buf;
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
/* Overlay page: 0 = QR, 1 = Connections, 2 = Units. A horizontal swipe steps
 * one page in either direction with wraparound; a fresh tap still dismisses the
 * whole overlay. Only meaningful while s_qr_active - hide_qr() resets it to the
 * QR page, so every fresh open starts there. */
#define QR_PAGE_QR          0
#define QR_PAGE_CONNECTIONS 1
#define QR_PAGE_UNITS       2
#define QR_PAGE_COUNT       3
static int32_t s_qr_page;
static int32_t s_qr_press_x;
static int32_t s_qr_press_y;
static bool s_qr_press_tracking;
/* One-shot latch: set when the tracked drag crosses SWIPE_MIN_PX so the shared
 * classifier fires exactly one action per gesture (the finger keeps generating
 * PRESSING after the classification). */
static bool s_qr_drag_classified;
/* Set when that classification happened, so the CLICKED that LVGL delivers
 * after the release (RELEASED is sent first, then CLICKED) is swallowed - a
 * drag is not a tap. Cleared when the next press seeds a fresh gesture. */
static bool s_qr_swipe_suppress;
/* Set as soon as a gesture moves past TAP_SLOP_PX, whether or not it was long
 * enough to classify. A moved gesture is a drag, not a tap, so its release
 * must NOT dismiss the overlay: without this a 20-47 px background flick
 * (under SWIPE_MIN_PX, so it sets no other latch) fell straight through to
 * qr_click_cb and closed the whole settings overlay - the "swiping takes me
 * out of the settings" board report. */
static bool s_qr_drag_seen;

/* One centred PAIR of square buttons per toggle page, matching the sim tap
 * hook's PAGE-LOCAL row: page 1 = 0 OBD BLE / 1 APP BLE, page 2 = 0 UNITS /
 * 1 REL/ABS. The pair is centred as a group with a gap smaller than the outer
 * margins, so the two read as one control cluster (the old 2-up-1-down triangle
 * is gone). */
#define QR_BTN_SIZE  130
#define QR_PAIR_GAP  40
#define QR_PAIR_X0   ((PAGE_SIZE - 2 * QR_BTN_SIZE - QR_PAIR_GAP) / 2)
#define QR_PAIR_X1   (QR_PAIR_X0 + QR_BTN_SIZE + QR_PAIR_GAP)
#define QR_PAIR_Y    150
#define QR_BTN_COUNT 2
static lv_obj_t *s_qr_btn[QR_BTN_COUNT];
static lv_obj_t *s_qr_btn_unit_label;
static lv_obj_t *s_qr_btn_ref_label;

static bool media_active(void);

static int32_t abs_i32(int32_t x) { return x < 0 ? -x : x; }

static bool media_active(void)
{
    return boost_gauge_media_active();
}

static void show_page(boost_page_id_t page);
static void hide_qr(void);
static void qr_click_cb(lv_event_t *event);
static void qr_press_cb(lv_event_t *event);
static void qr_pressing_cb(lv_event_t *event);
static void qr_release_cb(lv_event_t *event);
static void qr_goto_page(int32_t page);
static void qr_step(int32_t dir);
static void qr_gesture_begin(int32_t x, int32_t y);
static void qr_gesture_end(void);
static void qr_gesture_reset(void);
static void qr_gesture_rebuild(void);
static void qr_drag_update(int32_t x, int32_t y);
static void qr_tap_obd_cb(lv_event_t *event);
static void qr_tap_app_cb(lv_event_t *event);
static void qr_tap_units_cb(lv_event_t *event);
static void qr_tap_ref_cb(lv_event_t *event);
static void apply_theme_delta(int direction);
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
    /* Gesture bookkeeping is registered HERE, once, for every square. A press
     * that starts on a square is OWNED by the square, so the overlay never sees
     * its PRESSED/PRESSING/RELEASED: the square must feed the shared classifier
     * itself (seeding the gesture from the true touch-down point) and must clear
     * it at release, or the origin and the one-shot latch leak into the next
     * gesture. Registering it in the factory keeps all four call sites identical. */
    lv_obj_add_event_cb(b, qr_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(b, qr_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(b, qr_release_cb, LV_EVENT_RELEASED, NULL);
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
 * Page 0 is the QR; a horizontal swipe steps to the Connections then the Units
 * page (s_qr_page), wrapping. One shared widget tree - the host sim screenshots
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
    /* Gesture state is deliberately NOT touched here. show_qr() is also the
     * mid-gesture rebuild path (qr_goto_page() deletes and re-creates the
     * overlay to step a page), so clearing the latches here armed the
     * classifier again for the remainder of the SAME drag (a slow 300 px swipe
     * could step two pages) and dropped the "not a tap" latch the release needs
     * to swallow its CLICKED. The state is reset at the gesture boundaries and
     * when the overlay closes instead - see boost_page_create()/hide_qr(). */
    lv_obj_add_event_cb(s_qr_overlay, qr_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_qr_overlay, qr_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_qr_overlay, qr_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_qr_overlay, qr_release_cb, LV_EVENT_RELEASED, NULL);

    /* Page indicator + swipe hint render on EVERY page so the three-page
     * structure is visible from any of them. The dots are real objects, not
     * font glyphs (the -/X glyph pair read as mystery buttons); the active
     * page's dot is lit. Pure indicators - not clickable, a tap on one falls
     * through to the overlay's dismiss. */

    for (int i = 0; i < QR_BTN_COUNT; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;
    s_qr_btn_ref_label = NULL;

    if (s_qr_page == QR_PAGE_CONNECTIONS || s_qr_page == QR_PAGE_UNITS) {
        const bool units_page = (s_qr_page == QR_PAGE_UNITS);
        lv_obj_t *title = lv_label_create(s_qr_overlay);
        lv_label_set_text(title, units_page ? "Units" : "Connections");
        lv_obj_set_style_text_color(title, lv_color_white(), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

        /* One centred pair. Each button is one big tappable square so a mistap
         * on the label cannot fall through to the overlay and dismiss the
         * screen. A press starting on a button is OWNED by the button, so the
         * overlay never sees its PRESSING: PRESSED seeds the shared drag
         * tracker and PRESSING (delivered to the pressed button while the finger
         * stays inside it) runs the same classifier, letting a drag that starts
         * on a button step pages / change theme. */
        if (units_page) {
            /* The units button's second line is the current selection; tapping
             * it cycles PSI -> bar -> kPa. */
            s_qr_btn[0] = qr_make_square(s_qr_overlay, QR_PAIR_X0, QR_PAIR_Y, true, 0x4DD2FF);
            qr_square_set_text(s_qr_btn[0], "UNITS",
                               boost_units_label(boost_theme_pressure_unit()));
            s_qr_btn_unit_label =
                lv_obj_get_child(s_qr_btn[0], lv_obj_get_child_count(s_qr_btn[0]) - 1);
            lv_obj_add_event_cb(s_qr_btn[0], qr_tap_units_cb, LV_EVENT_CLICKED, NULL);

            /* Display reference. The primary line names the pair of modes and
             * the second line the one in force; `active` lights the square (and
             * its LED) only while ABSOLUTE is selected, so the on/off cue still
             * means something. The pair is abbreviated "REL/ABS" because the
             * button's label box is QR_BTN_SIZE - 12 = 118 px: parsing LVGL's own
             * montserrat_24 advance table, "GAUGE/ABS" is 148.7 px and
             * "PRESSURE" 133.1 px (both overflow), while "REL/ABS" is 106.8 px -
             * the same width class as the proven "OBD BLE" (113.1 px). The state
             * line runs at montserrat_20, so it carries the full words: "RELATIVE"
             * measures 99.9 px and "ABSOLUTE" 111.8 px. */
            s_qr_btn[1] = qr_make_square(s_qr_overlay, QR_PAIR_X1, QR_PAIR_Y,
                                         boost_theme_pressure_absolute(), 0xC792EA);
            qr_square_set_text(s_qr_btn[1], "REL/ABS",
                               boost_theme_pressure_absolute() ? "ABSOLUTE" : "RELATIVE");
            s_qr_btn_ref_label =
                lv_obj_get_child(s_qr_btn[1], lv_obj_get_child_count(s_qr_btn[1]) - 1);
            lv_obj_add_event_cb(s_qr_btn[1], qr_tap_ref_cb, LV_EVENT_CLICKED, NULL);
        } else {
            s_qr_btn[0] = qr_make_square(s_qr_overlay, QR_PAIR_X0, QR_PAIR_Y,
                                         boost_obd_enabled(), 0x62D6A5);
            qr_square_set_text(s_qr_btn[0], "OBD BLE", boost_obd_enabled() ? "ON" : "OFF");
            lv_obj_add_event_cb(s_qr_btn[0], qr_tap_obd_cb, LV_EVENT_CLICKED, NULL);

            s_qr_btn[1] = qr_make_square(s_qr_overlay, QR_PAIR_X1, QR_PAIR_Y,
                                         boost_app_ble_enabled(), 0x62D6A5);
            qr_square_set_text(s_qr_btn[1], "APP BLE", boost_app_ble_enabled() ? "ON" : "OFF");
            lv_obj_add_event_cb(s_qr_btn[1], qr_tap_app_cb, LV_EVENT_CLICKED, NULL);
        }

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

    for (int i = 0; i < QR_PAGE_COUNT; ++i) {
        lv_obj_t *dot = lv_obj_create(s_qr_overlay);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 14, 14);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(dot,
            (i == s_qr_page) ? lv_color_white() : lv_color_hex(0x5a5a5a), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_align(dot, LV_ALIGN_TOP_MID, (i - (QR_PAGE_COUNT / 2)) * 18, 18);
    }

    s_qr_active = true;
    /* Pause GIF playback so its direct panel push cannot overwrite the QR. */
    boost_gauge_media_pause();
    ESP_LOGI(TAG, "overlay page %d shown for AP %s", (int)s_qr_page, ap.ap_ssid);
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
    s_qr_page = QR_PAGE_QR;
    qr_gesture_reset();
    for (int i = 0; i < QR_BTN_COUNT; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;
    s_qr_btn_ref_label = NULL;
    /* Resume GIF playback (direct panel push) now that the overlay is gone. */
    boost_gauge_media_resume();
}

/* A theme/unit rebuild runs boost_gauge_apply_theme() -> build_scene(), and
 * while a GIF is loaded that build hides every screen child except the media
 * object (set_gauge_hidden(true)) and re-raises the media to the foreground.
 * That buries a still-active overlay behind the paused GIF (tapping UNITS on
 * the connections page was the repro). Re-assert the overlay's visibility and
 * foreground order after any rebuild that can happen with the overlay up. */
static void qr_reassert_overlay(void)
{
    if (!s_qr_active || s_qr_overlay == NULL) return;
    lv_obj_clear_flag(s_qr_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_qr_overlay);
    lv_obj_invalidate(s_qr_overlay);
}

static void qr_click_cb(lv_event_t *event)
{
    (void)event;
    /* A drag is not a tap, by either route: the gesture classified as a swipe
     * (suppress), or it moved past the tap slop without reaching the swipe
     * threshold (drag_seen). Both must swallow the CLICKED LVGL delivers after
     * the release, or the overlay is dismissed mid-gesture - the 20-47 px
     * background flick that used to close the whole settings overlay. */
    if (s_qr_swipe_suppress || s_qr_drag_seen) { qr_gesture_reset(); return; }
    hide_qr();
}

/* Deferred-toggle plumbing: the BLE side effects (NVS write, NimBLE mount,
 * host start, task creation) each block for tens to hundreds of ms. Running
 * them inside the switch's VALUE_CHANGED callback stalled the LVGL task long
 * enough to black the panel and drop the gesture (observed on hardware), so
 * the callback only records the request; a one-shot lv_timer applies it after
 * the current LVGL cycle finishes rendering. */
static int32_t s_qr_toggle_req = -1;   /* 0=app off 1=app on 2=obd off 3=obd on 4=cycle unit
                                        * 5=toggle the display reference */

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
        /* The rebuild re-raises a loaded GIF over every other screen child;
         * keep the overlay the user is looking at on top. */
        qr_reassert_overlay();
        return;
    }
    if (req == 5) {
        boost_theme_set_pressure_absolute(!boost_theme_pressure_absolute());
        /* Readouts take the reference live, but the dial numerals are baked at
         * scene build, so rebuild the face exactly like the unit cycle does.
         * The square's own lit state is a style, so rebuild the overlay page
         * too (its secondary line and glow both follow the mode). */
#ifdef ESP_PLATFORM
        boost_gauge_apply_theme(boost_model_active_theme());
#else
        boost_gauge_apply_theme(boost_theme_default());
#endif
        qr_goto_page(s_qr_page);
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
    /* The square's glow, its status LED and its ON/OFF line are all baked by
     * show_qr() from boost_app_ble_enabled()/boost_obd_enabled(), and nothing
     * else ever repaints the overlay (the 16 ms gauge path is gated on
     * s_qr_active). Applying the toggle without this rebuild left the button
     * looking dead until the next page step - the "BLE buttons don't respond to
     * taps" report. Same in-place turnover the reference toggle does, and the
     * same page, so the tap never appears to navigate. */
    qr_goto_page(s_qr_page);
}

static void qr_toggle_request(int32_t req)
{
    s_qr_toggle_req = req;
    lv_timer_t *t = lv_timer_create(qr_toggle_apply_cb, 0, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void qr_tap_obd_cb(lv_event_t *event)
{
    (void)event;
    /* Every CLICKED ends its gesture. A drag is not a tap by either route - it
     * classified as a swipe, or it only moved past the 12 px tap slop - so the
     * switch must not toggle: a 20-47 px flick that began on a switch used to
     * flip the setting (same rule as the overlay background in qr_click_cb). */
    const bool suppress = s_qr_swipe_suppress || s_qr_drag_seen;
    qr_gesture_reset();
    if (suppress) return;
    qr_toggle_request(boost_obd_enabled() ? 2 : 3);
}

static void qr_tap_app_cb(lv_event_t *event)
{
    (void)event;
    const bool suppress = s_qr_swipe_suppress || s_qr_drag_seen;
    qr_gesture_reset();
    if (suppress) return;
    qr_toggle_request(boost_app_ble_enabled() ? 0 : 1);
}

static void qr_tap_units_cb(lv_event_t *event)
{
    (void)event;
    const bool suppress = s_qr_swipe_suppress || s_qr_drag_seen;
    qr_gesture_reset();
    if (suppress) return;
    qr_toggle_request(4);
}

static void qr_tap_ref_cb(lv_event_t *event)
{
    (void)event;
    const bool suppress = s_qr_swipe_suppress || s_qr_drag_seen;
    qr_gesture_reset();
    if (suppress) return;
    qr_toggle_request(5);
}

/* Three-page overlay carousel with WRAPAROUND. A fresh tap (no drag) still
 * dismisses. The overlay is torn down and rebuilt on a step - show_qr()
 * early-returns while s_qr_active is set (caught by the sim: setting the flag
 * before show_qr() silently kept the old page). */
static void qr_goto_page(int32_t page)
{
    if (page < 0 || page >= QR_PAGE_COUNT) return;
    lv_obj_delete(s_qr_overlay);
    s_qr_overlay = NULL;
    s_qr_active = false;
    s_qr_page = page;
    /* The gesture is STILL IN FLIGHT: this rebuild is what its page step does.
     * Drop only the origin, keep the one-shot classification latch, so the rest
     * of the drag cannot act again (qr_drag_update() consults the latch BEFORE
     * re-seeding, so a slow swipe steps exactly one page). On glass LVGL would
     * also stop dispatching PRESSING at all here - lv_obj_delete() of the
     * pressed overlay runs obj_indev_reset() -> lv_indev_wait_release() - but the
     * state machine must not depend on that. */
    qr_gesture_rebuild();
    boost_gauge_media_pause();   /* show_qr pauses again; keep state */
    show_qr();
}

/* +1 = forward (0 -> 1 -> 2 -> 0), -1 = backward, both wrapping. */
static void qr_step(int32_t dir)
{
    qr_goto_page((s_qr_page + dir + QR_PAGE_COUNT) % QR_PAGE_COUNT);
}

/* One gesture = one touch-down. Every entry point that can start a gesture
 * funnels through qr_gesture_begin() and every release through
 * qr_gesture_end()/qr_gesture_reset(), so neither the origin nor the one-shot
 * latch can survive into the next gesture. That leak WAS the "swiping is
 * broken" defect: the origin was only ever seeded by a PRESSING, and only when
 * s_qr_press_tracking happened to be false. After a tap on a square, or after a
 * vertical (theme) classification, it stayed set - so the next background drag
 * measured its excursion from the PREVIOUS gesture's touch-down point (a 20 px
 * swipe classifying as a 200 px one, i.e. an accidental theme change), and with
 * s_qr_drag_classified still latched every later gesture returned early: the
 * overlay stopped stepping pages until it was reopened. */
static void qr_gesture_begin(int32_t x, int32_t y)
{
    s_qr_press_x = x;
    s_qr_press_y = y;
    s_qr_press_tracking = true;
    s_qr_drag_classified = false;
    s_qr_drag_seen = false;
    s_qr_swipe_suppress = false;
}

/* End of a gesture: drop the origin and the one-shot classification latch so
 * the next gesture cannot inherit either. The two RELEASE-CONSUMED latches
 * (suppress, drag_seen) are deliberately NOT touched here: LVGL delivers
 * CLICKED after RELEASED and that callback reads them to decide whether this
 * gesture was a drag. Clearing them here is exactly how a short drag slipped
 * past qr_click_cb and dismissed the overlay. They are cleared by
 * qr_gesture_reset() from every CLICKED path, and by the next gesture's begin -
 * so a release that produced no CLICKED (finger left the object) cannot leak
 * them either. */
static void qr_gesture_end(void)
{
    s_qr_press_tracking = false;
    s_qr_drag_classified = false;
}

static void qr_gesture_reset(void)
{
    qr_gesture_end();
    s_qr_swipe_suppress = false;
    s_qr_drag_seen = false;
}

/* A mid-gesture scene/overlay rebuild (qr_goto_page) drops the origin, because
 * the rebuilt object cannot receive the rest of the stream, but KEEPS the
 * one-shot classification latch: one gesture = one action, whatever the rebuild
 * does. qr_gesture_end() (a real release) clears both. */
static void qr_gesture_rebuild(void)
{
    s_qr_press_tracking = false;
}

/* The shared gesture classifier. A predominantly horizontal drag of at least
 * SWIPE_MIN_PX steps the overlay page in the DIRECTION OF THE DRAG (dragging
 * left advances, dragging right goes back, both wrapping). A VERTICAL drag does
 * nothing at all - user decision 2026-10-05: the theme is behind an opaque
 * settings cover, so changing it there has no affordance and an accidental
 * diagonal swipe read as the overlay "changing themes" (it used to call
 * apply_theme_delta(); the gauge's own vertical theme swipe on page 0 is
 * unchanged). Every drag still counts as a drag via s_qr_drag_seen, so a
 * vertical flick neither acts nor dismisses. The ratio tests match the gauge's
 * finish_press() classification. The point is passed in rather than read from an
 * indev so the host harness drives THIS function - the production classifier,
 * not a copy of it (the headless sim has no pointer device to synthesise real
 * PRESSING events on). */
static void qr_drag_update(int32_t x, int32_t y)
{
    if (!s_qr_active) return;
    /* One action per gesture. Consulted BEFORE the seeding fallback: a drag
     * whose origin was dropped by a mid-gesture rebuild (qr_goto_page) must not
     * re-seed and act again, which is what keeps a slow swipe to exactly one
     * page if further PRESSING samples arrive. */
    if (s_qr_drag_classified) return;
    if (!s_qr_press_tracking) {
        /* Fallback for a PRESSING whose PRESSED was not delivered: the first
         * sample of the gesture establishes the drag origin. */
        qr_gesture_begin(x, y);
        return;
    }
    const int32_t dx = x - s_qr_press_x;
    const int32_t dy = y - s_qr_press_y;
    const int32_t ax = abs_i32(dx);
    const int32_t ay = abs_i32(dy);
    /* Movement past the tap slop makes this gesture a DRAG, whatever it ends up
     * classifying as: its release must not dismiss the overlay. */
    if (ax >= TAP_SLOP_PX || ay >= TAP_SLOP_PX) s_qr_drag_seen = true;
    if (ax >= SWIPE_MIN_PX && (int64_t)ax * 4 >= (int64_t)ay * 5) {
        s_qr_drag_classified = true;
        s_qr_swipe_suppress = true;
        qr_step(dx < 0 ? 1 : -1);
    }
}

/* All three callbacks run while the overlay (or one of its squares, which owns
 * the press when the finger starts on one) is the pressed object. */
static bool qr_event_point(lv_event_t *event, lv_point_t *p)
{
    lv_indev_t *indev = lv_event_get_indev(event);
    if (indev == NULL) indev = lv_indev_get_act();
    if (indev == NULL) return false;
    lv_indev_get_point(indev, p);
    return true;
}

static void qr_press_cb(lv_event_t *event)
{
    if (!s_qr_active) return;
    lv_point_t p;
    if (!qr_event_point(event, &p)) return;
    qr_gesture_begin(p.x, p.y);
}

static void qr_pressing_cb(lv_event_t *event)
{
    if (!s_qr_active) return;
    lv_point_t p;
    if (!qr_event_point(event, &p)) return;
    qr_drag_update(p.x, p.y);
}

static void qr_release_cb(lv_event_t *event)
{
    (void)event;
    if (!s_qr_active) return;
    qr_gesture_end();
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
    s_qr_page = QR_PAGE_QR;
    qr_gesture_reset();
    for (int i = 0; i < QR_BTN_COUNT; ++i) s_qr_btn[i] = NULL;
    s_qr_btn_unit_label = NULL;
    s_qr_btn_ref_label = NULL;
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

int boost_page_qr_page(void)
{
    return s_qr_active ? (int)s_qr_page : -1;
}

void boost_page_qr_show(void)
{
    s_two_finger_seen = false;   /* hold path already finished in the sim */
    if (s_qr_active) return;     /* show_qr() early-returns too */
    s_qr_page = QR_PAGE_QR;      /* every fresh open starts on the QR page */
    qr_gesture_reset();          /* show_qr() deliberately does not touch it */
    show_qr();
}

void boost_page_qr_show_page(int page)
{
    if (page < 0 || page >= QR_PAGE_COUNT) return;
    s_two_finger_seen = false;
    if (s_qr_active) {
        qr_goto_page(page);
        return;
    }
    s_qr_page = page;
    qr_gesture_reset();
    show_qr();
}

/* --- Gesture injection -------------------------------------------------------
 * The headless sim has no pointer device, so a real PRESSING stream cannot be
 * synthesised as an LVGL event. These drive the SAME functions the overlay's
 * own callbacks call (qr_gesture_begin / qr_drag_update / qr_release_cb /
 * qr_click_cb), so the harness exercises the production classifier and the
 * production state machine - never a copy of either. Model a gesture as: press
 * (touch-down), one or more move samples, release. */
void boost_page_qr_press(int x, int y)
{
    if (!s_qr_active) return;
    qr_gesture_begin(x, y);
}

void boost_page_qr_move(int x, int y)
{
    qr_drag_update(x, y);
}

void boost_page_qr_release(void)
{
    if (!s_qr_active) return;
    qr_release_cb(NULL);   /* RELEASED: drop the origin and the one-shot latch */
    qr_click_cb(NULL);     /* the CLICKED that follows: dismisses only a tap */
}

/* A complete flick: touch-down, ONE move sample to the end point, release. One
 * sample on purpose - the classifier is one-shot per gesture and the hook then
 * models a flick exactly, with no re-sampling after a mid-drag page rebuild. */
void boost_page_qr_drag(int x0, int y0, int x1, int y1)
{
    boost_page_qr_press(x0, y0);
    boost_page_qr_move(x1, y1);
    boost_page_qr_release();
}

/* A flick whose PRESSED was never delivered: the first move sample must
 * establish its own origin. This is the path a background press takes while the
 * tracker still holds the previous gesture's touch-down point. */
void boost_page_qr_drag_unseeded(int x0, int y0, int x1, int y1)
{
    boost_page_qr_move(x0, y0);
    boost_page_qr_move(x1, y1);
    boost_page_qr_release();
}

void boost_page_qr_switch_gesture(int row, int dx, int dy)
{
    if (!s_qr_active || s_qr_page == QR_PAGE_QR) return;
    if (row < 0 || row >= QR_BTN_COUNT || s_qr_btn[row] == NULL) return;
    lv_obj_t *btn = s_qr_btn[row];
    const int32_t page_before = s_qr_page;
    lv_area_t coords;
    lv_obj_get_coords(btn, &coords);
    const int32_t x0 = (coords.x1 + coords.x2) / 2;
    const int32_t y0 = (coords.y1 + coords.y2) / 2;
    /* A press starting on a square is OWNED by that square on glass: PRESSED,
     * PRESSING, RELEASED and CLICKED all go to the square, never to the overlay.
     * Model exactly that - the shared state machine plus the square's OWN tap
     * callback, which is where the switch's drag rules live. */
    qr_gesture_begin(x0, y0);
    qr_drag_update(x0 + dx, y0 + dy);
    qr_release_cb(NULL);
    /* A page-stepping flick deleted this square (and the overlay with it), and
     * glass would then deliver no CLICKED at all. The PAGE is the witness, not
     * the object pointers: lv_obj_delete() frees the overlay's block LAST and
     * show_qr() re-creates the overlay as the very next same-size allocation, so
     * both `s_qr_overlay == overlay_before` and `s_qr_btn[row] == btn` can hold
     * again after a step (and lv_obj_is_valid() only tests membership of the live
     * tree) - which would raise CLICKED on a switch this gesture never touched.
     * A page step always changes s_qr_page. */
    if (s_qr_page == page_before && s_qr_btn[row] == btn) {
        lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
    }
}

void boost_page_qr_swipe_left(void)
{
    /* Forward, through the production classifier - not qr_step() directly. */
    if (!s_qr_active) return;
    boost_page_qr_drag(PAGE_SIZE - 60, PAGE_SIZE / 2, 60, PAGE_SIZE / 2);
}

void boost_page_qr_swipe_right(void)
{
    if (!s_qr_active) return;
    boost_page_qr_drag(60, PAGE_SIZE / 2, PAGE_SIZE - 60, PAGE_SIZE / 2);
}

void boost_page_qr_dismiss(void)
{
    /* A fresh tap: touch-down and release with no movement, which must still
     * dismiss. Routed through the same callbacks the glass path uses so the
     * "a drag is not a tap" rule is exercised rather than bypassed. */
    if (!s_qr_active) return;
    boost_page_qr_press(PAGE_SIZE / 2, PAGE_SIZE / 2);
    boost_page_qr_release();
}

void boost_page_qr_tap_switch(int row)
{
    /* Rows are PAGE-LOCAL: the QR page has no buttons, and each toggle page
     * has exactly QR_BTN_COUNT. */
    if (!s_qr_active || s_qr_page == QR_PAGE_QR || s_qr_overlay == NULL) return;
    if (row < 0 || row >= QR_BTN_COUNT || s_qr_btn[row] == NULL) return;
    lv_obj_send_event(s_qr_btn[row], LV_EVENT_CLICKED, NULL);
}

int boost_page_qr_pending_toggle(void) { return (int)s_qr_toggle_req; }

const char *boost_page_qr_switch_text(int row)
{
    if (s_qr_overlay == NULL || row < 0 || row >= QR_BTN_COUNT) return "";
    lv_obj_t *b = s_qr_btn[row];
    if (b == NULL || lv_obj_get_child_count(b) < 2) return "";
    lv_obj_t *s = lv_obj_get_child(b, lv_obj_get_child_count(b) - 1);
    if (!lv_obj_check_type(s, &lv_label_class)) return "";
    return lv_label_get_text(s);
}

