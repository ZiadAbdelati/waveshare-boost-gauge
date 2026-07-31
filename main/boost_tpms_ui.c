#include "boost_tpms_ui.h"

#include <stdio.h>
#include <string.h>

#define TPMS_SIZE 466
#define TPMS_BG 0x080B10
#define TPMS_PANEL 0x121923
#define TPMS_STEEL 0x667383
#define TPMS_TEXT 0xE7EDF4
#define TPMS_OK 0x62D6A5
#define TPMS_LOW 0xFFB020
#define TPMS_BAD 0xFF4D5A

static lv_obj_t *s_root;
static lv_obj_t *s_psi[4];
static lv_obj_t *s_status[4];

static lv_color_t rgb(uint32_t value)
{
    return lv_color_hex(value);
}

static void label_style(lv_obj_t *obj, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, rgb(color), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *pill(lv_obj_t *parent, int x, int y)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, 124, 78);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_radius(obj, 20, 0);
    lv_obj_set_style_bg_color(obj, rgb(TPMS_PANEL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, rgb(TPMS_STEEL), 0);
    return obj;
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

    lv_obj_t *title = lv_label_create(s_root);
    lv_label_set_text(title, "TIRE PRESSURE");
    label_style(title, &lv_font_montserrat_20, TPMS_TEXT);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *subtitle = lv_label_create(s_root);
    lv_label_set_text(subtitle, "LIVE SENSOR STATUS");
    label_style(subtitle, &lv_font_montserrat_12, TPMS_STEEL);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 46);

    /* Front and rear axles, with outside labels deliberately fixed. The three
     * fields stack vertically so the wide montserrat_20 pressure value never
     * collides with the status word. */
    static const char *names[4] = { "FL", "FR", "RL", "RR" };
    static const int xs[4] = { 28, 314, 28, 314 };
    static const int ys[4] = { 112, 112, 286, 286 };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *p = pill(s_root, xs[i], ys[i]);
        lv_obj_t *name = lv_label_create(p);
        lv_label_set_text(name, names[i]);
        label_style(name, &lv_font_montserrat_12, TPMS_STEEL);
        lv_obj_align(name, LV_ALIGN_TOP_LEFT, 14, 10);
        s_psi[i] = lv_label_create(p);
        label_style(s_psi[i], &lv_font_montserrat_20, TPMS_TEXT);
        lv_obj_align(s_psi[i], LV_ALIGN_LEFT_MID, 14, 0);
        s_status[i] = lv_label_create(p);
        label_style(s_status[i], &lv_font_montserrat_12, TPMS_OK);
        lv_obj_align(s_status[i], LV_ALIGN_BOTTOM_LEFT, 14, -10);
    }

    /* Dark, deliberately simple chassis silhouette: a body, wheel arches and
     * center spine. It is static art, not a vehicle model. */
    lv_obj_t *body = lv_obj_create(s_root);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 142, 274);
    lv_obj_set_pos(body, 162, 96);
    lv_obj_set_style_radius(body, 54, 0);
    lv_obj_set_style_bg_color(body, rgb(0x1D2733), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, rgb(TPMS_STEEL), 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *glass = lv_obj_create(body);
    lv_obj_remove_style_all(glass);
    lv_obj_set_size(glass, 94, 76);
    lv_obj_set_pos(glass, 24, 34);
    lv_obj_set_style_radius(glass, 24, 0);
    lv_obj_set_style_bg_color(glass, rgb(0x101923), 0);
    lv_obj_set_style_bg_opa(glass, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(glass, 1, 0);
    lv_obj_set_style_border_color(glass, rgb(TPMS_STEEL), 0);
    lv_obj_clear_flag(glass, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *spine = lv_obj_create(body);
    lv_obj_remove_style_all(spine);
    lv_obj_set_size(spine, 2, 144);
    lv_obj_set_pos(spine, 70, 118);
    lv_obj_set_style_bg_color(spine, rgb(TPMS_STEEL), 0);
    lv_obj_set_style_bg_opa(spine, LV_OPA_40, 0);
    lv_obj_clear_flag(spine, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *footer = lv_label_create(s_root);
    lv_label_set_text(footer, "TPMS  /  FOUR-WHEEL MONITOR");
    label_style(footer, &lv_font_montserrat_12, TPMS_STEEL);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -16);

    boost_tpms_ui_update(NULL);
}

void boost_tpms_ui_update(const boost_tpms_snapshot_t *snapshot)
{
    if (s_root == NULL) return;
    for (int i = 0; i < 4; ++i) {
        /* Three visually distinct states. A wheel that never reported keeps the
         * UINT32_MAX age sentinel from init -> red OFFLINE with no value. A
         * wheel that reported but aged past the staleness window still holds its
         * last pressure -> amber STALE with that value shown. A fresh wheel is
         * green OK, or amber LOW when below the placard band. */
        const bool received = snapshot != NULL && snapshot->wheel[i].age_ms != UINT32_MAX;
        const bool fresh = received && snapshot->wheel[i].valid;
        const bool low = fresh && snapshot->wheel[i].kpa < BOOST_TPMS_LOW_PRESSURE_KPA;
        char value[24];
        char state[12];
        uint32_t color;
        if (!received) {
            snprintf(value, sizeof(value), "--.- PSI");
            snprintf(state, sizeof(state), "OFFLINE");
            color = TPMS_BAD;
        } else if (!fresh) {
            snprintf(value, sizeof(value), "%4.1f PSI", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "STALE");
            color = TPMS_LOW;
        } else if (low) {
            snprintf(value, sizeof(value), "%4.1f PSI", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "LOW");
            color = TPMS_LOW;
        } else {
            snprintf(value, sizeof(value), "%4.1f PSI", (double)snapshot->wheel[i].psi);
            snprintf(state, sizeof(state), "OK");
            color = TPMS_OK;
        }
        lv_label_set_text(s_psi[i], value);
        lv_label_set_text(s_status[i], state);
        lv_obj_set_style_text_color(s_status[i], rgb(color), 0);
    }
}

void boost_tpms_ui_delete(void)
{
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }
    memset(s_psi, 0, sizeof(s_psi));
    memset(s_status, 0, sizeof(s_status));
}
