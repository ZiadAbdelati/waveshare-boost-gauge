#include "boost_page.h"

#include <stdlib.h>
#include <string.h>

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_theme.h"

#ifdef ESP_PLATFORM
#include "boost_display.h"
#include "boost_model.h"
#include "esp_log.h"
#include "esp_timer.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

#define PAGE_SIZE 466
#define TAP_SLOP_PX 12
#define SWIPE_MIN_PX 48
/* Calibrated from repeated on-glass tests rather than inferred from the panel
 * command timestamp. Keep this explicit so the physical feel stays intentional. */
#define HOLD_DIM_MS 1000

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

static bool media_active(void);

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
    /* Page replacement is a full-face operation. Make that contract explicit
     * for the partial-refresh adapter so no pixels from the hidden page survive
     * until one of the destination page's narrow live regions changes. */
    if (s_screen != NULL) lv_obj_invalidate(s_screen);
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
    const bool act = released && !s_hold_fired && !media_active();

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
    lv_indev_t *indev = (lv_indev_t *)lv_event_get_target(event);
    const lv_event_code_t code = lv_event_get_code(event);

    if (code == LV_EVENT_PRESSED) {
        if (media_active()) return;
        s_press_indev = indev;
        lv_indev_get_point(indev, &s_start);
        s_max_dx = s_max_dy = 0;
        s_press_active = true;
        s_hold_fired = false;
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
               indev == s_press_indev && !s_hold_fired && !media_active()) {
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

void boost_page_handle_event(lv_event_t *event)
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
