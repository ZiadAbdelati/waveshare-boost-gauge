#pragma once

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Bring up CO5300 + LVGL with internal DMA-capable draw buffers.
 *
 * The stock Waveshare BSP parks LVGL buffers in PSRAM. ESP32-S3 SPI GDMA
 * cannot stream from PSRAM, so every flush allocates a temporary DMA copy
 * and eventually returns ESP_ERR_NO_MEM — leaving the half white / half green
 * panel seen on hardware. This path keeps draw buffers in internal RAM and
 * caps transfer size to one strip.
 */
typedef struct {
    uint32_t render_fps;
    uint32_t flushes_per_second;
    uint32_t pixels_per_second;
    /* Longest gap between consecutive render-ready events in the window. This
     * is the number that tracks perceived choppiness: render_fps counts how
     * often the screen changed, which says nothing about whether one of those
     * changes stalled the pipeline for 70 ms. */
    uint32_t worst_render_us;
    /* Frame PACING, as distinct from render duration above. Judder is a gap
     * problem: a face can render every cycle in 12 ms and still look uneven if
     * the cycles land at 16/16/20/16 ms. Measured START-to-START. */
    uint32_t render_gap_p50_us;
    uint32_t render_gap_max_us;
    uint32_t frames_over_budget;
} boost_display_metrics_t;

lv_display_t *boost_display_start(void);

/**
 * Set panel brightness, 0-100.
 *
 * Owned here rather than delegated to bsp_display_brightness_set(): that writes
 * through a file-static panel handle only assigned inside bsp_display_new(),
 * which this module no longer calls. It is a single 0x51 command either way.
 */
esp_err_t boost_display_set_brightness(int percent);

esp_err_t boost_display_lock(uint32_t timeout_ms);
void boost_display_unlock(void);
void boost_display_get_metrics(boost_display_metrics_t *out);

#ifdef __cplusplus
}
#endif
