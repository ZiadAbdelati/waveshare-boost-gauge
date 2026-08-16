#include "boost_tpms_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tpms_powertrain_rgb565.h"

/* This module owns its local declaration for the compiled Saira
 * SemiCondensed-Bold physical readout font. */
LV_FONT_DECLARE(font_wide_22);

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define BG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BG_FREE(p)  heap_caps_free(p)
#else
#define BG_ALLOC(n) malloc(n)
#define BG_FREE(p)  free(p)
#endif

#define TPMS_SIZE       466
#define TPMS_WHITE      0xF2F5F8u
#define TPMS_GREEN      0x62D6A5u
#define TPMS_RED        0xE8362Eu
#define TPMS_AMBER      0xFFB020u
#define TPMS_OFFLINE    0x5A6573u
#define TPMS_BG_BYTES   ((size_t)TPMS_SIZE * (size_t)TPMS_SIZE * 2u)

_Static_assert(TPMS_POWERTRAIN_W == TPMS_SIZE && TPMS_POWERTRAIN_H == TPMS_SIZE,
               "TPMS powertrain art must be 466x466");
_Static_assert(sizeof(tpms_powertrain_rgb565) == TPMS_BG_BYTES,
               "TPMS powertrain art must be RGB565 466x466");

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    int16_t radius;
} tpms_capsule_t;

/* Final tire bounds after the source art is alpha-composited, sharpened and
 * scaled to 80% around the 466-space centre by process_tpms_powertrain.py. */
static const tpms_capsule_t s_capsule[4] = {
    { 129, 80, 52, 104, 26 }, /* FL */
    { 284, 80, 53, 104, 26 }, /* FR */
    { 114, 277, 54, 109, 27 }, /* RL */
    { 297, 277, 54, 109, 27 }, /* RR */
};

/* The drawn capsule grows this many px beyond the art's tire bounds on every
 * side. At exact bounds the art's anti-aliased tire edge leaves a couple of
 * white pixels peeking around the capsule; +2 covers them (the invalidation
 * margin below already accounts for the shadow, which spreads further). */
#define TPMS_CAPSULE_GROW 2

static lv_obj_t *s_root;
static lv_obj_t *s_canvas;
static lv_obj_t *s_face;
static lv_obj_t *s_psi[4];
static void *s_canvas_buf;
static uint32_t s_capsule_color[4] = {
    TPMS_OFFLINE, TPMS_OFFLINE, TPMS_OFFLINE, TPMS_OFFLINE,
};

static lv_color_t tpms_color(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static void make_passive(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

static void draw_tpms_capsules(lv_event_t *event)
{
    lv_layer_t *layer = lv_event_get_layer(event);
    lv_obj_t *face = lv_event_get_target(event);
    lv_area_t face_area;
    lv_obj_get_coords(face, &face_area);

    for (int i = 0; i < 4; ++i) {
        const tpms_capsule_t *capsule = &s_capsule[i];
        lv_draw_rect_dsc_t rect;
        lv_draw_rect_dsc_init(&rect);
        rect.bg_color = tpms_color(s_capsule_color[i]);
        rect.bg_opa = LV_OPA_COVER;
        rect.radius = capsule->radius + TPMS_CAPSULE_GROW;
        rect.shadow_color = rect.bg_color;
        rect.shadow_width = 6;
        rect.shadow_opa = LV_OPA_30;

        lv_area_t area = {
            face_area.x1 + capsule->x - TPMS_CAPSULE_GROW,
            face_area.y1 + capsule->y - TPMS_CAPSULE_GROW,
            face_area.x1 + capsule->x + capsule->w - 1 + TPMS_CAPSULE_GROW,
            face_area.y1 + capsule->y + capsule->h - 1 + TPMS_CAPSULE_GROW,
        };
        lv_draw_rect(layer, &rect, &area);
    }
}

static lv_obj_t *make_psi_label(lv_obj_t *parent, int x, int y, bool right_aligned)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, "--.-");
    lv_obj_set_width(label, 66);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, &font_wide_22, 0);
    lv_obj_set_style_text_color(label, tpms_color(TPMS_WHITE), 0);
    lv_obj_set_style_text_align(label,
                                right_aligned ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT,
                                0);
    make_passive(label);
    return label;
}

static void format_psi(char *out, size_t out_size, float psi)
{
    if (out == NULL || out_size == 0) return;
    snprintf(out, out_size, "%.1f", (double)psi);
}

void boost_tpms_ui_create(lv_obj_t *parent)
{
    if (parent == NULL) return;
    boost_tpms_ui_delete();

    s_root = lv_obj_create(parent);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, TPMS_SIZE, TPMS_SIZE);
    lv_obj_set_pos(s_root, 0, 0);
    lv_obj_set_style_bg_color(s_root, tpms_color(0x000000), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    make_passive(s_root);

    const uint32_t canvas_bytes = LV_CANVAS_BUF_SIZE(TPMS_SIZE, TPMS_SIZE, 16,
                                                      LV_DRAW_BUF_STRIDE_ALIGN);
    s_canvas_buf = BG_ALLOC(canvas_bytes);
    if (s_canvas_buf != NULL) {
        s_canvas = lv_canvas_create(s_root);
        lv_canvas_set_buffer(s_canvas, s_canvas_buf, TPMS_SIZE, TPMS_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_canvas, 0, 0);
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        /* The source is a complete packed RGB565 frame. Copy it into the canvas
         * row by row using the draw-buffer stride so the art stays un-sheared
         * even if the stride is padded beyond 466*2 (the gauge caches address
         * their rows via header.stride the same way). The leading clear keeps
         * the canvas black in any stride padding / overscan. */
        memset(s_canvas_buf, 0, canvas_bytes);
        const uint32_t row_bytes = (uint32_t)TPMS_SIZE * 2u;
        lv_draw_buf_t *tpms_db = lv_canvas_get_draw_buf(s_canvas);
        const uint32_t stride = (tpms_db != NULL && tpms_db->header.stride >= row_bytes)
                                ? tpms_db->header.stride : row_bytes;
        for (uint32_t y = 0; y < TPMS_SIZE; ++y) {
            memcpy((uint8_t *)s_canvas_buf + (size_t)y * stride,
                   tpms_powertrain_rgb565 + (size_t)y * row_bytes, row_bytes);
        }
    }

    /* The face is transparent and draws only the four live capsules. It is
     * created after the canvas so its draw callback is composited on top. */
    s_face = lv_obj_create(s_root);
    lv_obj_remove_style_all(s_face);
    lv_obj_set_size(s_face, TPMS_SIZE, TPMS_SIZE);
    lv_obj_set_pos(s_face, 0, 0);
    make_passive(s_face);
    lv_obj_add_event_cb(s_face, draw_tpms_capsules, LV_EVENT_DRAW_MAIN, NULL);

    /* FL, FR, RL, RR. A 66 px box gives "--.-" and 2-digit PSI values room at
     * Saira SemiCondensed Bold 22. Left boxes end 8 px before the tire; right boxes begin 8 px
     * after it, matching the browser's native-466 geometry. */
    s_psi[0] = make_psi_label(s_root, 55, 119, true);
    s_psi[1] = make_psi_label(s_root, 345, 119, false);
    s_psi[2] = make_psi_label(s_root, 40, 318, true);
    s_psi[3] = make_psi_label(s_root, 359, 318, false);

    boost_tpms_ui_update(NULL);
}

void boost_tpms_ui_update(const boost_tpms_snapshot_t *snapshot)
{
    if (s_root == NULL) return;

    boost_tpms_config_t cfg;
    boost_tpms_get_config(&cfg);
    for (int i = 0; i < 4; ++i) {
        const bool received = snapshot != NULL &&
                              snapshot->wheel[i].age_ms != UINT32_MAX;
        const bool fresh = received && snapshot->wheel[i].valid;
        const bool low = fresh &&
                         snapshot->wheel[i].kpa < cfg.low_kpa;
        const char *value = "--.-";
        uint32_t color = TPMS_OFFLINE;
        char formatted[16];

        if (received) {
            format_psi(formatted, sizeof(formatted), snapshot->wheel[i].psi);
            value = formatted;
            if (!fresh) {
                color = TPMS_AMBER;
            } else if (low) {
                color = TPMS_RED;
            } else {
                color = TPMS_GREEN;
            }
        }

        s_capsule_color[i] = color;
        lv_label_set_text(s_psi[i], value);
    }

    /* Invalidate only the four capsule rects (plus shadow margin) instead of
     * the entire 466x466 face. A full-face invalidate repaints 217k px every
     * 250 ms tick, which tanks the gauge render rate. */
    if (s_face != NULL) {
        for (int i = 0; i < 4; ++i) {
            const tpms_capsule_t *c = &s_capsule[i];
            lv_area_t a = { c->x - 8, c->y - 8,
                            c->x + c->w + 8, c->y + c->h + 8 };
            lv_obj_invalidate_area(s_face, &a);
            if (s_psi[i] != NULL) lv_obj_invalidate(s_psi[i]);
        }
    }
}

void boost_tpms_ui_delete(void)
{
    /* Canvas pixels live outside the object. Drain queued software draw units
     * before deleting the canvas and releasing that PSRAM allocation. */
    lv_draw_wait_for_finish();
    if (s_root != NULL) {
        lv_obj_delete(s_root);
        s_root = NULL;
    }

    s_canvas = NULL;
    s_face = NULL;
    memset(s_psi, 0, sizeof(s_psi));

    if (s_canvas_buf != NULL) {
        BG_FREE(s_canvas_buf);
        s_canvas_buf = NULL;
    }

    for (int i = 0; i < 4; ++i) s_capsule_color[i] = TPMS_OFFLINE;
}
