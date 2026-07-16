#include "boost_gauge.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

#include "lvgl.h"

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

#define COLOR_VOID   0x050608
#define COLOR_GHOST  0x1A1D24
#define COLOR_STEEL  0x6B7280
#define COLOR_ICE    0xE8ECF2
#define COLOR_TEAL   0x2EE6C5
#define COLOR_AMBER  0xFFB020
#define COLOR_FLARE  0xFF3B30
#define COLOR_MUTED  0x3A3F4A

/* Gauge range (psi). Arc sweeps 270° from 135° to 45° (classic auto face). */
#define PSI_MIN       (-15.0f)
#define PSI_MAX       (25.0f)
#define PSI_OVERBOOST (18.0f)
#define ARC_START     135
#define ARC_END       45
#define ARC_RANGE     270

#define DISP_SIZE     466
#define ARC_DIAMETER  400
#define ARC_WIDTH     27   /* ~1.5× the original 18px track */
#define TICK_FONT     (&lv_font_montserrat_16)
#define TICK_RADIUS   152.0f  /* inside the thicker arc */
#define NOTCH_RADIUS  200.0f

static const char *TAG = "boost_gauge";

static lv_obj_t *s_arc_track;
static lv_obj_t *s_arc_value;
static lv_obj_t *s_zero_notch;
static lv_obj_t *s_value_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_peak_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_zone_label;
static lv_obj_t *s_tick_labels[5];

static float s_display_psi;
static float s_peak_psi;
static bool s_ui_ready;

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

/* Map PSI onto the 270° face: LVGL 0° = east, clockwise. */
static float psi_to_angle(float psi)
{
    psi = clampf(psi, PSI_MIN, PSI_MAX);
    const float t = (psi - PSI_MIN) / (PSI_MAX - PSI_MIN);
    return (float)ARC_START + t * (float)ARC_RANGE;
}

/*
 * Fill from the zero notch toward the current PSI.
 * Vacuum grows counter-clockwise from 0; boost clockwise from 0.
 */
static void set_value_arc(float psi)
{
    const float zero_a = psi_to_angle(0.0f);
    const float val_a = psi_to_angle(psi);
    float start;
    float end;

    if (psi >= 0.0f) {
        start = zero_a;
        end = val_a;
    } else {
        start = val_a;
        end = zero_a;
    }

    /* Collapse near zero so the indicator doesn't leave a stub. */
    if (fabsf(end - start) < 0.4f) {
        start = zero_a;
        end = zero_a;
    }

    lv_arc_set_angles(s_arc_value, start, end);
}

static lv_color_t color_for_psi(float psi)
{
    if (psi >= PSI_OVERBOOST) {
        return c(COLOR_FLARE);
    }
    if (psi >= 0.35f) {
        return c(COLOR_AMBER);
    }
    if (psi > -0.35f) {
        return c(COLOR_ICE); /* ATMO: neutral, not boost-amber */
    }
    return c(COLOR_TEAL);
}

static const char *zone_for_psi(float psi)
{
    if (psi >= PSI_OVERBOOST) {
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

static void format_signed_psi(char *buf, size_t n, float psi)
{
    /* One decimal; force leading sign so vacuum and boost share width. */
    snprintf(buf, n, "%+.1f", (double)psi);
}

static void on_screen_clicked(lv_event_t *e)
{
    (void)e;
    boost_sim_reset_peak();
    /* Peak is boost-oriented: never display a vacuum peak. */
    s_peak_psi = s_display_psi > 0.0f ? s_display_psi : 0.0f;
    if (s_peak_label) {
        char buf[32];
        snprintf(buf, sizeof(buf), "PEAK  %+.1f", (double)s_peak_psi);
        lv_label_set_text(s_peak_label, buf);
        lv_obj_set_style_text_color(s_peak_label, c(COLOR_AMBER), 0);
    }
    ESP_LOGI(TAG, "peak reset");
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
    lv_obj_update_layout(lab);

    const lv_coord_t w = lv_obj_get_width(lab);
    const lv_coord_t h = lv_obj_get_height(lab);
    /* Pull slightly inward so thicker arc never covers the digits. */
    float r = TICK_RADIUS;
    /* Zero sits under the ice notch — nudge further inward. */
    if (fabsf(psi) < 0.01f) {
        r = TICK_RADIUS - 14.0f;
    }
    const float x = cx + r * cosf(rad) - w * 0.5f;
    const float y = cy + r * sinf(rad) - h * 0.5f;
    lv_obj_set_pos(lab, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
    s_tick_labels[idx] = lab;
}

void boost_gauge_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, c(COLOR_VOID), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_clicked, LV_EVENT_CLICKED, NULL);

    /* Soft vignette well under the arc. */
    lv_obj_t *well = lv_obj_create(scr);
    lv_obj_set_size(well, 424, 424);
    lv_obj_center(well);
    lv_obj_set_style_radius(well, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(well, c(COLOR_GHOST), 0);
    lv_obj_set_style_bg_opa(well, LV_OPA_40, 0);
    lv_obj_set_style_border_width(well, 0, 0);
    lv_obj_clear_flag(well, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_set_style_arc_color(s_arc_track, c(COLOR_MUTED), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_track, c(COLOR_MUTED), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_track, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_arc_track, LV_OBJ_FLAG_CLICKABLE);

    /* Value arc — angles set so vacuum/boost both grow from the zero notch. */
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

    /* Zero mark — radial ice tick (always perpendicular to the arc). */
    s_zero_notch = lv_line_create(scr);
    {
        static lv_point_precise_t zero_pts[2];
        const float deg = psi_to_angle(0.0f);
        const float rad = deg * (float)M_PI / 180.0f;
        const float cx = DISP_SIZE * 0.5f;
        const float cy = DISP_SIZE * 0.5f;
        const float r_outer = (float)ARC_DIAMETER * 0.5f + 2.0f;
        const float r_inner = r_outer - (float)ARC_WIDTH - 6.0f;
        zero_pts[0].x = cx + r_inner * cosf(rad);
        zero_pts[0].y = cy + r_inner * sinf(rad);
        zero_pts[1].x = cx + r_outer * cosf(rad);
        zero_pts[1].y = cy + r_outer * sinf(rad);
        lv_line_set_points(s_zero_notch, zero_pts, 2);
    }
    lv_obj_set_style_line_width(s_zero_notch, 5, 0);
    lv_obj_set_style_line_color(s_zero_notch, c(COLOR_ICE), 0);
    lv_obj_set_style_line_rounded(s_zero_notch, true, 0);
    lv_obj_set_style_line_opa(s_zero_notch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_zero_notch, LV_OBJ_FLAG_CLICKABLE);

    add_tick_label(0, -15.0f, "-15");
    add_tick_label(1, 0.0f, "0");
    add_tick_label(2, 10.0f, "10");
    add_tick_label(3, 18.0f, "18");
    add_tick_label(4, 25.0f, "25");

    /*
     * Center stack lives in the open face (inside the 270° arc).
     * Keeps DEMO / zone off the top track where the arc runs.
     */
    s_zone_label = lv_label_create(scr);
    lv_label_set_text(s_zone_label, "ATMO");
    lv_obj_set_style_text_font(s_zone_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zone_label, c(COLOR_ICE), 0);
    lv_obj_set_style_text_letter_space(s_zone_label, 4, 0);
    lv_obj_align(s_zone_label, LV_ALIGN_CENTER, 0, -78);

    s_value_label = lv_label_create(scr);
    lv_label_set_text(s_value_label, "+0.0");
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_value_label, c(COLOR_ICE), 0);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, -18);

    s_unit_label = lv_label_create(scr);
    lv_label_set_text(s_unit_label, "PSI");
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_unit_label, c(COLOR_STEEL), 0);
    lv_obj_set_style_text_letter_space(s_unit_label, 4, 0);
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 28);

    s_peak_label = lv_label_create(scr);
    lv_label_set_text(s_peak_label, "PEAK  +0.0");
    lv_obj_set_style_text_font(s_peak_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_peak_label, c(COLOR_AMBER), 0);
    lv_obj_align(s_peak_label, LV_ALIGN_CENTER, 0, 56);

    s_mode_label = lv_label_create(scr);
    lv_label_set_text(s_mode_label, "DEMO");
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mode_label, c(COLOR_STEEL), 0);
    lv_obj_set_style_text_letter_space(s_mode_label, 3, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 84);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "TAP  RESET  PEAK");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(hint, c(COLOR_MUTED), 0);
    lv_obj_set_style_text_letter_space(hint, 2, 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -28);

    s_display_psi = 0.0f;
    s_peak_psi = 0.0f;
    s_ui_ready = true;
    ESP_LOGI(TAG, "UI ready (arc %dpx wide, ticks 16pt)", ARC_WIDTH);
}

void boost_gauge_update(const boost_sample_t *sample)
{
    if (!s_ui_ready || sample == NULL) {
        return;
    }

    s_display_psi = sample->psi;
    /* Peak is boost-oriented for the readout. */
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
    lv_obj_set_style_text_color(
        s_peak_label,
        s_peak_psi >= PSI_OVERBOOST ? c(COLOR_FLARE) : c(COLOR_AMBER),
        0);

    lv_label_set_text(s_mode_label, sample->demo ? "DEMO" : "LIVE");
}
