#include "boost_display.h"
#include "boost_theme.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "bsp/touch.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

/*
 * ---------------------------------------------------------------------------
 * Tearing-effect (TE) synchronisation
 * ---------------------------------------------------------------------------
 *
 * The panel already drives TE: k_lcd_init_cmds below sends {0x35, 0x00}
 * (Tearing Effect Line ON, V-blank only). The line lands on GPIO13 - decoded
 * from the board netlist (PIU1018 NLGPIO13 NLLCD0TE, next to
 * PIU1017 NLGPIO12 NLLCD0CS, and GPIO12 is BSP_LCD_CS, which validates the
 * decode). The Waveshare BSP does not expose it, which is why it looked
 * unrouted. GPIO13 is used by nothing else in this firmware: the BSP pin map
 * has no other net on it and no other module here calls gpio_config().
 *
 * Why this is hand-rolled instead of ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC:
 * the adapter's display_bridge_v9_flush_gpio_te() calls te_sync_begin_frame()
 * (which drains the semaphore) and te_sync_wait_for_vsync() on EVERY flush,
 * and then blocks on ulTaskNotifyTake(portMAX_DELAY) for the transfer as well.
 * LVGL runs partial mode with 20-line strips here, so one needle update is 2-3
 * strips and a full repaint is 24. That would be 2-3 TE waits (33-50 ms) for a
 * needle and ~400 ms for a repaint - strictly worse than the tearing, and the
 * portMAX_DELAY would also serialise transfer with rasterisation.
 *
 * Instead: stay in TEAR_AVOID_MODE_NONE and gate in application code through
 * the adapter's custom_draw_bitmap hook, which display_bridge_v9_flush_default()
 * honours (and only honours) in that mode. Arm a gate at LV_EVENT_RENDER_START;
 * the FIRST strip of the cycle waits for a TE edge, every later strip of the
 * same cycle streams out unblocked. One wait per render cycle, not per flush.
 *
 * The effect is a quantiser: the cycle locks to a whole number of panel frames.
 * Median render is ~12.7 ms and the frame is 14-17 ms, so a typical cycle
 * rounds up to one frame and paces evenly instead of drifting. Cycles that
 * overrun a frame round up to two - slower, but bimodal and steady rather than
 * randomly juddering.
 *
 * FAIL-SAFE. Every wait is bounded (never portMAX_DELAY). If TE is not wired
 * the way the schematic says, the pull-up holds the line high, no edge ever
 * arrives, and each wait costs BOOST_LCD_TE_TIMEOUT_MS. After
 * BOOST_LCD_TE_GIVEUP_STREAK consecutive timeouts TE gating latches off for
 * good and the display reverts to exactly today's behaviour. A gauge that
 * tears occasionally beats a gauge that freezes.
 */
#define BOOST_LCD_USE_TE 1

/* TE line. Not in the BSP pin map; see the netlist decode above. */
#define BOOST_LCD_TE_GPIO 13

/*
 * DCS TE with parameter 0x00 asserts the line at the start of vertical
 * blanking, so the RISING edge is the moment it is safest to start writing.
 * Flip to GPIO_INTR_NEGEDGE in one line if the panel drives it inverted; the
 * boot log prints the idle level so this is checkable rather than guessed.
 */
#define BOOST_LCD_TE_EDGE GPIO_INTR_POSEDGE

/*
 * Bound on one wait. A 60 Hz frame is 16.7 ms and the adapter's own Tvdl+Tvdh
 * defaults imply ~14 ms, so 25 ms covers a full period either way with margin.
 * CONFIG_FREERTOS_HZ is 1000 here, so this is honoured to the millisecond.
 */
#define BOOST_LCD_TE_TIMEOUT_MS 25

/* Consecutive timeouts before TE gating gives up permanently (~200 ms). */
#define BOOST_LCD_TE_GIVEUP_STREAK 8

/*
 * If the last edge is younger than this, blanking has only just started and
 * the write can go immediately - waiting would burn a whole extra frame for
 * nothing. Rasterisation of the first strip usually lands us here.
 *
 * Used by the per-flush path (te_wait_for_vblank, region-dbuf OFF) where the
 * write always starts at the panel's top row, so "close enough to vblank" is
 * the only condition available. region-dbuf ON uses te_wait_for_region_spans()
 * below instead, which knows each span's actual row range and so is not
 * limited to this fixed window - this constant remains its fallback for the
 * first ~2 s after boot, before the panel's period has been measured.
 */
#define BOOST_LCD_TE_FRESH_US 2000u

/*
 * Rows of slack subtracted from the estimated scan position before
 * te_wait_for_region_spans() decides a skip is safe: covers ISR-to-LVGL-task wake
 * latency and the row-time estimate's own quantisation, both of which bias
 * the raw arithmetic toward optimism. Conservative default, not tuned
 * against hardware - tightening it needs an A/B against teSkips/teTimeouts
 * and, since the payoff is invisible, the user's own eyes on the glass (see
 * the ledger method rules: snapshots cannot show a tear by construction).
 */
#define BOOST_LCD_TE_ROW_MARGIN 20

/*
 * CO5300 0x44 (set_tear_scanline) moves the TE assertion off V-blank to a
 * chosen line, so a write can be started just ahead of the scan rather than
 * racing it. Left off by default: the useful scanline depends on where the
 * strips actually are, and the panel's real scan rate is not confirmed. Set to
 * a line number in 0..(BSP_LCD_V_RES-1) to try it - it is one #define.
 */
#define BOOST_LCD_TE_SCANLINE (-1)

/* Number of TE periods to average before reporting the panel's real scan rate. */
#define BOOST_LCD_TE_PROBE_N 120u

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

#if BOOST_LCD_USE_TE
/*
 * TE state. Everything the ISR writes is a single 32-bit word so the LVGL task
 * can read it without a critical section: aligned 32-bit loads are atomic on
 * the S3, and the microsecond timestamp is deliberately truncated to 32 bits
 * (unsigned subtraction wraps correctly, and 71 minutes of range is far more
 * than the ~17 ms this is compared against).
 */
static SemaphoreHandle_t s_te_sem;
static volatile bool s_te_active;          /* latched off on repeated timeout */
static volatile bool s_te_enabled;         /* runtime toggle from settings */
static volatile uint32_t s_te_last_edge_us;
static volatile uint32_t s_te_edges;
static volatile uint32_t s_te_probe_sum_us; /* ISR stops writing once n hits N */
static volatile uint32_t s_te_probe_n;
static bool s_te_period_logged;
static bool s_te_gate_armed;               /* LVGL task only */
static uint32_t s_te_waits;
static uint32_t s_te_timeouts;
static uint32_t s_te_skips;   /* te_wait_for_region_spans() proved a wait unnecessary */
static uint8_t s_te_miss_streak;
/* Panel row time in ns, derived from the measured TE period once
 * te_log_period_once() has enough samples; 0 until then. Nanoseconds (not
 * microseconds) so te_wait_for_region_spans()'s scan-position estimate keeps a
 * useful amount of precision without floating point. */
static uint32_t s_te_row_time_ns;

/*
 * Not installed with ESP_INTR_FLAG_IRAM - the touch driver registers a
 * non-IRAM handler on the same shared GPIO service, so demanding IRAM here
 * would break it. IRAM_ATTR on the handler still keeps the common path off
 * flash; a TE edge missed during a flash write degrades to one timeout, which
 * the streak counter absorbs.
 */
static void IRAM_ATTR te_gpio_isr(void *arg)
{
    (void)arg;
    const uint32_t now = (uint32_t)esp_timer_get_time();
    const uint32_t prev = s_te_last_edge_us;
    const uint32_t edges = s_te_edges;

    s_te_last_edge_us = now;
    s_te_edges = edges + 1;

    if (edges != 0) {
        const uint32_t n = s_te_probe_n;
        if (n < BOOST_LCD_TE_PROBE_N) {
            /* Sum before count: the reader checks the count first. */
            s_te_probe_sum_us += now - prev;
            s_te_probe_n = n + 1;
        }
    }

    BaseType_t need_yield = pdFALSE;
    xSemaphoreGiveFromISR(s_te_sem, &need_yield);
    if (need_yield) {
        portYIELD_FROM_ISR();
    }
}

static esp_err_t te_init(void)
{
    s_te_sem = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_te_sem != NULL, ESP_ERR_NO_MEM, TAG, "TE semaphore alloc failed");

    const gpio_config_t te_cfg = {
        .pin_bit_mask = 1ULL << BOOST_LCD_TE_GPIO,
        .mode = GPIO_MODE_INPUT,
        /* Pull-up so an unconnected pin idles high and simply never edges,
         * which the give-up path turns into a clean "TE not seen" instead of
         * random interrupts from a floating input. */
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = BOOST_LCD_TE_EDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te_cfg), TAG, "TE gpio_config failed");

    const int idle_level = gpio_get_level(BOOST_LCD_TE_GPIO);

    esp_err_t err = gpio_install_isr_service(ESP_INTR_FLAG_LOWMED);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "TE isr service install failed");
    }
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(BOOST_LCD_TE_GPIO, te_gpio_isr, NULL),
                        TAG, "TE isr handler add failed");

    s_te_active = true;
    ESP_LOGI(TAG, "TE sync active on GPIO%d, %s edge, level=%d at boot, %d ms timeout",
             BOOST_LCD_TE_GPIO,
             (BOOST_LCD_TE_EDGE == GPIO_INTR_NEGEDGE) ? "falling" : "rising",
             idle_level, BOOST_LCD_TE_TIMEOUT_MS);
    return ESP_OK;
}

/* Called on the LVGL task, once per render cycle, from the first strip's blit. */
static void te_wait_for_vblank(void)
{
    if (!s_te_active || !s_te_enabled) {
        return;
    }

    ++s_te_waits;

    if (s_te_edges != 0) {
        const uint32_t age = (uint32_t)esp_timer_get_time() - s_te_last_edge_us;
        if (age < BOOST_LCD_TE_FRESH_US) {
            /* Blanking has only just begun - write now rather than idling a
             * whole frame to arrive at the same place. */
            s_te_miss_streak = 0;
            return;
        }
    }

    /* Drop the stale edge so the wait below returns on a fresh one. */
    (void)xSemaphoreTake(s_te_sem, 0);

    if (xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(BOOST_LCD_TE_TIMEOUT_MS)) == pdTRUE) {
        s_te_miss_streak = 0;
        return;
    }

    ++s_te_timeouts;
    if (s_te_miss_streak < BOOST_LCD_TE_GIVEUP_STREAK) {
        ++s_te_miss_streak;
    }
    if (s_te_miss_streak >= BOOST_LCD_TE_GIVEUP_STREAK && s_te_active) {
        s_te_active = false;
        ESP_LOGW(TAG,
                 "TE: no edge on GPIO%d for %d cycles (%u seen total) - gating off, "
                 "display may tear but will not stall",
                 BOOST_LCD_TE_GPIO, BOOST_LCD_TE_GIVEUP_STREAK, (unsigned)s_te_edges);
    }
}

/*
 * ---------------------------------------------------------------------------
 * Region double-buffering
 * ---------------------------------------------------------------------------
 * Fix for the residual needle tearing a per-flush TE wait could not touch
 * (see the "TE sync engages but does not stop the needle tearing" ledger
 * row): waiting for vblank only synchronises the FIRST strip of a render
 * cycle. Rasterisation of later strips (CPU-bound; the S3 has no 2D
 * accelerator) then lets the scan outrun the writes, so later strips still
 * land mid-scan.
 *
 * The fix is to stop interleaving raster and transfer: rasterise the whole
 * cycle's dirty strips into a PSRAM staging canvas first (te_draw_bitmap_cb
 * below intercepts every strip, copies it into the canvas, and returns
 * ESP_ERR_NOT_ALLOWED so the adapter calls flush_ready immediately without
 * touching the panel), then push the accumulated region to the panel
 * back-to-back after a single TE wait, from LV_EVENT_RENDER_READY.
 *
 * Feasibility was measured on hardware before building this (branch
 * spike/region-double-buffer): per-strip DMA transfer at 80 MHz QSPI averaged
 * 999.0 us (200 back-to-back transfers, 18,640 B each) against a ~0.47 ms
 * theoretical figure; PSRAM->internal memcpy for the same block size averaged
 * 50.9 us (~349 MB/s). For the needle's 205-row (~191 KB) worst-case band,
 * rounded to 11 strips: 11 x (999.0 + 50.9) = 11,548.9 us against a measured
 * tePeriodUs of 16,753 us - about 5.2 ms (31%) of margin. Rasterisation alone
 * (measured by making this hook skip the real write and reading worstRenderUs
 * with teSync off) had a 13.6 ms median over a 30 s window, also under the
 * period. GDMA still cannot stream from PSRAM, so the memcpy into an internal
 * DMA-capable scratch buffer per 20-line chunk is mandatory, not optional.
 *
 * Caveat found during measurement, not asked for but load-bearing: the
 * measured write rate (999.0 us / 20 lines = 49.95 us/row) is SLOWER than the
 * panel's own measured scan rate (16,753 us / 466 rows = 35.95 us/row). A
 * single wait-then-blast burst only stays ahead of the scan for the WHOLE
 * accumulated region if the region's top row is far enough down the panel
 * (solving the closing-gap arithmetic for this board: roughly row 86 of 466
 * or lower). A region starting nearer the top does not have unconditional
 * protection from this scheme alone - the CO5300's set_tear_scanline (0x44,
 * BOOST_LCD_TE_SCANLINE above) exists for exactly this and could shift the
 * effective trigger point per-cycle, but that is a further change, not done
 * here. This does not fail the Phase 1 gate (the scheme is a real, measured
 * improvement over interleaved raster/transfer either way) but it does mean
 * "eliminates tearing" would be an overclaim; "removes the interleaving that
 * made every frame race the scan" is the honest claim.
 *
 * Second measured caveat, found during hardware verification of this feature
 * (not Phase 1): a single cycle's dirty strips are not always one contiguous
 * band. update_vault() can invalidate the needle AND a digit slot label in
 * the same tick, at unrelated y ranges. A single min/max union across the
 * whole cycle (the first version of this code) would then span - and
 * transfer - every untouched row between them too: measured on hardware,
 * that inflated pixelsPerSecond ~7.5x on vault-tec (437,764 -> 3,269,456 B/s
 * median) and dropped the cadence guard from min 49/median 57 to min
 * 30/median 53-54. The fix below tracks up to BOOST_REGION_DBUF_MAX_SPANS
 * disjoint row-spans per cycle instead of one union, so unrelated dirty areas
 * are transferred separately rather than dragging the gap between them along.
 *
 * ---------------------------------------------------------------------------
 * RETRACTION (branch fix/region-dbuf-worst-case): the 999.0 us figure above
 * does not reproduce and the "write is SLOWER than scan" conclusion it
 * supported is wrong.
 * ---------------------------------------------------------------------------
 * Re-measured with a committed harness (size sweep 932 B - 37,280 B, three
 * clocks, 40 reps, completion-to-completion deltas - see the "measurement
 * nobody can re-run is not evidence" ledger row and README's Animation
 * performance contract section): the same 18,640 B strip at 80 MHz measures
 * 575.9 us at quiet boot / 578.1 us under full Wi-Fi+LVGL+HTTP load, not
 * 999.0 us. That is 28.8 us/row, AGAINST the panel's own measured scan rate
 * of 35.95 us/row (16,753 us / 466 rows) - the write is ~25% FASTER than the
 * scan, not slower. Including the PSRAM->internal memcpy (~2.5 us/row at the
 * measured ~349 MB/s), total write cost is ~31.3 us/row against the same
 * 35.95 us/row scan: roughly 13% margin, still write-faster-than-scan. The
 * root cause of the original 999.0 us figure was never found (it was not
 * committed as inspectable code - see the retraction row); it simply does
 * not reproduce.
 *
 * This changes the closing-gap arithmetic entirely: previously the burst
 * needed to start below roughly row 86 to have unconditional margin; with
 * write faster than scan, a burst starting AT OR BEFORE the scan's current
 * position stays ahead of the scan for its ENTIRE length, not just from
 * some row onward. See te_wait_for_region_spans() below - this argument is
 * Fix 2, extended per-span as Fix 3 (both defined together further down).
 *
 * ---------------------------------------------------------------------------
 * Fix 1 (investigated, NOT implemented): LVGL rendering directly into the
 * PSRAM canvas, eliminating the LVGL-strip -> canvas memcpy in
 * te_draw_bitmap_cb ("copy 1").
 * ---------------------------------------------------------------------------
 * This needs LVGL's direct render mode (LV_DISPLAY_RENDER_MODE_DIRECT):
 * LVGL rasterises straight into a full-screen buffer it owns, rather than a
 * small strip, so there is no intermediate strip to copy out of. The
 * vendored adapter (managed_components/espressif__esp_lvgl_adapter) DOES
 * support this - display_manager_pick_render_mode() maps
 * ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT to
 * ESP_LV_ADAPTER_DISPLAY_RENDER_MODE_DIRECT (display_manager.c:1869-1881) -
 * but display_manager_validate_tearing_mode() (display_manager.c:1814-1864)
 * only accepts tear_avoid_mode NONE or TE_SYNC for
 * ESP_LV_ADAPTER_PANEL_IF_OTHER, which is what this QSPI/CO5300 panel uses
 * (register_display() below). DOUBLE_DIRECT is accepted only for
 * ESP_LV_ADAPTER_PANEL_IF_RGB / MIPI_DSI. Passing DOUBLE_DIRECT here does
 * not degrade to something safe - display_manager_validate_tearing_mode()
 * returns false, esp_lv_adapter_register_display() returns NULL at
 * display_manager.c:219-221, and boost_display_start() fails outright. This
 * is a hard block confirmed by reading the vendored source, not a
 * measurement - no hardware needed to know it fails, and none was used.
 *
 * The alternative - bypassing esp_lv_adapter_register_display()'s own mode
 * picking and calling LVGL's lv_display_set_render_mode()/
 * lv_display_set_buffers() directly to force direct mode behind the
 * adapter's back - was considered and rejected without being built. The
 * adapter's own node state (esp_lv_adapter_display_node_t.cfg) tracks
 * render_mode/buffer layout for its OTHER entry points (rotation change,
 * sleep/wake framebuffer refetch, panel rebind); desyncing that from what
 * LVGL is actually doing is exactly the class of "edit a vendored
 * component's behaviour without it knowing" failure this repo has already
 * paid for twice (the phantom 60 MHz QSPI trial reverted by a dependency
 * refresh; bsp_display_brightness_set()'s file-static panel_handle left
 * NULL after panel_new() took over bring-up). Unlike those two, this one
 * cannot even be evaluated without hardware, and hardware is the one thing
 * not available while writing this. Per the task's own instruction: this is
 * a genuine blocker, so it falls back to optimising copy 1 instead of
 * removing it (below), not to a behind-the-back hack.
 *
 * Copy 1 optimisation actually made (te_draw_bitmap_cb): when the strip is
 * full panel width (x_start==0, x_end==BSP_LCD_H_RES - the common case for
 * most invalidated areas, though NOT the vault needle's column-narrow
 * wedge), source rows (color_map, already stride-compacted to exactly
 * `cols` per row) and destination rows (canvas stride BSP_LCD_H_RES) are
 * BOTH contiguous across the whole strip, so the per-row memcpy loop
 * collapses to one memcpy of rows*cols*2 bytes. This removes (rows-1) memcpy
 * call overheads per full-width strip; it does not touch the per-row path
 * needed when the strip is narrower than the panel, which cannot collapse
 * (destination rows are not contiguous when x_start > 0 or x_end <
 * BSP_LCD_H_RES). This is a real but modest reduction in call overhead, not
 * a change in bytes moved - it has NOT been measured on hardware, and PSRAM
 * write bandwidth/cache behaviour for the ORIGINAL idea (LVGL rasterising
 * into PSRAM directly) was never measured either, because the adapter
 * block above makes that measurement moot for this panel interface.
 */
#define BOOST_REGION_DBUF_QUEUE_DEPTH CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH

/* Needle + a handful of digit/peak labels is the realistic worst case for one
 * vault tick; 8 is generous headroom over that. Exceeding it degrades to
 * extending the nearest span (still correct, just coarser) rather than
 * dropping rows. */
#define BOOST_REGION_DBUF_MAX_SPANS 8

typedef struct {
    int y0; /* inclusive */
    int y1; /* exclusive */
    int x0; /* inclusive */
    int x1; /* exclusive */
} region_span_t;

static volatile bool s_region_dbuf_enabled;                       /* runtime toggle */
static uint16_t *s_region_canvas;                                 /* PSRAM, full panel */
static uint16_t *s_region_xfer_bufs[BOOST_REGION_DBUF_QUEUE_DEPTH]; /* internal DMA-capable */
static bool s_region_has_data;                                    /* LVGL task only */
static region_span_t s_region_spans[BOOST_REGION_DBUF_MAX_SPANS];
static int s_region_span_count;
static uint32_t s_region_xfer_idx;

/*
 * Merge [x0,x1) x [y0,y1) into the accumulated span set for this cycle.
 * Grouping is by y-overlap/adjacency only (x always unions into whichever
 * span the row-range lands in); this matters because the vault needle's
 * radial-wedge invalidation (VAULT_NEEDLE_SEGS=3) is column-narrow, not
 * full-width, and transferring full BSP_LCD_H_RES for every span - the first
 * version of this code - is exactly the kind of over-transfer this exists to
 * avoid, the same way the y-span split avoids dragging untouched rows along
 * with a digit-label update elsewhere on the face. Spans are not re-merged
 * against each other after extension (a bounded amount of redundant transfer
 * if two spans grow into overlapping, cheap at this scale).
 */
static void region_span_add(int x0, int y0, int x1, int y1)
{
    for (int i = 0; i < s_region_span_count; ++i) {
        if (y0 <= s_region_spans[i].y1 && y1 >= s_region_spans[i].y0) {
            if (y0 < s_region_spans[i].y0) s_region_spans[i].y0 = y0;
            if (y1 > s_region_spans[i].y1) s_region_spans[i].y1 = y1;
            if (x0 < s_region_spans[i].x0) s_region_spans[i].x0 = x0;
            if (x1 > s_region_spans[i].x1) s_region_spans[i].x1 = x1;
            return;
        }
    }
    if (s_region_span_count < BOOST_REGION_DBUF_MAX_SPANS) {
        s_region_spans[s_region_span_count] = (region_span_t){ .y0 = y0, .y1 = y1, .x0 = x0, .x1 = x1 };
        ++s_region_span_count;
        return;
    }
    /* Span budget exhausted this cycle: extend whichever existing span is
     * closest rather than lose the strip. */
    int nearest = 0;
    int best_gap = INT32_MAX;
    for (int i = 0; i < s_region_span_count; ++i) {
        int gap = (y0 > s_region_spans[i].y1) ? (y0 - s_region_spans[i].y1)
                                              : (s_region_spans[i].y0 - y1);
        if (gap < 0) gap = 0;
        if (gap < best_gap) { best_gap = gap; nearest = i; }
    }
    if (y0 < s_region_spans[nearest].y0) s_region_spans[nearest].y0 = y0;
    if (y1 > s_region_spans[nearest].y1) s_region_spans[nearest].y1 = y1;
    if (x0 < s_region_spans[nearest].x0) s_region_spans[nearest].x0 = x0;
    if (x1 > s_region_spans[nearest].x1) s_region_spans[nearest].x1 = x1;
}

static void region_dbuf_free(void)
{
    if (s_region_canvas != NULL) {
        heap_caps_free(s_region_canvas);
        s_region_canvas = NULL;
    }
    for (size_t i = 0; i < BOOST_REGION_DBUF_QUEUE_DEPTH; ++i) {
        if (s_region_xfer_bufs[i] != NULL) {
            heap_caps_free(s_region_xfer_bufs[i]);
            s_region_xfer_bufs[i] = NULL;
        }
    }
    s_region_has_data = false;
}

/* Sized for the whole panel rather than the ~205-row needle band specifically:
 * at 434,312 B against ~6.7 MB PSRAM free this costs nothing extra to make
 * general, and it removes any need to reject/clamp a larger dirty cycle (a
 * theme rebuild, pixel shift) instead of just degrading its cadence like the
 * existing TE quantiser already does. */
static bool region_dbuf_alloc(void)
{
    if (s_region_canvas != NULL) {
        return true;
    }
    const size_t canvas_bytes = (size_t)BSP_LCD_H_RES * (size_t)BSP_LCD_V_RES *
                                 (BSP_LCD_BITS_PER_PIXEL / 8);
    s_region_canvas = (uint16_t *)heap_caps_malloc(canvas_bytes, MALLOC_CAP_SPIRAM);
    if (s_region_canvas == NULL) {
        ESP_LOGW(TAG, "region-dbuf: %u B PSRAM canvas alloc failed; staying on per-strip path",
                 (unsigned)canvas_bytes);
        return false;
    }
    for (size_t i = 0; i < BOOST_REGION_DBUF_QUEUE_DEPTH; ++i) {
        s_region_xfer_bufs[i] = (uint16_t *)heap_caps_malloc(BOOST_LVGL_STRIP_BYTES,
                                                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (s_region_xfer_bufs[i] == NULL) {
            ESP_LOGW(TAG, "region-dbuf: internal scratch buffer %u/%d alloc failed; "
                     "staying on per-strip path", (unsigned)i, BOOST_REGION_DBUF_QUEUE_DEPTH);
            region_dbuf_free();
            return false;
        }
    }
    ESP_LOGI(TAG, "region-dbuf: %u B PSRAM canvas + %d x %u B internal scratch buffers ready",
             (unsigned)canvas_bytes, BOOST_REGION_DBUF_QUEUE_DEPTH, (unsigned)BOOST_LVGL_STRIP_BYTES);
    return true;
}

/*
 * Fix 2 (superseded by Fix 3 below - kept as the reference proof, since Fix 3
 * is an extension of exactly this argument, not a different one).
 *
 * Called once per render cycle, right before region-dbuf's burst. The
 * original single-call version took one [top_row, bottom_row] spanning ALL
 * of the cycle's dirty spans (min y0 / max y1 across them).
 *
 * te_wait_for_vblank() above always assumes the write starts at row 0, so
 * "safe to go now" can only mean "very close to the vblank edge"
 * (BOOST_LCD_TE_FRESH_US, a fixed 2 ms window). That assumption is wrong
 * here on two counts: region-dbuf's burst does not start at row 0, it starts
 * at top_row (frequently >100 for a needle/label update well down the
 * panel); and by the time this runs, ~13.6 ms of rasterisation has usually
 * already elapsed since RENDER_START, so the 2 ms window is stale on almost
 * every cycle - measured worst case, that forces a wait for the NEXT edge,
 * costing up to a full extra ~16.75 ms period on top of the raster time =
 * the observed 32 ms tail.
 *
 * The actual requirement is simpler than "start at vblank": with the write
 * now measured FASTER than the panel's scan (see the retraction block
 * comment above - 31.3 us/row write including memcpy vs 35.95 us/row scan),
 * there are TWO provably tear-free windows to start in, not one:
 *
 *   (a) EARLY: the scan has not yet reached top_row this pass. Proof sketch:
 *       let f(R) = time-for-write-to-reach-row-R - time-for-scan-to-reach-
 *       row-R for R in [top_row, bottom_row]. f is monotonically DEcreasing
 *       in R because the write is faster per row than the scan. If
 *       f(top_row) <= 0 - the scan has not yet passed top_row - then
 *       f(R) <= 0 for every later R too (write never falls behind): no tear
 *       anywhere in the burst.
 *   (b) LATE: the scan has already passed bottom_row this pass, i.e. it
 *       will not re-enter the burst's row range until its NEXT pass. Then
 *       f(top_row) >= 0 (scan is already past) AND f(bottom_row) >= 0 (the
 *       write, even starting from top_row, cannot cover ground fast enough
 *       to catch a scan that already has a head start over the ENTIRE
 *       burst height - the write is only ~13% faster per row, not enough to
 *       close a multi-row head start within one burst's own width). f
 *       decreasing and non-negative at both ends means it stays
 *       non-negative throughout: the scan reads OLD data for the whole
 *       region this pass, and the burst's new data shows cleanly on the
 *       panel's NEXT pass. No tear, and no half-frame-old flicker either -
 *       "next pass" is ~16.75 ms away, not user-visible as staleness.
 *
 * (The THIRD case - scan currently somewhere INSIDE [top_row, bottom_row] -
 * is the genuinely unsafe middle, and is deliberately NOT approximated here;
 * it falls back to waiting for the next edge, i.e. today's behaviour.)
 *
 * Scan position is estimated from elapsed time since the last TE edge and
 * the panel's own measured period (s_te_row_time_ns, set once by
 * te_log_period_once() - unavailable for the first ~2 s after boot, when
 * this falls back to the same FRESH_US window te_wait_for_vblank() uses).
 * This is deliberately conservative: it only skips the wait when it can
 * prove doing so is safe, and degrades to the pre-fix behaviour (never
 * worse) otherwise.
 *
 * Where this fell short (found measuring fast vault-tec motion, branch
 * spike/fast-motion-cadence): the vault needle and a digit-slot label
 * routinely invalidate in the SAME cycle at unrelated y ranges (this is
 * exactly what region_span_add's disjoint-span tracking exists for). A scan
 * position sitting in the GAP between two such spans can be genuinely LATE
 * for the near span and genuinely EARLY for the far span at the same
 * instant - two independently valid proofs this single blanket [top_row,
 * bottom_row] question cannot see, because "top_row"/"bottom_row" force one
 * yes/no answer about the union of rows nobody may even be writing (the gap
 * rows are never transferred - see region_dbuf_writeback()'s per-span loop).
 * See te_wait_for_region_spans() below, which asks this same early/late
 * question per span instead. It is not a new argument, it is this one
 * applied to the rows the write actually touches instead of their
 * enclosing box.
 */

/*
 * Fix 3 (spike/fast-motion-cadence): per-span instead of per-cycle-union.
 *
 * Same proof as Fix 2 above, applied per span instead of once to the whole
 * cycle's [top_row, bottom_row] union. The two directions are NOT symmetric
 * once there is more than one span, because region_dbuf_writeback() transfers
 * spans SEQUENTIALLY (sorted by y0) and does not spend any time on the rows
 * in a gap between them:
 *
 *   - LATE only needs "the scan, AS OF RIGHT NOW, has already passed this
 *     span's bottom row". The current (unprojected) scan estimate is
 *     conservative here: the scan only moves forward, so if it has already
 *     passed a row now, it stays past that row for the rest of this pass no
 *     matter when this particular span's transfer actually starts.
 *
 *   - EARLY needs "the scan has not yet reached this span's top row AT THE
 *     MOMENT this span's transfer actually begins" - which, for the second
 *     and later spans, is NOT now; it is after every earlier span in the
 *     burst has already gone out. Using the CURRENT scan estimate for a
 *     later span's EARLY test would be unsafe in the wrong direction - it
 *     would UNDERSTATE how far the scan will really have moved by the time
 *     the write gets there. So the estimate used for span i's EARLY test is
 *     projected forward by a deliberately pessimistic estimate of how long
 *     transferring spans 0..i-1 takes: their combined row count PLUS a fixed
 *     overhead per BOOST_LVGL_BUF_LINES-row chunk (each one pays its own
 *     CASET/RASET/RAMWR intercept - see the README's Animation performance
 *     contract), both expressed in the SCAN's own (slower, ~35.95 us/row)
 *     row time rather than the write's (faster, ~31.3 us/row measured) row
 *     time, and BOOST_REGION_DBUF_CHUNK_OVERHEAD_US rounded UP from the
 *     ~106 us measured intercept. Every one of those substitutions makes the
 *     projection arrive LATER than reality, never earlier: overestimating
 *     elapsed time can only make a later span's EARLY test harder to pass,
 *     never wrongly satisfy it.
 *
 * Provably at least as safe as the Fix 2 union version, not just faster:
 * union-EARLY used the first span's own y0 (top_row already equals it, since
 * spans are sorted by y0) - so union-EARLY implies THIS span's EARLY test
 * for span 0 (rows_before is 0 there, identical to Fix 2), and once the
 * write is ahead of the scan for the very first row it touches, every span
 * after it only gains lead time (a skipped gap costs the write ~0 rows while
 * the scan keeps moving), so union-EARLY implies every later span's EARLY
 * test too. Union-LATE used the LAST span's y1 - if the scan has already
 * passed the highest row the whole burst touches, it has necessarily already
 * passed every lower span's rows as well, so union-LATE implies every span's
 * own LATE test. Anything the union proof could skip, this can still skip;
 * this additionally skips cycles where no single blanket claim holds but
 * every individual span still has its own valid proof (the needle-is-late /
 * far-label-is-early case above) - a strictly more precise version of the
 * exact same argument, not a looser one. Any span that cannot be proved
 * either way still forces the real wait below - never worse than Fix 2.
 */
#define BOOST_REGION_DBUF_CHUNK_OVERHEAD_US 150u

static void te_wait_for_region_spans(const region_span_t *spans, int count)
{
    if (!s_te_active || !s_te_enabled) {
        return;
    }

    ++s_te_waits;

    if (s_te_edges != 0 && s_te_row_time_ns > 0) {
        /* age is bounded to roughly one TE period (~16.75 ms) in normal
         * operation; *1000 keeps this comfortably inside uint32_t even with
         * an order of magnitude of slack, same as Fix 2. */
        const uint32_t age = (uint32_t)esp_timer_get_time() - s_te_last_edge_us;
        const int32_t scan_row_now = (int32_t)((age * 1000u) / s_te_row_time_ns);
        const uint32_t chunk_overhead_rows =
            (uint32_t)((uint64_t)BOOST_REGION_DBUF_CHUNK_OVERHEAD_US * 1000u / s_te_row_time_ns);

        uint32_t rows_before = 0;   /* pessimistic elapsed-time-so-far, in scan-row units */
        bool all_provable = true;
        for (int i = 0; i < count; ++i) {
            const int y0 = spans[i].y0;
            const int y1 = spans[i].y1;
            const int height = y1 - y0;

            bool span_ok = (scan_row_now - (int32_t)BOOST_LCD_TE_ROW_MARGIN >= y1); /* LATE */
            if (!span_ok) {
                const int32_t scan_row_projected = scan_row_now + (int32_t)rows_before;
                span_ok = (scan_row_projected + (int32_t)BOOST_LCD_TE_ROW_MARGIN <= y0); /* EARLY */
            }
            if (!span_ok) {
                all_provable = false;
                break;
            }

            /* Whether this span was proved LATE or EARLY, the write still
             * spends real time transferring it before the next span's
             * transfer can start - accumulate that pessimistic cost for the
             * next span's EARLY projection. */
            const uint32_t chunks = ((uint32_t)height + BOOST_LVGL_BUF_LINES - 1) / BOOST_LVGL_BUF_LINES;
            rows_before += (uint32_t)height + chunks * chunk_overhead_rows;
        }

        if (all_provable) {
            s_te_miss_streak = 0;
            ++s_te_skips;
            return;
        }
    } else if (s_te_edges != 0) {
        /* Period not measured yet (first ~2 s after boot): same fixed-window
         * fallback te_wait_for_vblank() uses. */
        const uint32_t age = (uint32_t)esp_timer_get_time() - s_te_last_edge_us;
        if (age < BOOST_LCD_TE_FRESH_US) {
            s_te_miss_streak = 0;
            ++s_te_skips;
            return;
        }
    }

    /* Drop the stale edge so the wait below returns on a fresh one. */
    (void)xSemaphoreTake(s_te_sem, 0);

    if (xSemaphoreTake(s_te_sem, pdMS_TO_TICKS(BOOST_LCD_TE_TIMEOUT_MS)) == pdTRUE) {
        s_te_miss_streak = 0;
        return;
    }

    ++s_te_timeouts;
    if (s_te_miss_streak < BOOST_LCD_TE_GIVEUP_STREAK) {
        ++s_te_miss_streak;
    }
    if (s_te_miss_streak >= BOOST_LCD_TE_GIVEUP_STREAK && s_te_active) {
        s_te_active = false;
        ESP_LOGW(TAG,
                 "TE: no edge on GPIO%d for %d cycles (%u seen total) - gating off, "
                 "display may tear but will not stall",
                 BOOST_LCD_TE_GPIO, BOOST_LCD_TE_GIVEUP_STREAK, (unsigned)s_te_edges);
    }
}

/*
 * Pushes the accumulated spans to the panel back-to-back, full width, in the
 * same 20-line chunks the production path always uses. One TE wait for the
 * whole burst, taken here (right before the burst starts) rather than at the
 * first strip's rasterisation - waiting earlier would let the ~13 ms raster
 * pass go stale against the edge it waited for, defeating the point of
 * waiting at all. Spans are visited top-to-bottom so the burst writes in
 * panel scan order, matching the race-margin arithmetic in the block comment
 * above.
 *
 * Buffer-reuse safety for s_region_xfer_bufs relies on the SPI driver's own
 * queue depth backpressure, not a completion callback: with exactly
 * CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH round-robin buffers, submitting chunk N
 * cannot return until a queue slot frees, which only happens once chunk
 * (N - QUEUE_DEPTH) - the transfer that owns the SAME buffer - has actually
 * completed. No new completion signalling was added; the existing adapter
 * on_color_trans_done registration still fires for these transfers and calls
 * lv_display_flush_ready(), which is `disp->flushing = 0` in this LVGL build
 * (CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS is off) - idempotent, so the
 * extra calls this generates are harmless no-ops, not new state to manage.
 */
static void region_dbuf_writeback(void)
{
    /* Small N (<= BOOST_REGION_DBUF_MAX_SPANS): selection sort by y0 is
     * plenty cheap and keeps the burst's row order (and hence the scan-race
     * margin, and the per-span EARLY projection's cumulative-rows-before
     * ordering) predictable. Sorted BEFORE the TE-wait decision below (Fix 3)
     * - te_wait_for_region_spans() walks the spans in transfer order, which
     * is only meaningful once this is sorted. */
    for (int i = 0; i < s_region_span_count - 1; ++i) {
        int min_idx = i;
        for (int j = i + 1; j < s_region_span_count; ++j) {
            if (s_region_spans[j].y0 < s_region_spans[min_idx].y0) min_idx = j;
        }
        if (min_idx != i) {
            const region_span_t tmp = s_region_spans[i];
            s_region_spans[i] = s_region_spans[min_idx];
            s_region_spans[min_idx] = tmp;
        }
    }

    if (s_te_gate_armed) {
        s_te_gate_armed = false;
        /* s_region_span_count == 0 cannot happen here: the caller
         * (display_metrics_event_cb) only invokes region_dbuf_writeback()
         * when s_region_has_data is true, which te_draw_bitmap_cb only sets
         * alongside adding at least one span. Guarded anyway rather than
         * trusting that invariant across a future refactor. */
        te_wait_for_region_spans(s_region_spans, s_region_span_count);
    }

    for (int s = 0; s < s_region_span_count; ++s) {
        const int x0 = s_region_spans[s].x0;
        const int x1 = s_region_spans[s].x1;
        const int width = x1 - x0;
        int y0 = s_region_spans[s].y0;
        const int y_end = s_region_spans[s].y1;
        while (y0 < y_end) {
            int lines = BOOST_LVGL_BUF_LINES;
            if (y0 + lines > y_end) {
                lines = y_end - y0;
            }
            uint16_t *xfer = s_region_xfer_bufs[s_region_xfer_idx % BOOST_REGION_DBUF_QUEUE_DEPTH];
            ++s_region_xfer_idx;
            /* Per-span width can be narrower than the panel (the vault
             * needle's radial-wedge invalidation is column-narrow), so copy
             * row by row rather than one block memcpy: canvas rows are
             * BSP_LCD_H_RES wide, the transfer buffer only needs `width`
             * columns packed tight. */
            for (int r = 0; r < lines; ++r) {
                memcpy(xfer + (size_t)r * width,
                       s_region_canvas + (size_t)(y0 + r) * BSP_LCD_H_RES + x0,
                       (size_t)width * sizeof(uint16_t));
            }
            const esp_err_t ret = esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y0 + lines, xfer);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "region-dbuf: draw_bitmap failed at x=%d y=%d: %s",
                         x0, y0, esp_err_to_name(ret));
                break;
            }
            ++s_flush_count;
            s_pixel_count += (uint32_t)width * (uint32_t)lines;
            y0 += lines;
        }
    }
    s_region_span_count = 0;
}

/*
 * Replaces the adapter's own esp_lcd_panel_draw_bitmap() call. Contract from
 * esp_lv_adapter_display.h: return ESP_OK once the blit is under way and the
 * completion ISR will finish the handshake; return anything else and the
 * adapter unblocks the flush itself. So this must not swallow errors.
 */
static esp_err_t te_draw_bitmap_cb(lv_display_t *disp, esp_lcd_panel_handle_t panel,
                                   int x_start, int y_start, int x_end, int y_end,
                                   const void *color_map, void *user_ctx)
{
    (void)disp;
    (void)user_ctx;

    if (s_region_dbuf_enabled && s_region_canvas != NULL) {
        /* Accumulate into the PSRAM canvas; defer the real transfer to
         * LV_EVENT_RENDER_READY so the whole dirty region goes out
         * back-to-back instead of interleaved with rasterisation. Leave
         * s_te_gate_armed untouched here - region_dbuf_writeback() consumes
         * it later, once, right before the real burst. color_map arrives
         * already stride-compacted to exactly (x_end-x_start) columns per
         * row (display_bridge_v9_flush_default calls compact_stride_to_packed
         * before invoking this hook), so a tight per-row copy is correct. */
        const int rows = y_end - y_start;
        const int cols = x_end - x_start;
        const uint16_t *src = (const uint16_t *)color_map;
        uint16_t *dst_row = s_region_canvas + (size_t)y_start * BSP_LCD_H_RES + x_start;
        if (cols == BSP_LCD_H_RES) {
            /* Full panel width: source (stride-compacted to `cols`) and
             * destination (canvas stride is exactly BSP_LCD_H_RES) are both
             * contiguous across the whole strip, so this collapses to one
             * memcpy instead of `rows` of them. See the "Fix 1 (investigated,
             * NOT implemented)" block comment above - this is the fallback
             * optimisation for copy 1, not a replacement for eliminating it. */
            memcpy(dst_row, src, (size_t)rows * (size_t)cols * sizeof(uint16_t));
        } else {
            /* Narrower than the panel (e.g. the vault needle's column-narrow
             * wedge): destination rows are BSP_LCD_H_RES apart, not `cols`
             * apart, so they cannot be collapsed - copy row by row. */
            for (int r = 0; r < rows; ++r) {
                memcpy(dst_row + (size_t)r * BSP_LCD_H_RES, src + (size_t)r * cols,
                       (size_t)cols * sizeof(uint16_t));
            }
        }
        region_span_add(x_start, y_start, x_end, y_end);
        s_region_has_data = true;
        return ESP_ERR_NOT_ALLOWED;
    }

    if (s_te_gate_armed) {
        s_te_gate_armed = false;
        te_wait_for_vblank();
    }
    return esp_lcd_panel_draw_bitmap(panel, x_start, y_start, x_end, y_end, color_map);
}

/* Report the panel's measured frame period once, from the LVGL task. */
static void te_log_period_once(void)
{
    if (s_te_period_logged || s_te_probe_n < BOOST_LCD_TE_PROBE_N) {
        return;
    }
    s_te_period_logged = true;
    const uint32_t mean_us = s_te_probe_sum_us / BOOST_LCD_TE_PROBE_N;
    if (mean_us > 0) {
        /* Row time for te_wait_for_region_spans()'s scan-position estimate.
         * mean_us * 1000 / BSP_LCD_V_RES: e.g. 16,753 us * 1000 / 466 =
         * 35,952 ns/row, matching the panel's independently-measured
         * ~35.95 us/row scan rate. */
        s_te_row_time_ns = (mean_us * 1000u) / BSP_LCD_V_RES;
        ESP_LOGI(TAG, "TE period %u us measured over %u edges (%u.%u Hz panel scan)",
                 (unsigned)mean_us, (unsigned)BOOST_LCD_TE_PROBE_N,
                 (unsigned)(1000000u / mean_us),
                 (unsigned)((10000000u / mean_us) % 10u));
    }
    s_metrics.te_period_us = mean_us;
}
#endif /* BOOST_LCD_USE_TE */

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
#if BOOST_LCD_USE_TE
        /* Arm the gate for this cycle. Consumed either by the first strip's
         * blit (region-dbuf off: te_draw_bitmap_cb) or once, later, by
         * region_dbuf_writeback() right before its burst (region-dbuf on).
         * Same task as the flush, so a plain flag is sufficient. */
        s_te_gate_armed = true;
#endif
        s_region_has_data = false;
        s_region_span_count = 0;
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
        if (s_region_dbuf_enabled && s_region_canvas != NULL && s_region_has_data) {
            /* Included in worst_render_us below by design: this is the real
             * transfer for the cycle, and the metric is supposed to capture
             * total cycle cost, not just rasterisation. */
            region_dbuf_writeback();
        }
        s_region_has_data = false;
        ++s_render_count;
        /* Duration of the cycle itself, not the gap between cycles: an idle
         * screen produces long gaps but no stall, and conflating the two is
         * what made the first cut of this metric useless. */
        if (s_last_render_us != 0) {
            const uint32_t dur = (uint32_t)(esp_timer_get_time() - s_last_render_us);
            if (dur > s_worst_gap_us) s_worst_gap_us = dur;
        }
    } else if (code == LV_EVENT_FLUSH_START) {
        /* When region-dbuf is engaged, every strip in the cycle still raises
         * this event even though te_draw_bitmap_cb skipped the real transfer
         * (it returns ESP_ERR_NOT_ALLOWED). Counting here too would double
         * flushesPerSecond/pixelsPerSecond against region_dbuf_writeback()'s
         * own accounting for the real burst, so skip it in that mode. */
        if (!(s_region_dbuf_enabled && s_region_canvas != NULL)) {
            const lv_area_t *area = lv_event_get_param(e);
            if (area != NULL) {
                ++s_flush_count;
                s_pixel_count += (uint32_t)lv_area_get_width(area) * (uint32_t)lv_area_get_height(area);
            }
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
#if BOOST_LCD_USE_TE
        te_log_period_once();
        s_metrics.te_waits = s_te_waits;
        s_metrics.te_timeouts = s_te_timeouts;
        s_metrics.te_skips = s_te_skips;
        s_te_waits = 0;
        s_te_timeouts = 0;
        s_te_skips = 0;
#endif
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
    /* Tearing Effect Line ON, V-blank only. */
    {0x35, (uint8_t[]){0x00}, 1, 0},
#if BOOST_LCD_USE_TE && (BOOST_LCD_TE_SCANLINE >= 0)
    /* set_tear_scanline: assert TE as the scan crosses this line instead of at
     * V-blank, so the write starts just ahead of the scan. STS is 9 bits. */
    {0x44, (uint8_t[]){(uint8_t)((BOOST_LCD_TE_SCANLINE >> 8) & 0x01),
                       (uint8_t)(BOOST_LCD_TE_SCANLINE & 0xFF)}, 2, 0},
#endif
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

void boost_display_set_te(bool enabled)
{
#if BOOST_LCD_USE_TE
    s_te_enabled = enabled;
    ESP_LOGI(TAG, "TE sync %s (runtime)", enabled ? "enabled" : "disabled");
#else
    (void)enabled;
#endif
}

bool boost_display_te(void)
{
#if BOOST_LCD_USE_TE
    return s_te_enabled && s_te_active;
#else
    return false;
#endif
}

/*
 * Lock around the enable/disable transition: the LVGL/adapter task
 * dereferences s_region_canvas / s_region_xfer_bufs from the same context
 * this lock already serialises against (te_draw_bitmap_cb and
 * region_dbuf_writeback both run under it), so freeing them while a render
 * cycle is mid-accumulation would be a use-after-free without it. A failed
 * PSRAM allocation degrades to the existing per-strip path with a warning,
 * the same pattern already used for the cached face backgrounds, rather than
 * failing boot or the request.
 */
void boost_display_set_region_dbuf(bool enabled)
{
    if (boost_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "region-dbuf: could not acquire display lock, toggle skipped");
        return;
    }
    s_region_dbuf_enabled = enabled;
    if (enabled) {
        if (!region_dbuf_alloc()) {
            s_region_dbuf_enabled = false;
        }
    } else {
        region_dbuf_free();
    }
    boost_display_unlock();
    ESP_LOGI(TAG, "region double-buffer %s (runtime)", s_region_dbuf_enabled ? "enabled" : "disabled");
}

bool boost_display_region_dbuf(void)
{
    return s_region_dbuf_enabled && s_region_canvas != NULL;
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

/* Persisted rotation -> adapter enum. Quarter turns only; the adapter maps them
 * onto the panel scan order, so they cost nothing per frame. Anything else is
 * treated as 0 rather than guessed at. */
static esp_lv_adapter_rotation_t rotation_setting(void)
{
    switch (boost_theme_rotation()) {
    case 90:  return ESP_LV_ADAPTER_ROTATE_90;
    case 180: return ESP_LV_ADAPTER_ROTATE_180;
    case 270: return ESP_LV_ADAPTER_ROTATE_270;
    default:  return ESP_LV_ADAPTER_ROTATE_0;
    }
}

static lv_display_t *register_display(void)
{
    ESP_RETURN_ON_FALSE(panel_new() == ESP_OK, NULL, TAG, "panel_new failed");

    esp_lv_adapter_display_config_t adapter_disp = {
        .panel = s_panel,
        .panel_io = s_panel_io,
        .profile = {
            .interface = ESP_LV_ADAPTER_PANEL_IF_OTHER,
            .rotation = rotation_setting(),
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
    ESP_LOGI(TAG, "LVGL %dx%d partial, %d lines, internal DMA buffers, strip=%u B, rotation %u deg",
             BSP_LCD_H_RES, BSP_LCD_V_RES, BOOST_LVGL_BUF_LINES,
             (unsigned)BOOST_LVGL_STRIP_BYTES, (unsigned)boost_theme_rotation());

#if BOOST_LCD_USE_TE
    if (te_init() != ESP_OK) {
        ESP_LOGW(TAG, "TE sync unavailable; continuing without it");
    } else {
        const esp_lv_adapter_draw_bitmap_callbacks_t te_cbs = {
            .custom_draw_bitmap = te_draw_bitmap_cb,
        };
        if (esp_lv_adapter_set_draw_bitmap_callbacks(disp, &te_cbs, NULL) != ESP_OK) {
            s_te_active = false;
            ESP_LOGW(TAG, "TE draw-bitmap hook rejected; TE sync inactive");
        }
    }
#else
    ESP_LOGI(TAG, "TE sync disabled at compile time (BOOST_LCD_USE_TE=0)");
#endif
    return disp;
}

static lv_indev_t *register_touch(lv_display_t *disp)
{
    bsp_display_cfg_t touch_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        /* Must match the display, or touch lands on the pre-rotation coords. */
        .rotation = rotation_setting(),
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
