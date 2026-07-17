#include "boost_gauge.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#ifdef ESP_PLATFORM
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
#define STATE_LOCK() portENTER_CRITICAL(&s_state_mux)
#define STATE_UNLOCK() portEXIT_CRITICAL(&s_state_mux)
#else
#define STATE_LOCK() ((void)0)
#define STATE_UNLOCK() ((void)0)
#endif

#include "lvgl.h"
#include "boost_brightness.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define COLOR_VOID   0x000000
#define COLOR_GHOST  0x1A1D24
#define COLOR_STEEL  0x6B7280
#define COLOR_ICE    0xE8ECF2
#define COLOR_TEAL   0x2EE6C5
#define COLOR_AMBER  0xFFB020
#define COLOR_FLARE  0xFF3B30
#define COLOR_MUTED  0x3A3F4A

#define FACE_BG_DEFAULT COLOR_VOID

#define PSI_MIN       (-15.0f)
#define PSI_MAX       (25.0f)
#define PSI_OVERBOOST (18.0f)
#define ARC_START     135
#define ARC_END       45
#define ARC_RANGE     270

#define DISP_SIZE     466
#define ARC_DIAMETER  452
#define ARC_WIDTH     45
#define ZERO_LINE_W   20
#define ZERO_GAP_VAC_DEG   3.6f
#define ZERO_GAP_BOOST_DEG 4.00f
#define TICK_FONT     (&lv_font_montserrat_20)
#define TICK_RADIUS   160.0f
#define HOLD_DIM_MS   2000
#define WELL_SIZE     DISP_SIZE

static const char *TAG = "boost_gauge";

static lv_obj_t *s_arc_track;
static lv_obj_t *s_arc_value;
static lv_obj_t *s_zero_notch;
static lv_obj_t *s_value_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_tick_labels[5];
static lv_obj_t *s_peak_label;
static lv_obj_t *s_zone_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_face_well;

static float s_display_psi;
static float s_peak_psi;
static bool s_ui_ready;
static bool s_hold_dim_fired;
static uint32_t s_press_start_ms;
static uint32_t s_face_color = FACE_BG_DEFAULT;
static float s_last_applied_psi;

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float psi_to_angle(float psi)
{
    psi = clampf(psi, PSI_MIN, PSI_MAX);
    const float t = (psi - PSI_MIN) / (PSI_MAX - PSI_MIN);
    return (float)ARC_START + t * (float)ARC_RANGE;
}

static void set_value_arc(float psi)
{
    const float zero_a = psi_to_angle(0.0f);
    const float val_a = psi_to_angle(psi);
    float start;
    float end;

    if (psi >= 0.0f) {
        start = zero_a + ZERO_GAP_BOOST_DEG;
        end = val_a;
        if (end < start) end = start;
    } else {
        start = val_a;
        end = zero_a - ZERO_GAP_VAC_DEG;
        if (end < start) end = start;
    }

    if (fabsf(end - start) < 0.4f) {
        start = zero_a;
        end = zero_a;
    }
    lv_arc_set_angles(s_arc_value, start, end);
}

static lv_color_t color_for_psi(float psi)
{
    if (psi >= PSI_OVERBOOST) return c(COLOR_FLARE);
    if (psi >= 0.35f) return c(COLOR_AMBER);
    if (psi > -0.35f) return c(COLOR_ICE);
    return c(COLOR_TEAL);
}

static const char *zone_for_psi(float psi)
{
    if (psi >= PSI_OVERBOOST) return "OVER";
    if (psi >= 0.35f) return "BOOST";
    if (psi > -0.35f) return "ATMO";
    return "VAC";
}

static void format_signed_psi(char *buf, size_t n, float psi)
{
    snprintf(buf, n, "%+.1f", (double)psi);
}

static void reset_peak_ui(void)
{
    boost_sim_reset_peak();
    s_peak_psi = s_display_psi > 0.0f ? s_display_psi : 0.0f;
    if (s_peak_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PEAK  %+.1f", (double)s_peak_psi);
        lv_label_set_text(s_peak_label, buf);
        lv_obj_set_style_text_color(s_peak_label, c(COLOR_AMBER), 0);
    }
    ESP_LOGI(TAG, "peak reset");
}

static void on_screen_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_press_start_ms = lv_tick_get();
        s_hold_dim_fired = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!s_hold_dim_fired && lv_tick_elaps(s_press_start_ms) >= HOLD_DIM_MS) {
            s_hold_dim_fired = true;
            boost_brightness_toggle_max_min();
            ESP_LOGI(TAG, "brightness toggle -> %d%%", boost_brightness_get());
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_hold_dim_fired && lv_tick_elaps(s_press_start_ms) < HOLD_DIM_MS) {
            reset_peak_ui();
        }
        s_hold_dim_fired = false;
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED && !s_hold_dim_fired) {
        s_hold_dim_fired = true;
        boost_brightness_toggle_max_min();
    }
}

static void add_tick_label(int idx, float psi, const char *text)
{
    const float deg = psi_to_angle(psi);
    const float rad = deg * (float)M_PI / 180.0f;
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;

    lv_obj_t *lab = lv_label_create(lv_screen_active());
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_font(lab, TICK_FONT, 0);
    lv_obj_set_style_text_color(lab, c(COLOR_STEEL), 0);
    lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_update_layout(lab);

    const lv_coord_t w = lv_obj_get_width(lab);
    const lv_coord_t h = lv_obj_get_height(lab);
    float r = TICK_RADIUS;
    if (fabsf(psi) < 0.01f) r = TICK_RADIUS - 18.0f;
    const float x = cx + r * cosf(rad) - w * 0.5f;
    const float y = cy + r * sinf(rad) - h * 0.5f;
    lv_obj_set_pos(lab, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
    s_tick_labels[idx] = lab;
}

void boost_gauge_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, c(s_face_color), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_size(scr, DISP_SIZE, DISP_SIZE);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_LONG_PRESSED, NULL);

    s_face_well = lv_obj_create(scr);
    lv_obj_remove_style_all(s_face_well);
    lv_obj_set_size(s_face_well, WELL_SIZE, WELL_SIZE);
    lv_obj_center(s_face_well);
    lv_obj_set_style_radius(s_face_well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_face_well, c(s_face_color), 0);
    lv_obj_set_style_bg_opa(s_face_well, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_face_well, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_arc_track = lv_arc_create(scr);
    lv_obj_set_size(s_arc_track, ARC_DIAMETER, ARC_DIAMETER);
    lv_obj_center(s_arc_track);
    lv_arc_set_rotation(s_arc_track, 0);
    lv_arc_set_bg_angles(s_arc_track, ARC_START, ARC_END);
    lv_arc_set_angles(s_arc_track, ARC_START, ARC_START);
    lv_arc_set_range(s_arc_track, 0, 1000);
    lv_arc_set_value(s_arc_track, 0);
    lv_obj_remove_style(s_arc_track, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc_track, ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_track, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_track, c(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_track, c(COLOR_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_track, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_arc_track, LV_OBJ_FLAG_CLICKABLE);

    s_arc_value = lv_arc_create(scr);
    lv_obj_set_size(s_arc_value, ARC_DIAMETER, ARC_DIAMETER);
    lv_obj_center(s_arc_value);
    lv_arc_set_rotation(s_arc_value, 0);
    lv_arc_set_bg_angles(s_arc_value, ARC_START, ARC_END);
    lv_arc_set_angles(s_arc_value, psi_to_angle(0.0f), psi_to_angle(0.0f));
    lv_obj_remove_style(s_arc_value, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc_value, ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_value, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_value, LV_OPA_0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_value, c(COLOR_TEAL), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_value, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_arc_value, LV_OBJ_FLAG_CLICKABLE);

    s_zero_notch = lv_line_create(scr);
    {
        static lv_point_precise_t zero_pts[2];
        const float deg = psi_to_angle(0.0f);
        const float rad = deg * (float)M_PI / 180.0f;
        const float cx = DISP_SIZE * 0.5f;
        const float cy = DISP_SIZE * 0.5f;
        const float r_outer = (float)ARC_DIAMETER * 0.5f - 1.0f;
        const float r_inner = r_outer - (float)ARC_WIDTH + 1.0f;
        zero_pts[0].x = cx + r_inner * cosf(rad);
        zero_pts[0].y = cy + r_inner * sinf(rad);
        zero_pts[1].x = cx + r_outer * cosf(rad);
        zero_pts[1].y = cy + r_outer * sinf(rad);
        lv_line_set_points(s_zero_notch, zero_pts, 2);
    }
    lv_obj_set_style_line_width(s_zero_notch, ZERO_LINE_W, 0);
    lv_obj_set_style_line_color(s_zero_notch, c(COLOR_ICE), 0);
    lv_obj_set_style_line_rounded(s_zero_notch, true, 0);
    lv_obj_set_style_line_opa(s_zero_notch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_zero_notch, LV_OBJ_FLAG_CLICKABLE);

    add_tick_label(0, -15.0f, "-15");
    add_tick_label(1, 0.0f, "0");
    add_tick_label(2, 10.0f, "10");
    add_tick_label(3, 18.0f, "18");
    add_tick_label(4, 25.0f, "25");

    s_zone_label = lv_label_create(scr);
    lv_label_set_text(s_zone_label, "ATMO");
    lv_obj_set_style_text_font(s_zone_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zone_label, c(COLOR_ICE), 0);
    lv_obj_set_style_text_letter_space(s_zone_label, 4, 0);
    lv_obj_align(s_zone_label, LV_ALIGN_CENTER, 0, -88);
    lv_obj_clear_flag(s_zone_label, LV_OBJ_FLAG_CLICKABLE);

    s_value_label = lv_label_create(scr);
    lv_label_set_text(s_value_label, "+0.0");
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_value_label, c(COLOR_ICE), 0);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -22);
    lv_obj_clear_flag(s_value_label, LV_OBJ_FLAG_CLICKABLE);

    s_unit_label = lv_label_create(scr);
    lv_label_set_text(s_unit_label, "PSI");
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_unit_label, c(COLOR_STEEL), 0);
    lv_obj_set_style_text_letter_space(s_unit_label, 4, 0);
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 26);
    lv_obj_clear_flag(s_unit_label, LV_OBJ_FLAG_CLICKABLE);

    s_peak_label = lv_label_create(scr);
    lv_label_set_text(s_peak_label, "PEAK  +0.0");
    lv_obj_set_style_text_font(s_peak_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_peak_label, c(COLOR_AMBER), 0);
    lv_obj_align(s_peak_label, LV_ALIGN_CENTER, 0, 54);
    lv_obj_clear_flag(s_peak_label, LV_OBJ_FLAG_CLICKABLE);

    s_mode_label = lv_label_create(scr);
    lv_label_set_text(s_mode_label, "DEMO");
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mode_label, c(COLOR_STEEL), 0);
    lv_obj_set_style_text_letter_space(s_mode_label, 3, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 82);
    lv_obj_clear_flag(s_mode_label, LV_OBJ_FLAG_CLICKABLE);

    /* No bottom hint — it was reading as a gray strip on-device. */

    s_display_psi = 0.0f;
    s_peak_psi = 0.0f;
    STATE_LOCK();
    s_last_applied_psi = 0.0f;
    s_ui_ready = true;
    STATE_UNLOCK();
    ESP_LOGI(TAG, "UI ready arc=%d stroke=%d", ARC_DIAMETER, ARC_WIDTH);
}

void boost_gauge_update(const boost_sample_t *sample)
{
    /* Must be called under bsp_display_lock / LVGL lock. */
    if (!s_ui_ready || sample == NULL) {
        return;
    }

    s_display_psi = sample->psi;
    s_peak_psi = sample->peak_psi > 0.0f ? sample->peak_psi : 0.0f;

    set_value_arc(sample->psi);

    const lv_color_t col = color_for_psi(sample->psi);
    lv_obj_set_style_arc_color(s_arc_value, col, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_zone_label, col, 0);
    lv_obj_set_style_text_color(s_value_label, sample->psi >= PSI_OVERBOOST ? c(COLOR_FLARE) : c(COLOR_ICE), 0);

    char buf[32];
    format_signed_psi(buf, sizeof(buf), sample->psi);
    lv_label_set_text(s_value_label, buf);
    lv_label_set_text(s_zone_label, zone_for_psi(sample->psi));

    snprintf(buf, sizeof(buf), "PEAK  %+.1f", (double)s_peak_psi);
    lv_label_set_text(s_peak_label, buf);
    lv_obj_set_style_text_color(s_peak_label, s_peak_psi >= PSI_OVERBOOST ? c(COLOR_FLARE) : c(COLOR_AMBER), 0);
    lv_label_set_text(s_mode_label, sample->demo ? "DEMO" : "LIVE");
    STATE_LOCK();
    s_last_applied_psi = sample->psi;
    STATE_UNLOCK();
}

void boost_gauge_set_theme(uint8_t theme_id)
{
    s_face_color = (theme_id == 1) ? COLOR_GHOST : COLOR_VOID;
    if (!s_ui_ready) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, c(s_face_color), 0);
    if (s_face_well) {
        lv_obj_set_style_bg_color(s_face_well, c(s_face_color), 0);
    }
}

void boost_gauge_reset_peak(void)
{
    if (!s_ui_ready) {
        boost_sim_reset_peak();
        return;
    }
    reset_peak_ui();
}

float boost_gauge_last_psi(void)
{
    STATE_LOCK();
    const float psi = s_last_applied_psi;
    STATE_UNLOCK();
    return psi;
}

bool boost_gauge_is_ready(void)
{
    STATE_LOCK();
    const bool ready = s_ui_ready;
    STATE_UNLOCK();
    return ready;
}
