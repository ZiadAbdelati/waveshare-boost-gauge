#include "boost_page.h"

#include <stdlib.h>
#include <string.h>

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_theme.h"

#ifdef ESP_PLATFORM
#include "boost_model.h"
#endif

#define PAGE_SIZE 466
#define TAP_SLOP_PX 12
#define SWIPE_MIN_PX 48
#define HOLD_DIM_MS 2000

static lv_obj_t *s_page_root[2];
static boost_page_id_t s_active = BOOST_PAGE_BOOST;
static lv_obj_t *s_screen;
static lv_point_t s_start;
static int32_t s_max_dx;
static int32_t s_max_dy;
static uint32_t s_start_ms;
static bool s_press_active;
static bool s_hold_fired;
static bool s_tpms_built;

static int32_t abs_i32(int32_t x) { return x < 0 ? -x : x; }

static bool media_active(void)
{
    return boost_gauge_media_active();
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

void boost_page_handle_event(lv_event_t *event)
{
    if (event == NULL || media_active()) return;
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev == NULL) return;
        lv_indev_get_point(indev, &s_start);
        s_max_dx = s_max_dy = 0;
        s_start_ms = lv_tick_get();
        s_press_active = true;
        s_hold_fired = false;
    } else if (code == LV_EVENT_PRESSING && s_press_active) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev != NULL) {
            lv_point_t point;
            lv_indev_get_point(indev, &point);
            int32_t dx = point.x - s_start.x;
            int32_t dy = point.y - s_start.y;
            if (abs_i32(dx) > abs_i32(s_max_dx)) s_max_dx = dx;
            if (abs_i32(dy) > abs_i32(s_max_dy)) s_max_dy = dy;
        }
        if (!s_hold_fired && lv_tick_elaps(s_start_ms) >= HOLD_DIM_MS) {
            boost_brightness_toggle_max_min();
            s_hold_fired = true;
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && s_press_active) {
        bool released = code == LV_EVENT_RELEASED;
        if (released && !s_hold_fired) {
            int32_t ax = abs_i32(s_max_dx), ay = abs_i32(s_max_dy);
            if (ax < TAP_SLOP_PX && ay < TAP_SLOP_PX) {
                if (s_active == BOOST_PAGE_BOOST) boost_gauge_reset_peak();
            } else if (ax >= SWIPE_MIN_PX && (int64_t)ax * 4 >= (int64_t)ay * 5) {
                /* Finger travel follows the page motion: swipe left from the
                 * boost page advances to TPMS; swipe right returns. Outward
                 * swipes at either end are deliberately ignored (no wrap). */
                if (s_active == BOOST_PAGE_BOOST && s_max_dx < 0) {
                    show_page(BOOST_PAGE_TPMS);
                } else if (s_active == BOOST_PAGE_TPMS && s_max_dx > 0) {
                    show_page(BOOST_PAGE_BOOST);
                }
            } else if (s_active == BOOST_PAGE_BOOST &&
                       ay >= SWIPE_MIN_PX && (int64_t)ay * 4 >= (int64_t)ax * 5) {
                /* Theme gestures remain vertical and are legal only on page 0. */
                apply_theme_delta(s_max_dy < 0 ? 1 : -1);
            }
        }
        s_press_active = false;
        s_hold_fired = false;
    }
}

void boost_page_create(void)
{
    s_screen = lv_screen_active();
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_size(s_screen, PAGE_SIZE, PAGE_SIZE);
    lv_obj_add_flag(s_screen, LV_OBJ_FLAG_CLICKABLE);
    /* The screen must NOT be scrollable: the page coordinator owns every
     * gesture itself, and a scrollable screen lets the indev switch into
     * scroll mode on touch jitter during a long press - which steals the
     * PRESSING stream the 2 s hold-to-dim timer depends on. */
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_screen, boost_page_handle_event, LV_EVENT_PRESS_LOST, NULL);

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
    show_page(BOOST_PAGE_BOOST);
}

void boost_page_update(const boost_sample_t *sample)
{
    if (s_active == BOOST_PAGE_BOOST) boost_gauge_update(sample);
}

void boost_page_update_tpms(const boost_tpms_snapshot_t *snapshot)
{
    if (s_active == BOOST_PAGE_TPMS) boost_tpms_ui_update(snapshot);
}

boost_page_id_t boost_page_active(void) { return s_active; }

void boost_page_show(boost_page_id_t page)
{
    if (page <= BOOST_PAGE_TPMS) show_page(page);
}

lv_obj_t *boost_page_root(boost_page_id_t page)
{
    return page <= BOOST_PAGE_TPMS ? s_page_root[page] : NULL;
}
