#include "boost_gauge.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "boost_media_store.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#include "lvgl.h"
#include "boost_brightness.h"
#ifdef ESP_PLATFORM
#include "boost_model.h"
#include "boost_display.h"
#endif
#if LV_USE_GIF
#include "libs/gif/lv_gif.h"
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Visual system — "Pit Lane Night"
 * -------------------------------
 * Subject: turbo boost on a round AMOLED, glanceable from a dark cabin.
 * Job: signed PSI at a glance; vacuum vs boost read as two climates.
 *
 * Palette
 *   VOID   #050608  panel black (AMOLED off-pixel)
 *   GHOST  #1A1D24  track / wells
 *   STEEL  #6B7280  tick labels
 *   ICE    #E8ECF2  primary readout
 *   TEAL   #2EE6C5  vacuum (signature cool)
 *   AMBER  #FFB020  boost
 *   FLARE  #FF3B30  overboost / peak stress
 *
 * Signature: dual-climate arc — teal below 0 psi, amber above,
 * flare past the overboost threshold — with a hard zero notch at top.
 */

#define COLOR_VOID   0x000000  /* pure black AMOLED face (active) */
#define COLOR_GHOST  0x1A1D24  /* standby gray face — swap FACE_BG back to this to revert */
#define COLOR_STEEL  0x6B7280
#define COLOR_ICE    0xE8ECF2
#define COLOR_TEAL   0x2EE6C5
#define COLOR_AMBER  0xFFB020
#define COLOR_FLARE  0xFF3B30
#define COLOR_MUTED  0x3A3F4A
/* Active face fill. Gray standby: set FACE_BG to COLOR_GHOST. */
#define FACE_BG      COLOR_VOID

/* Gauge arc sweeps 270° from 135° to 405°/45° (classic auto face). The zero
 * notch is user-positioned while vacuum and boost scale independently. */
#define DEFAULT_PSI_MIN       (-15.0f)
#define DEFAULT_PSI_MAX       (10.0f)
#define DEFAULT_PSI_OVERBOOST (8.0f)
#define DEFAULT_ZERO_ANGLE    236.25f
#define ARC_START     135
#define ARC_END       45
#define ARC_RANGE     270

#define DISP_SIZE     466
/* Nominal outer arc edge stays 2 px inside the 466 px AMOLED face. */
#define ARC_DIAMETER  462
#define ARC_WIDTH     45
#define ZERO_LINE_W   20
/* Zero-side hide gap — boost tuned via sim pixels (3.8 clean; 4.0 margin). */
#define ZERO_GAP_VAC_DEG   3.6f
#define ZERO_GAP_BOOST_DEG 4.00f
#define TICK_FONT     (&lv_font_montserrat_20)
/* Original label placement: inside the arc's inner edge. */
#define TICK_RADIUS   160.0f
/* Fixed decimal anchors keep the fractional pair stationary; integer digits
 * grow only to the left when boost reaches two figures. */
#define VALUE_SLOT_WIDTH   26
#define VALUE_SLOT_HEIGHT  64
#define VALUE_SIGN_X       (-69)
#define VALUE_TENS_X       (-43)
#define VALUE_ONES_X       (-17)
#define VALUE_DECIMAL_X    8
#define VALUE_TENTHS_X     30
#define HOLD_DIM_MS   2000
#define WELL_SIZE     DISP_SIZE

static const char *TAG = "boost_gauge";

static lv_obj_t *s_arc_track;
static lv_obj_t *s_arc_value_canvas;
static lv_obj_t *s_well;
static lv_obj_t *s_zero_notch;
static lv_obj_t *s_value_sign_label;
static lv_obj_t *s_value_tens_label;
static lv_obj_t *s_value_ones_label;
static lv_obj_t *s_value_decimal_label;
static lv_obj_t *s_value_tenths_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_peak_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_zone_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_tick_labels[5];
#if LV_USE_GIF
static lv_obj_t *s_media_gif;
static lv_image_dsc_t s_media_dsc;
#endif

static float s_display_psi;
static float s_peak_psi;
static bool s_ui_ready;
static bool s_hold_dim_fired;
static uint32_t s_press_start_ms;
static char s_theme_id[BOOST_THEME_ID_MAX];
static float s_psi_min = DEFAULT_PSI_MIN;
static float s_psi_max = DEFAULT_PSI_MAX;
static float s_psi_overboost = DEFAULT_PSI_OVERBOOST;
static float s_zero_angle = DEFAULT_ZERO_ANGLE;
static float s_tick_psi[5];

static float psi_to_angle(float psi);
static void load_range_from_config(void)
{
#ifdef ESP_PLATFORM
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    s_psi_min = cfg.psi_min;
    s_psi_max = cfg.psi_max;
    s_psi_overboost = cfg.psi_overboost;
    s_zero_angle = cfg.zero_angle;
#else
    s_psi_min = DEFAULT_PSI_MIN;
    s_psi_max = DEFAULT_PSI_MAX;
    s_psi_overboost = DEFAULT_PSI_OVERBOOST;
    s_zero_angle = DEFAULT_ZERO_ANGLE;
#endif
}

static bool midpoint_is_clear(float psi)
{
    const float rad = psi_to_angle(psi) * (float)M_PI / 180.0f;
    const float overboost_rad = psi_to_angle(s_psi_overboost) * (float)M_PI / 180.0f;
    const float dx = TICK_RADIUS * (cosf(rad) - cosf(overboost_rad));
    const float dy = TICK_RADIUS * (sinf(rad) - sinf(overboost_rad));
    /* A 20 px font plus 8 px breathing room prevents adjacent labels touching. */
    return dx * dx + dy * dy >= 28.0f * 28.0f;
}

static void compute_tick_psis(void)
{
    s_tick_psi[0] = s_psi_min;
    s_tick_psi[1] = 0.0f;
    const float midpoint = s_psi_max * 0.5f;
    s_tick_psi[2] = midpoint_is_clear(midpoint) ? midpoint : NAN;
    s_tick_psi[3] = s_psi_overboost;
    s_tick_psi[4] = s_psi_max;
}

static void format_tick_text(char *buf, size_t len, float psi)
{
    if (fabsf(psi) < 0.05f) {
        snprintf(buf, len, "0");
        return;
    }
    const float rounded = roundf(psi);
    if (fabsf(psi - rounded) < 0.05f) {
        snprintf(buf, len, "%d", (int)rounded);
    } else {
        snprintf(buf, len, "%.1f", (double)psi);
    }
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

static const boost_theme_t *active_theme(void)
{
#ifdef ESP_PLATFORM
    return boost_model_active_theme();
#else
    static const boost_theme_t host_theme = {
        .id = "pit-lane",
        .name = "Pit Lane",
        .face = 0x000000,
        .track = 0x3A3F4A,
        .text = 0xE8ECF2,
        .muted = 0x6B7280,
        .vacuum = 0x2EE6C5,
        .boost = 0xFFB020,
        .overboost = 0xFF3B30,
        .zero = 0xE8ECF2,
        .brightness_high = 100,
        .brightness_low = 12,
    };
    return &host_theme;
#endif
}

#if LV_USE_GIF
static void set_gauge_hidden(bool hidden)
{
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (child != s_media_gif) lv_obj_set_flag(child, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

static void destroy_media_gif(void)
{
    if (s_media_gif != NULL) {
        lv_obj_delete(s_media_gif);
        s_media_gif = NULL;
    }
    memset(&s_media_dsc, 0, sizeof(s_media_dsc));
    set_gauge_hidden(false);
}

static bool load_media_gif_locked(void)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    if (boost_media_store_map(&data, &size, &width, &height) != ESP_OK) return false;
    set_gauge_hidden(true);
    s_media_gif = lv_gif_create(lv_screen_active());
    if (s_media_gif == NULL) {
        boost_media_store_unmap();
        set_gauge_hidden(false);
        return false;
    }
    s_media_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_media_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    s_media_dsc.header.w = width;
    s_media_dsc.header.h = height;
    s_media_dsc.data_size = size;
    s_media_dsc.data = data;
    lv_obj_set_size(s_media_gif, DISP_SIZE, DISP_SIZE);
    lv_obj_set_style_bg_color(s_media_gif, c(COLOR_VOID), 0);
    lv_obj_set_style_bg_opa(s_media_gif, LV_OPA_COVER, 0);
    lv_gif_set_color_format(s_media_gif, LV_COLOR_FORMAT_RGB565);
    lv_image_set_inner_align(s_media_gif, LV_IMAGE_ALIGN_CENTER);
    lv_gif_set_src(s_media_gif, &s_media_dsc);
    if (!lv_gif_is_loaded(s_media_gif)) {
        lv_obj_delete(s_media_gif);
        s_media_gif = NULL;
        memset(&s_media_dsc, 0, sizeof(s_media_dsc));
        boost_media_store_unmap();
        set_gauge_hidden(false);
        return false;
    }
    lv_obj_center(s_media_gif);
    lv_obj_move_foreground(s_media_gif);
    lv_obj_clear_flag(s_media_gif, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return true;
}
#endif

/* Map PSI onto the 270° face with user-positioned zero: LVGL 0° = east,
 * clockwise. Vacuum [psiMin, 0] → [135, zeroAngle]; boost [0, psiMax] →
 * [zeroAngle, 405]. */
static float psi_to_angle(float psi)
{
    psi = clampf(psi, s_psi_min, s_psi_max);
    if (psi < 0.0f) {
        const float span = 0.0f - s_psi_min;
        const float t = (span > 0.0f) ? (psi - s_psi_min) / span : 1.0f;
        return (float)ARC_START + t * (s_zero_angle - (float)ARC_START);
    }
    const float span = s_psi_max;
    const float t = (span > 0.0f) ? psi / span : 0.0f;
    return s_zero_angle + t * ((float)ARC_START + (float)ARC_RANGE - s_zero_angle);
}
static lv_color_t color_for_psi(const boost_theme_t *theme, float psi);

static void value_arc_angles(float psi, float *start, float *end)
{
    const float zero_a = psi_to_angle(0.0f);
    if (psi >= 0.0f) {
        *start = zero_a + ZERO_GAP_BOOST_DEG;
        *end = fmaxf(psi_to_angle(psi), *start);
    } else {
        *start = fminf(psi_to_angle(psi), zero_a - ZERO_GAP_VAC_DEG);
        *end = zero_a - ZERO_GAP_VAC_DEG;
    }
}

static void draw_value_arc(lv_event_t *event)
{
    const float psi = *(const float *)lv_event_get_user_data(event);
    float start;
    float end;
    value_arc_angles(psi, &start, &end);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color_for_psi(active_theme(), psi);
    dsc.width = ARC_WIDTH;
    dsc.start_angle = start;
    dsc.end_angle = end;
    dsc.center.x = DISP_SIZE / 2;
    dsc.center.y = DISP_SIZE / 2;
    dsc.radius = ARC_DIAMETER / 2;
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = true;
    lv_draw_arc(lv_event_get_layer(event), &dsc);
}

static void invalidate_value_arc(float start, float end)
{
    /* lv_draw_arc_get_area accepts a contiguous arc through one quarter turn.
     * Split the 240°→405° boost span at quarter boundaries; otherwise LVGL
     * treats it as a wrapped whole-object invalidation and leaves stale pixels. */
    for (float segment_start = start; segment_start < end;) {
        const float boundary = (floorf(segment_start / 90.0f) + 1.0f) * 90.0f;
        const float segment_end = fminf(end, boundary);
        lv_area_t area;
        lv_draw_arc_get_area(DISP_SIZE / 2, DISP_SIZE / 2, ARC_DIAMETER / 2,
                             segment_start, segment_end, ARC_WIDTH, true, &area);
        lv_obj_invalidate_area(s_arc_value_canvas, &area);
        segment_start = segment_end;
    }
}

static void set_value_arc(float psi)
{
    float old_start;
    float old_end;
    float new_start;
    float new_end;
    value_arc_angles(s_display_psi, &old_start, &old_end);
    value_arc_angles(psi, &new_start, &new_end);

    if ((s_display_psi < 0.0f) != (psi < 0.0f) ||
        !lv_color_eq(color_for_psi(active_theme(), s_display_psi), color_for_psi(active_theme(), psi))) {
        /* Color-zone changes and zero crossings need both complete arcs repainted. */
        invalidate_value_arc(old_start, old_end);
        invalidate_value_arc(new_start, new_end);
    } else if (psi >= 0.0f) {
        invalidate_value_arc(fminf(old_end, new_end), fmaxf(old_end, new_end));
    } else {
        invalidate_value_arc(fminf(old_start, new_start), fmaxf(old_start, new_start));
    }
}

static lv_color_t color_for_psi(const boost_theme_t *theme, float psi)
{
    if (psi >= s_psi_overboost) {
        return c(theme->overboost);
    }
    if (psi >= 0.35f) {
        return c(theme->boost);
    }
    if (psi > -0.35f) {
        return c(theme->text);
    }
    return c(theme->vacuum);
}
static const char *zone_for_psi(float psi)
{
    if (psi >= s_psi_overboost) {
        return "OVER";
    }
    if (psi >= 0.35f) {
        return "BOOST";
    }
    if (psi > -0.35f) {
        return "ATMO";
    }
    return "VAC";
}

static void format_value_slots(char *sign, char *tens, char *ones, char *tenths, float psi)
{
    const int tenths_psi = (int)lroundf(fabsf(psi) * 10.0f);
    const int whole = tenths_psi / 10;
    *sign = psi < -0.05f ? '-' : ' ';
    *tens = whole >= 10 ? (char)('0' + whole / 10) : ' ';
    *ones = (char)('0' + whole % 10);
    *tenths = (char)('0' + tenths_psi % 10);
}
static void reset_peak_ui(void)
{
    boost_sim_reset_peak();
    /* Peak is boost-oriented and starts from the current boost reading. */
    s_peak_psi = fmaxf(s_display_psi, 0.0f);
    ESP_LOGI(TAG, "peak reset");
}

static void on_screen_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    /*
     * Manual hold timing is more reliable than depending solely on
     * LV_EVENT_LONG_PRESSED (BSP touch indev timing can vary).
     */
    if (code == LV_EVENT_PRESSED) {
        s_press_start_ms = lv_tick_get();
        s_hold_dim_fired = false;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        if (!s_hold_dim_fired &&
            lv_tick_elaps(s_press_start_ms) >= HOLD_DIM_MS) {
            s_hold_dim_fired = true;
            boost_brightness_toggle_max_min();
            ESP_LOGI(TAG, "brightness toggle -> %d%%", boost_brightness_get());
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_hold_dim_fired &&
            lv_tick_elaps(s_press_start_ms) < HOLD_DIM_MS) {
            reset_peak_ui();
        }
        s_hold_dim_fired = false;
        return;
    }

    /* Fallback if only LONG_PRESSED is delivered by the port. */
    if (code == LV_EVENT_LONG_PRESSED && !s_hold_dim_fired) {
        s_hold_dim_fired = true;
        boost_brightness_toggle_max_min();
        ESP_LOGI(TAG, "brightness toggle (long_pressed) -> %d%%", boost_brightness_get());
    }
}

static void place_tick_label(int idx, float psi, const char *text)
{
    const boost_theme_t *theme = active_theme();
    const float deg = psi_to_angle(psi);
    const float rad = deg * (float)M_PI / 180.0f;
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;

    lv_obj_t *lab = s_tick_labels[idx];
    if (lab == NULL) {
        lab = lv_label_create(lv_screen_active());
        s_tick_labels[idx] = lab;
        lv_obj_set_style_text_font(lab, TICK_FONT, 0);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, c(theme->muted), 0);
    lv_obj_update_layout(lab);

    const lv_coord_t w = lv_obj_get_width(lab);
    const lv_coord_t h = lv_obj_get_height(lab);
    float r = TICK_RADIUS;
    /* Zero sits under the ice notch — keep its glyph clear of the ring. */
    if (fabsf(psi) < 0.01f) {
        r = TICK_RADIUS - 18.0f;
    }
    const float x = cx + r * cosf(rad) - w * 0.5f;
    const float y = cy + r * sinf(rad) - h * 0.5f;
    lv_obj_set_pos(lab, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
}

static void refresh_zero_notch(void)
{
    if (s_zero_notch == NULL) {
        return;
    }
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

static void refresh_tick_labels(void)
{
    compute_tick_psis();
    for (int i = 0; i < 5; ++i) {
        if (!isfinite(s_tick_psi[i])) {
            if (s_tick_labels[i] != NULL) lv_obj_add_flag(s_tick_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char text[12];
        format_tick_text(text, sizeof(text), s_tick_psi[i]);
        place_tick_label(i, s_tick_psi[i], text);
        lv_obj_remove_flag(s_tick_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *add_value_slot(lv_obj_t *scr, const char *text, int x)
{
    const boost_theme_t *theme = active_theme();
    lv_obj_t *slot = lv_label_create(scr);
    lv_label_set_text(slot, text);
    lv_obj_set_size(slot, VALUE_SLOT_WIDTH, VALUE_SLOT_HEIGHT);
    lv_obj_set_style_text_font(slot, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(slot, c(theme->text), 0);
    lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(slot, LV_ALIGN_CENTER, x, -22);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    return slot;
}

void boost_gauge_create(void)
{
    load_range_from_config();
    const boost_theme_t *theme = active_theme();
    snprintf(s_theme_id, sizeof(s_theme_id), "%s", theme->id);
    lv_obj_t *scr = lv_screen_active();
    /* Remove the BSP/default white style before the first panel flush. */
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, DISP_SIZE, DISP_SIZE);
    lv_obj_set_style_bg_color(scr, c(theme->face), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    /*
     * Gestures:
     *  - short press/release = peak reset
     *  - hold ~2s = brightness max/min toggle
     * Track PRESSED/PRESSING/RELEASED manually for reliable 2s hold.
     */
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_LONG_PRESSED, NULL);

    /* Full-panel opaque face; explicit style avoids inherited white defaults. */
    s_well = lv_obj_create(scr);
    lv_obj_remove_style_all(s_well);
    lv_obj_set_size(s_well, WELL_SIZE, WELL_SIZE);
    lv_obj_center(s_well);
    lv_obj_set_style_radius(s_well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_well, c(theme->face), 0);
    lv_obj_set_style_bg_opa(s_well, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_well, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* Background track. */
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
    lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_track, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_arc_track, LV_OBJ_FLAG_CLICKABLE);

    /* Custom-drawn active arc invalidates only the changed angular wedge. */
    s_arc_value_canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(s_arc_value_canvas);
    lv_obj_set_size(s_arc_value_canvas, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_arc_value_canvas);
    lv_obj_clear_flag(s_arc_value_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_arc_value_canvas, draw_value_arc, LV_EVENT_DRAW_MAIN, &s_display_psi);

    /*
     * Zero mark — thicker radial ice tick that fits inside the gray ring only.
     * Drawn after the value arc so it covers the zero-side fill end.
     */
    s_zero_notch = lv_line_create(scr);
    refresh_zero_notch();
    lv_obj_set_style_line_width(s_zero_notch, ZERO_LINE_W, 0);
    lv_obj_set_style_line_color(s_zero_notch, c(theme->zero), 0);
    lv_obj_set_style_line_rounded(s_zero_notch, true, 0);

    lv_obj_set_style_line_opa(s_zero_notch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_zero_notch, LV_OBJ_FLAG_CLICKABLE);

    refresh_tick_labels();

    /* Center stack lives in the open face inside the 270° arc. */
    s_zone_label = lv_label_create(scr);
    lv_label_set_text(s_zone_label, "ATMO");
    lv_obj_set_style_text_font(s_zone_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zone_label, c(theme->text), 0);
    lv_obj_set_style_text_letter_space(s_zone_label, 0, 0);
    lv_obj_align(s_zone_label, LV_ALIGN_CENTER, 0, -88);
    lv_obj_clear_flag(s_zone_label, LV_OBJ_FLAG_CLICKABLE);

    s_value_sign_label = add_value_slot(scr, " ", VALUE_SIGN_X);
    s_value_tens_label = add_value_slot(scr, " ", VALUE_TENS_X);
    s_value_ones_label = add_value_slot(scr, "0", VALUE_ONES_X);
    s_value_decimal_label = add_value_slot(scr, ".", VALUE_DECIMAL_X);
    s_value_tenths_label = add_value_slot(scr, "0", VALUE_TENTHS_X);

    s_unit_label = lv_label_create(scr);
    lv_label_set_text(s_unit_label, "PSI");
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_unit_label, c(theme->muted), 0);
    lv_obj_set_style_text_letter_space(s_unit_label, 0, 0);
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 26);
    lv_obj_clear_flag(s_unit_label, LV_OBJ_FLAG_CLICKABLE);

    s_peak_label = lv_label_create(scr);
    lv_label_set_text(s_peak_label, "PEAK  0.0");
    lv_obj_set_style_text_font(s_peak_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_peak_label, c(theme->boost), 0);
    lv_obj_align(s_peak_label, LV_ALIGN_CENTER, 0, 54);
    lv_obj_clear_flag(s_peak_label, LV_OBJ_FLAG_CLICKABLE);

    s_mode_label = lv_label_create(scr);
    lv_label_set_text(s_mode_label, "DEMO");
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mode_label, c(theme->muted), 0);
    lv_obj_set_style_text_letter_space(s_mode_label, 0, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 82);

    /* No bottom hint: it can become a persistent strip on partial-flush panels. */
    s_display_psi = 0.0f;
    s_peak_psi = 0.0f;
    s_ui_ready = true;
    ESP_LOGI(TAG, "UI ready (inset arc %dpx, stroke %d)", ARC_DIAMETER, ARC_WIDTH);
}

void boost_gauge_apply_theme(const boost_theme_t *theme)
{
    if (!s_ui_ready || theme == NULL) {
        return;
    }
    snprintf(s_theme_id, sizeof(s_theme_id), "%s", theme->id);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, c(theme->face), 0);
    if (s_well) {
        lv_obj_set_style_bg_color(s_well, c(theme->face), 0);
    }
    if (s_arc_track) {
        lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_MAIN);
        lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_INDICATOR);
    }
    if (s_arc_value_canvas) {
        lv_obj_invalidate(s_arc_value_canvas);
    }
    if (s_zero_notch) {
        lv_obj_set_style_line_color(s_zero_notch, c(theme->zero), 0);
    }
    for (size_t i = 0; i < sizeof(s_tick_labels) / sizeof(s_tick_labels[0]); ++i) {
        if (s_tick_labels[i]) {
            lv_obj_set_style_text_color(s_tick_labels[i], c(theme->muted), 0);
        }
    }
    if (s_unit_label) {
        lv_obj_set_style_text_color(s_unit_label, c(theme->muted), 0);
    }
    if (s_mode_label) {
        lv_obj_set_style_text_color(s_mode_label, c(theme->muted), 0);
    }
    if (s_hint_label) {
        lv_obj_set_style_text_color(s_hint_label, c(theme->track), 0);
    }
}

void boost_gauge_apply_config(void)
{
    if (!s_ui_ready) {
        return;
    }
    load_range_from_config();
    refresh_zero_notch();
    refresh_tick_labels();
    if (s_arc_value_canvas) {
        lv_obj_invalidate(s_arc_value_canvas);
    }
}
bool boost_gauge_media_load(void)
{
#if LV_USE_GIF && defined(ESP_PLATFORM)
    if (boost_display_lock(5000) != ESP_OK) return false;
    destroy_media_gif();
    boost_media_store_unmap();
    const bool loaded = load_media_gif_locked();
    boost_display_unlock();
    return loaded;
#else
    return false;
#endif
}

void boost_gauge_media_delete(void)
{
#if LV_USE_GIF && defined(ESP_PLATFORM)
    if (boost_display_lock(5000) != ESP_OK) return;
    destroy_media_gif();
    boost_display_unlock();
    boost_media_store_unmap();
#endif
}
void boost_gauge_update(const boost_sample_t *sample)
{
    if (!s_ui_ready || sample == NULL) {
        return;
    }
    const boost_theme_t *theme = active_theme();
    set_value_arc(sample->psi);
    s_display_psi = sample->psi;
    s_peak_psi = fmaxf(s_peak_psi, fmaxf(sample->peak_psi, 0.0f));

    const lv_color_t col = color_for_psi(theme, sample->psi);
    const char *zone = zone_for_psi(sample->psi);
    if (strcmp(lv_label_get_text(s_zone_label), zone) != 0) {
        lv_obj_set_style_text_color(s_zone_label, col, 0);
        lv_label_set_text(s_zone_label, zone);
    }
    const lv_color_t value_color = sample->psi >= s_psi_overboost ? c(theme->overboost) : c(theme->text);
    lv_obj_t *value_slots[] = {
        s_value_sign_label, s_value_tens_label, s_value_ones_label,
        s_value_decimal_label, s_value_tenths_label,
    };
    for (size_t i = 0; i < sizeof(value_slots) / sizeof(value_slots[0]); ++i) {
        if (!lv_color_eq(lv_obj_get_style_text_color(value_slots[i], 0), value_color)) {
            lv_obj_set_style_text_color(value_slots[i], value_color, 0);
        }
    }

    char sign[2] = {0};
    char tens[2] = {0};
    char ones[2] = {0};
    char tenths[2] = {0};
    format_value_slots(sign, tens, ones, tenths, sample->psi);
    if (strcmp(lv_label_get_text(s_value_sign_label), sign) != 0) lv_label_set_text(s_value_sign_label, sign);
    if (strcmp(lv_label_get_text(s_value_tens_label), tens) != 0) lv_label_set_text(s_value_tens_label, tens);
    if (strcmp(lv_label_get_text(s_value_ones_label), ones) != 0) lv_label_set_text(s_value_ones_label, ones);
    if (strcmp(lv_label_get_text(s_value_tenths_label), tenths) != 0) lv_label_set_text(s_value_tenths_label, tenths);
    char buf[32];

    snprintf(buf, sizeof(buf), "PEAK  %.1f", (double)s_peak_psi);
    if (strcmp(lv_label_get_text(s_peak_label), buf) != 0) {
        lv_label_set_text(s_peak_label, buf);
    }
    const lv_color_t peak_color = s_peak_psi >= s_psi_overboost ? c(theme->overboost) : c(theme->boost);
    if (!lv_color_eq(lv_obj_get_style_text_color(s_peak_label, 0), peak_color)) {
        lv_obj_set_style_text_color(s_peak_label, peak_color, 0);
    }

    const char *mode = sample->demo ? "DEMO" : "LIVE";
    if (strcmp(lv_label_get_text(s_mode_label), mode) != 0) {
        lv_label_set_text(s_mode_label, mode);
    }
}
