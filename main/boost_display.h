#pragma once

#include <stdbool.h>

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
    /* Tearing-effect sync. te_period_us is the panel's measured frame period,
     * averaged over the first ~120 TE edges - the only direct evidence of the
     * real scan rate, since the init sequence sets no frame-rate register.
     * Zero means TE is off or has not been measured yet. te_waits/te_timeouts
     * count per second: one wait per render cycle, and a timeout means the TE
     * edge did not arrive and the strip was flushed unsynchronised. A steady
     * nonzero te_timeouts means TE is not wired as assumed. */
    uint32_t te_period_us;
    uint32_t te_waits;
    uint32_t te_timeouts;
    /* Region-dbuf's adaptive wait (te_wait_for_region(), Fix 2): count of
     * cycles per second where the estimated panel scan position was proven
     * to be either before the burst's dirty rows or already past all of
     * them, so the vblank wait was skipped as unnecessary rather than
     * burning up to a full ~16.75 ms period. 0 when region-dbuf is off (the
     * per-flush path still uses the older fixed-window check, which is not
     * counted here). */
    uint32_t te_skips;
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

/* Enable/disable tearing-effect synchronisation at runtime. The GPIO/ISR are
 * always set up at boot (cheap); this only gates whether render cycles wait for
 * the panel's vertical-blank edge. boost_display_te() reports whether it is
 * both enabled and actually receiving edges. */
void boost_display_set_te(bool enabled);
bool boost_display_te(void);

/*
 * Region double-buffering: rasterise a whole render cycle's dirty strips into
 * a PSRAM staging canvas instead of transferring each one immediately, then
 * push the accumulated region to the panel back-to-back after a single TE
 * wait. Fixes the residual needle tearing that a per-flush TE wait could not:
 * per-flush waiting only synchronises the first strip, and rasterisation of
 * later strips (CPU-bound, no 2D accelerator) lets the scan outrun the
 * writes. See the "region double-buffer feasibility" ledger row for the
 * measured per-strip transfer time and PSRAM->internal memcpy bandwidth this
 * is sized against.
 *
 * Runtime-toggleable, default OFF, same shape as boost_display_set_te(): a
 * failed PSRAM allocation degrades to the existing per-strip path with a
 * warning rather than failing boot, matching the pattern already used for
 * the cached face backgrounds.
 */
void boost_display_set_region_dbuf(bool enabled);
bool boost_display_region_dbuf(void);

esp_err_t boost_display_lock(uint32_t timeout_ms);
void boost_display_unlock(void);
void boost_display_get_metrics(boost_display_metrics_t *out);

#ifdef __cplusplus
}
#endif
