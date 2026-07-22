#include "boost_display.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
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

/*
 * QSPI clock for the CO5300. The driver's own macro defaults to 40 MHz, and the
 * BSP's bsp_display_new() takes that default with no hook to override it, so
 * the panel bring-up is vendored below purely to own this number. Do not move
 * it back into managed_components/: an edit there is silently reverted by any
 * dependency refresh, which is exactly how this repo ended up documenting a
 * "60 MHz trial" that was never actually in effect.
 *
 * Measured on hardware, worst render cycle for a full-screen recolour:
 *   40 MHz -> 45.1 ms      80 MHz -> 37.6 ms
 * The gain lands only on full-frame pushes; partial-update faces are bound by
 * CPU rasterisation and move 0-3%. If the panel ever shows sparkle or torn
 * rows, drop this back to 40 MHz first.
 */
#define BOOST_LCD_PCLK_HZ (80 * 1000 * 1000)

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
static int64_t s_last_render_us;
static uint32_t s_worst_gap_us;
/* START-to-START gaps for the current window. 80 slots covers a second at any
 * rate the panel can actually achieve; beyond that we stop recording and the
 * count still tells the story. */
#define GAP_SLOTS 80
static int64_t s_last_start_us;
static uint16_t s_gaps_ms10[GAP_SLOTS];  /* tenths of a ms, plenty of range */
static uint8_t s_gap_n;
static uint32_t s_gap_max_us;
static uint32_t s_over_budget;
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
    if (code == LV_EVENT_RENDER_START) {
        const int64_t t = esp_timer_get_time();
        if (s_last_start_us != 0) {
            const uint32_t gap = (uint32_t)(t - s_last_start_us);
            if (gap > s_gap_max_us) s_gap_max_us = gap;
            /* 20 ms: a 16 ms budget plus a quarter frame of slack. */
            if (gap > 20000u) ++s_over_budget;
            if (s_gap_n < GAP_SLOTS) s_gaps_ms10[s_gap_n++] = (uint16_t)(gap / 100u);
        }
        s_last_start_us = t;
        s_last_render_us = t;
    } else if (code == LV_EVENT_RENDER_READY) {
        ++s_render_count;
        /* Duration of the cycle itself, not the gap between cycles: an idle
         * screen produces long gaps but no stall, and conflating the two is
         * what made the first cut of this metric useless. */
        if (s_last_render_us != 0) {
            const uint32_t dur = (uint32_t)(esp_timer_get_time() - s_last_render_us);
            if (dur > s_worst_gap_us) s_worst_gap_us = dur;
        }
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
        s_metrics.worst_render_us = s_worst_gap_us;
        /* Median by insertion sort - at most 80 entries, once a second. */
        if (s_gap_n > 0) {
            for (uint8_t i = 1; i < s_gap_n; ++i) {
                const uint16_t v = s_gaps_ms10[i];
                int8_t j = (int8_t)i - 1;
                while (j >= 0 && s_gaps_ms10[j] > v) { s_gaps_ms10[j + 1] = s_gaps_ms10[j]; --j; }
                s_gaps_ms10[j + 1] = v;
            }
            s_metrics.render_gap_p50_us = (uint32_t)s_gaps_ms10[s_gap_n / 2] * 100u;
        } else {
            s_metrics.render_gap_p50_us = 0;
        }
        s_metrics.render_gap_max_us = s_gap_max_us;
        s_metrics.frames_over_budget = s_over_budget;
        s_gap_n = 0;
        s_gap_max_us = 0;
        s_over_budget = 0;
        s_render_count = 0;
        s_flush_count = 0;
        s_pixel_count = 0;
        s_worst_gap_us = 0;
        s_metrics_start_us = now_us;
    }
}

/* Vendored from the Waveshare BSP's bsp_display_new(). Identical apart from
 * pclk_hz and the queue depth; kept here so the clock is owned by this repo. */
static const co5300_lcd_init_cmd_t k_lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static esp_err_t panel_new(void)
{
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        (int)BOOST_LVGL_STRIP_BYTES);
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, NULL, NULL);
    io_config.pclk_hz = BOOST_LCD_PCLK_HZ;
    io_config.trans_queue_depth = CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH;

    co5300_vendor_config_t vendor_config = {
        .init_cmds = k_lcd_init_cmds,
        .init_cmds_size = sizeof(k_lcd_init_cmds) / sizeof(k_lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    ESP_RETURN_ON_ERROR(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, &s_panel_io),
        TAG, "esp_lcd_new_panel_io_spi failed");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BSP_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_BITS_PER_PIXEL,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_panel_io, &panel_config, &s_panel),
                        TAG, "esp_lcd_new_panel_co5300 failed");

    esp_lcd_panel_set_gap(s_panel, 0x06, 0);
    esp_lcd_panel_reset(s_panel);
    esp_lcd_panel_init(s_panel);
    esp_lcd_panel_disp_on_off(s_panel, true);
    ESP_LOGI(TAG, "panel up at %d MHz QSPI", BOOST_LCD_PCLK_HZ / 1000000);
    return ESP_OK;
}

esp_err_t boost_display_set_brightness(int percent)
{
    ESP_RETURN_ON_FALSE(s_panel_io != NULL, ESP_ERR_INVALID_STATE, TAG, "panel io not ready");
    ESP_RETURN_ON_FALSE(percent >= 0 && percent <= 100, ESP_ERR_INVALID_ARG, TAG,
                        "brightness %d out of range", percent);
    /* CO5300 write-display-brightness, QSPI framing: cmd in bits 8..15 with the
     * 0x02 command prefix in bits 24..31, one data byte. */
    const uint32_t lcd_cmd = (0x02u << 24) | (0x51u << 8);
    const uint8_t param = (uint8_t)(percent * 255 / 100);
    return esp_lcd_panel_io_tx_param(s_panel_io, lcd_cmd, &param, 1);
}

static lv_display_t *register_display(void)
{
    ESP_RETURN_ON_FALSE(panel_new() == ESP_OK, NULL, TAG, "panel_new failed");

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
    lv_display_add_event_cb(disp, display_metrics_event_cb, LV_EVENT_RENDER_START, NULL);
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
        boost_display_set_brightness(100) == ESP_OK,
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
