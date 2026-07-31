#include "boost_tpms_ui.h"

#include <stdio.h>
#include <string.h>

#define TPMS_SIZE 466
#define TPMS_BG 0x080B10
#define TPMS_PANEL 0x11161D
#define TPMS_PANEL_EDGE 0x394552
#define TPMS_HIGHLIGHT 0x8B96A3
#define TPMS_WHITE 0xF2F5F8
#define TPMS_GREEN 0x62D6A5
#define TPMS_RED 0xE8362E
#define TPMS_AMBER 0xFFB020
#define TPMS_OFFLINE 0x5A6573

static lv_obj_t *s_root;
static lv_obj_t *s_tire[4];
static lv_obj_t *s_psi[4];
static lv_obj_t *s_unit[4];
static lv_obj_t *s_status[4];

static lv_color_t rgb(uint32_t value)
{
    return lv_color_hex(value);
}

static void no_interaction(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void label_style(lv_obj_t *obj, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, rgb(color), 0);
    no_interaction(obj);
}

static lv_obj_t *rounded(lv_obj_t *parent, int x, int y, int w, int h,
                         int radius, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_bg_color(obj, rgb(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    no_interaction(obj);
    return obj;
}

static lv_obj_t *line(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    return rounded(parent, x, y, w, h, radius, TPMS_WHITE);
}

static lv_obj_t *highlight_arc(lv_obj_t *parent, int x, int y, int size,
                               int width, lv_opa_t opa)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_remove_style_all(arc);
    lv_obj_set_size(arc, size, size);
    lv_obj_set_pos(arc, x, y);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, rgb(TPMS_HIGHLIGHT), LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, opa, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_opa(arc, opa, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_arc_set_angles(arc, 198, 342);
    no_interaction(arc);
    return arc;
}

static lv_obj_t *glyph_sidewall(lv_obj_t *parent, bool right)
{
    static lv_point_precise_t left_points[] = {
        { 223, 220 }, { 218, 224 }, { 216, 231 }, { 218, 239 }, { 223, 244 }
    };
    static lv_point_precise_t right_points[] = {
        { 243, 220 }, { 248, 224 }, { 250, 231 }, { 248, 239 }, { 243, 244 }
    };
    lv_obj_t *line_obj = lv_line_create(parent);
    lv_line_set_points(line_obj, right ? right_points : left_points, 5);
    lv_obj_set_style_line_width(line_obj, 4, 0);
    lv_obj_set_style_line_color(line_obj, rgb(TPMS_WHITE), 0);
    lv_obj_set_style_line_opa(line_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_line_rounded(line_obj, true, 0);
    no_interaction(line_obj);
    return line_obj;
}


static lv_obj_t *wheel_label(lv_obj_t *parent, int x, int y, bool right,
                             const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    label_style(label, font, color);
    lv_obj_set_width(label, 62);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_align(label, right ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
    return label;
}

void boost_tpms_ui_create(lv_obj_t *parent)
{
    if (parent == NULL) return;
    boost_tpms_ui_delete();

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, TPMS_SIZE, TPMS_SIZE);
    lv_obj_set_style_bg_color(s_root, rgb(TPMS_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    no_interaction(s_root);

    /* Glossy instrument tile: a dark face, steel bezel, soft outer shadow,
     * and two restrained highlight arcs following the rounded top edge. */
    lv_obj_t *panel = rounded(s_root, 18, 18, 430, 430, 40, TPMS_PANEL);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_border_color(panel, rgb(TPMS_PANEL_EDGE), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 18, 0);
    lv_obj_set_style_shadow_spread(panel, 3, 0);
    lv_obj_set_style_shadow_color(panel, rgb(0x000000), 0);
    lv_obj_set_style_shadow_opa(panel, LV_OPA_70, 0);
    lv_obj_t *inner = rounded(s_root, 24, 24, 418, 418, 34, TPMS_PANEL);
    lv_obj_set_style_bg_opa(inner, LV_OPA_20, 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_set_style_border_color(inner, rgb(0x5A6673), 0);
    lv_obj_set_style_border_opa(inner, LV_OPA_40, 0);
    /* A thin reflected strip gives the bezel a restrained glossy top edge. */
    lv_obj_t *shine = rounded(s_root, 58, 37, 350, 2, 1, TPMS_HIGHLIGHT);
    lv_obj_set_style_bg_opa(shine, LV_OPA_30, 0);

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "TPMS");
    label_style(title, &lv_font_montserrat_32, TPMS_WHITE);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 52);

    /* The white H-shaped drivetrain is deliberately behind the four capsules. */
    line(s_root, 126, 145, 214, 10, 5);
    line(s_root, 126, 325, 214, 10, 5);
    line(s_root, 228, 150, 10, 180, 5);
    /* Small center couplers make each axle read as a single mechanical bar. */
    rounded(s_root, 215, 143, 36, 14, 7, TPMS_WHITE);
    rounded(s_root, 215, 323, 36, 14, 7, TPMS_WHITE);

    /* Four status-colored, vertical tire capsules. */
    static const int tire_x[4] = { 106, 326, 106, 326 };
    static const int tire_y[4] = { 112, 112, 293, 293 };
    static const int text_y[4] = { 126, 126, 307, 307 };
    static const int status_y[4] = { 157, 157, 338, 338 };
    for (int i = 0; i < 4; ++i) {
        s_tire[i] = rounded(s_root, tire_x[i], tire_y[i], 34, 74, 17, TPMS_OFFLINE);
        lv_obj_set_style_shadow_width(s_tire[i], 8, 0);
        lv_obj_set_style_shadow_spread(s_tire[i], 1, 0);
        lv_obj_set_style_shadow_color(s_tire[i], rgb(TPMS_OFFLINE), 0);
        lv_obj_set_style_shadow_opa(s_tire[i], LV_OPA_30, 0);

        const bool right = (i & 1) != 0;
        const int label_x = right ? 365 : 36;
        const int unit_x = right ? 365 : 36;
        s_psi[i] = wheel_label(s_root, label_x, text_y[i], right,
                                &lv_font_montserrat_16, TPMS_WHITE);
        s_unit[i] = wheel_label(s_root, unit_x, text_y[i] + 20, right,
                                &lv_font_montserrat_12, TPMS_WHITE);
        s_status[i] = wheel_label(s_root, label_x, status_y[i], right,
                                   &lv_font_montserrat_12, TPMS_OFFLINE);
    }

    /* Center TPMS warning glyph: ring, tire-cross-section arcs, and !. */
    lv_obj_t *ring = rounded(s_root, 202, 202, 62, 62, 31, TPMS_BG);
    lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ring, 7, 0);
    lv_obj_set_style_border_color(ring, rgb(TPMS_WHITE), 0);
    lv_obj_set_style_border_opa(ring, LV_OPA_COVER, 0);
    glyph_sidewall(s_root, false);
    glyph_sidewall(s_root, true);   /* ( ! ) tire-cross-section arcs */
    rounded(s_root, 222, 221, 22, 34, 0, TPMS_PANEL); // clear line overlap before drawing !
    lv_obj_t *mask = lv_obj_get_child(s_root, -1);
    lv_obj_set_style_bg_opa(mask, LV_OPA_COVER, 0); // ! is intentionally above this mask
    rounded(s_root, 230, 215, 6, 29, 3, TPMS_WHITE);
    rounded(s_root, 229, 247, 8, 8, 4, TPMS_WHITE); /* ! */

    boost_tpms_ui_update(NULL);
}

void boost_tpms_ui_update(const boost_tpms_snapshot_t *snapshot)
{
    if (s_root == NULL) return;

    for (int i = 0; i < 4; ++i) {
        const bool received = snapshot != NULL &&
                              snapshot->wheel[i].age_ms != UINT32_MAX;
        const bool fresh = received && snapshot->wheel[i].valid;
        const bool low = fresh &&
                         snapshot->wheel[i].kpa < BOOST_TPMS_LOW_PRESSURE_KPA;
        char value[16];
        char state[12];
        uint32_t color;

        if (!received) {
            snprintf(value, sizeof(value), "--.-");
            snprintf(state, sizeof(state), "OFFLINE");
            color = TPMS_OFFLINE;
        } else if (!fresh) {
            snprintf(value, sizeof(value), "%4.1f", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "STALE");
            color = TPMS_AMBER;
        } else if (low) {
            snprintf(value, sizeof(value), "%4.1f", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "LOW");
            color = TPMS_RED;
        } else {
            snprintf(value, sizeof(value), "%4.1f", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "OK");
            color = TPMS_GREEN;
        }

        lv_label_set_text(s_psi[i], value);
        lv_label_set_text(s_unit[i], "PSI");
        lv_label_set_text(s_status[i], state);
        lv_obj_set_style_text_color(s_status[i], rgb(color), 0);
        lv_obj_set_style_bg_color(s_tire[i], rgb(color), 0);
        lv_obj_set_style_shadow_color(s_tire[i], rgb(color), 0);
        lv_obj_set_style_shadow_opa(s_tire[i],
                                    color == TPMS_OFFLINE ? LV_OPA_10 : LV_OPA_30, 0);
    }
}

void boost_tpms_ui_delete(void)
{
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    memset(s_tire, 0, sizeof(s_tire));
    memset(s_psi, 0, sizeof(s_psi));
    memset(s_unit, 0, sizeof(s_unit));
    memset(s_status, 0, sizeof(s_status));
}
