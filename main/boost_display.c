#include "boost_display.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"

/*
 * ESP32-S3 SPI GDMA cannot stream from PSRAM. The stock Waveshare BSP sets
 * use_psram=true for LVGL draw buffers, so every flush allocates an internal
 * DMA bounce copy of the strip. Under load that returns ESP_ERR_NO_MEM and
 * leaves the panel stuck half-white / half-green.
 *
 * Own the adapter profile: internal DRAM buffers (DMA-capable) + strip height
 * small enough for the internal heap, and cap SPI max_transfer_sz to one strip.
 */
#define BOOST_LVGL_BUF_LINES 20
#define BOOST_LVGL_STRIP_BYTES \
    ((size_t)BSP_LCD_H_RES * BOOST_LVGL_BUF_LINES * (BSP_LCD_BITS_PER_PIXEL / 8))

static const char *TAG = "boost_disp";

static lv_display_t *s_disp;
static lv_indev_t *s_indev;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_touch_handle_t s_touch;
static boost_display_metrics_t s_metrics;
static uint32_t s_render_count;
static uint32_t s_flush_count;
static uint32_t s_pixel_count;
static int64_t s_metrics_start_us;
/* CO5300 requires even x1/y1 and odd x2/y2 window edges. */
static void rounder_event_cb(lv_event_t *e)
{
    lv_area_t *area = (lv_area_t *)lv_event_get_param(e);
    if (area == NULL) {
        return;
    }
    area->x1 = (area->x1 >> 1) << 1;
    area->y1 = (area->y1 >> 1) << 1;
    area->x2 = ((area->x2 >> 1) << 1) + 1;
    area->y2 = ((area->y2 >> 1) << 1) + 1;
}

static void display_metrics_event_cb(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RENDER_READY) {
        ++s_render_count;
    } else if (code == LV_EVENT_FLUSH_START) {
        const lv_area_t *area = lv_event_get_param(e);
        if (area != NULL) {
            ++s_flush_count;
            s_pixel_count += (uint32_t)lv_area_get_width(area) * (uint32_t)lv_area_get_height(area);
        }
    }
    const int64_t now_us = esp_timer_get_time();
    if (now_us - s_metrics_start_us >= 1000000) {
        s_metrics.render_fps = s_render_count;
        s_metrics.flushes_per_second = s_flush_count;
        s_metrics.pixels_per_second = s_pixel_count;
        s_render_count = 0;
        s_flush_count = 0;
        s_pixel_count = 0;
        s_metrics_start_us = now_us;
    }
}

static lv_display_t *register_display(void)
{
    const bsp_display_config_t disp_cfg = {
        .max_transfer_sz = (int)BOOST_LVGL_STRIP_BYTES,
    };

    ESP_RETURN_ON_FALSE(
        bsp_display_new(&disp_cfg, &s_panel, &s_panel_io) == ESP_OK,
        NULL, TAG, "bsp_display_new failed");

    esp_lv_adapter_display_config_t adapter_disp = {
        .panel = s_panel,
        .panel_io = s_panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = ESP_LV_ADAPTER_ROTATE_0,
            .hor_res = BSP_LCD_H_RES,
            .ver_res = BSP_LCD_V_RES,
            .buffer_height = BOOST_LVGL_BUF_LINES,
            .use_psram = false, /* DMA-capable internal RAM */
            .enable_ppa_accel = false,
            .require_double_buffer = true,
            .mono_layout = ESP_LV_ADAPTER_MONO_LAYOUT_NONE,
        },
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .te_sync = ESP_LV_ADAPTER_TE_SYNC_DISABLED(),
    };

    lv_display_t *disp = esp_lv_adapter_register_display(&adapter_disp);
    if (disp == NULL) {
        ESP_LOGE(TAG, "esp_lv_adapter_register_display failed");
        return NULL;
    }

    s_metrics_start_us = esp_timer_get_time();
    lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);
    lv_display_add_event_cb(disp, display_metrics_event_cb, LV_EVENT_RENDER_READY, NULL);
    lv_display_add_event_cb(disp, display_metrics_event_cb, LV_EVENT_FLUSH_START, NULL);
    ESP_LOGI(TAG, "LVGL %dx%d partial, %d lines, internal DMA buffers, strip=%u B",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BOOST_LVGL_BUF_LINES,
             (unsigned)BOOST_LVGL_STRIP_BYTES);
    return disp;
}

static lv_indev_t *register_touch(lv_display_t *disp)
{
    bsp_display_cfg_t touch_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {
            .swap_xy = 0,
            .mirror_x = 1,
            .mirror_y = 1,
        },
    };

    if (bsp_touch_new(&touch_cfg, &s_touch) != ESP_OK || s_touch == NULL) {
        ESP_LOGE(TAG, "bsp_touch_new failed");
        return NULL;
    }

    const esp_lv_adapter_touch_config_t touch_indev =
        ESP_LV_ADAPTER_TOUCH_DEFAULT_CONFIG(disp, s_touch);
    lv_indev_t *indev = esp_lv_adapter_register_touch(&touch_indev);
    if (indev == NULL) {
        ESP_LOGE(TAG, "esp_lv_adapter_register_touch failed");
        return NULL;
    }
    return indev;
}

lv_display_t *boost_display_start(void)
{
    if (s_disp != NULL) {
        return s_disp;
    }

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.stack_in_psram = true;
    adapter_cfg.task_stack_size = 8 * 1024;
    adapter_cfg.task_priority = 6;
    adapter_cfg.task_max_delay_ms = 16;

    ESP_RETURN_ON_FALSE(
        esp_lv_adapter_init(&adapter_cfg) == ESP_OK,
        NULL, TAG, "esp_lv_adapter_init failed");

    s_disp = register_display();
    if (s_disp == NULL) {
        return NULL;
    }

    s_indev = register_touch(s_disp);
    if (s_indev == NULL) {
        ESP_LOGW(TAG, "touch init failed; continuing without input");
    }

    ESP_RETURN_ON_FALSE(
        bsp_display_brightness_init() == ESP_OK,
        NULL, TAG, "brightness init failed");

    ESP_RETURN_ON_FALSE(
        esp_lv_adapter_start() == ESP_OK,
        NULL, TAG, "esp_lv_adapter_start failed");

    ESP_LOGI(TAG, "display ready (DMA-safe partial refresh)");
    return s_disp;
}

esp_err_t boost_display_lock(uint32_t timeout_ms)
{
    return esp_lv_adapter_lock(timeout_ms);
}

void boost_display_unlock(void)
{
    esp_lv_adapter_unlock();
}

void boost_display_get_metrics(boost_display_metrics_t *out)
{
    if (out != NULL) {
        *out = s_metrics;
    }
}
