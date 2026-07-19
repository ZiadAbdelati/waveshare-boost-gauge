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
} boost_display_metrics_t;

lv_display_t *boost_display_start(void);

esp_err_t boost_display_lock(uint32_t timeout_ms);
void boost_display_unlock(void);
void boost_display_get_metrics(boost_display_metrics_t *out);

#ifdef __cplusplus
}
#endif
