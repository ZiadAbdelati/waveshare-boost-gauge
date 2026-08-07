/**
 * Desktop LVGL simulator for the boost gauge UI.
 *
 * Modes:
 *   --screenshot DIR   render fixed PSI states to DIR/*.raw + convert helper
 *   --window           open SDL window (needs display / xvfb)
 *   default            headless screenshot into ../preview/sim
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#ifdef SIM_HAVE_SDL
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#endif

#include "boost_gauge.h"
#include "boost_page.h"
#include "boost_sim.h"
#include "boost_theme.h"
#include "boost_tpms.h"
#include "boost_tpms_mock.h"

#ifdef _WIN32
#include <direct.h>
#define sim_mkdir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define sim_mkdir(p) mkdir((p), 0775)
#endif

#define DISP_W 466
#define DISP_H 466

typedef struct {
    float psi;
    float peak;
    const char *name;
} shot_state_t;

static const shot_state_t k_states[] = {
    { -12.0f, 0.0f, "vac" },
    { 0.0f, 0.0f, "atmo" },
    { 8.5f, 8.5f, "boost" },
    { 19.5f, 19.5f, "over" },
};

static void pump_lvgl(uint32_t ms)
{
    const uint32_t step = 5;
    for (uint32_t t = 0; t < ms; t += step) {
        lv_tick_inc(step);
        lv_timer_handler();
        usleep(step * 1000);
    }
}

static bool write_raw_rgba(const char *path, const uint8_t *px, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return false;
    }
    /* simple header: "RGBA" + w + h little-endian, then pixels */
    const char magic[4] = { 'R', 'G', 'B', 'A' };
    const uint32_t ww = (uint32_t)w;
    const uint32_t hh = (uint32_t)h;
    fwrite(magic, 1, 4, f);
    fwrite(&ww, 4, 1, f);
    fwrite(&hh, 4, 1, f);
    fwrite(px, 1, (size_t)w * (size_t)h * 4, f);
    fclose(f);
    return true;
}

static bool snapshot_screen(const char *path)
{
    lv_obj_t *scr = lv_screen_active();
    lv_draw_buf_t *buf = lv_snapshot_take(scr, LV_COLOR_FORMAT_ARGB8888);
    if (buf == NULL) {
        fprintf(stderr, "lv_snapshot_take failed for %s\n", path);
        return false;
    }

    const bool ok = write_raw_rgba(path, buf->data, (int)buf->header.w, (int)buf->header.h);
    lv_draw_buf_destroy(buf);
    return ok;
}

/*
 * Hold a fixed reading for `ms`, sampling at the firmware's 16 ms cadence.
 *
 * A single boost_gauge_update() followed by an idle pump is not what the device
 * does, and it is not enough for any face with a time-based element: the Aurora
 * odometer starts a roll on the update that changes a digit and needs the
 * following ticks to carry it home, so a one-shot update would freeze every
 * screenshot with the wheels part-way round.
 */
static void hold_state(const boost_sample_t *sample, uint32_t ms)
{
    const uint32_t step = 16;
    for (uint32_t t = 0; t < ms; t += step) {
        boost_gauge_update(sample);
        lv_tick_inc(step);
        lv_timer_handler();
        usleep(step * 1000);
    }
}

static void apply_state(const shot_state_t *st)
{
    boost_sample_t sample = {
        .psi = st->psi,
        .peak_psi = st->peak,
        .demo = true,
    };
    hold_state(&sample, 320);
}

static bool render_tpms_state(const char *out_dir, boost_tpms_mock_scenario_t scenario,
                              const char *name);

static int run_screenshots(const char *out_dir, const char *theme_id)
{
    /* Portable: "mkdir -p" is not available on the Windows shell. */
    sim_mkdir(out_dir);

    boost_sim_init();
    boost_page_create();
    if (theme_id != NULL) {
        const boost_theme_t *t = boost_theme_find(theme_id);
        if (t == NULL) {
            fprintf(stderr, "unknown theme: %s\n", theme_id);
            return 1;
        }
        boost_gauge_apply_theme(t);
    }
    pump_lvgl(50);

    for (size_t i = 0; i < sizeof(k_states) / sizeof(k_states[0]); i++) {
        apply_state(&k_states[i]);
        char path[512];
        snprintf(path, sizeof(path), "%s/gauge_%s.raw", out_dir, k_states[i].name);
        if (!snapshot_screen(path)) {
            return 2;
        }
        printf("wrote %s (psi=%+.1f)\n", path, k_states[i].psi);
    }

    /* short animated sweep as sequential raw frames */
    char anim_dir[512];
    snprintf(anim_dir, sizeof(anim_dir), "%s/frames", out_dir);
    sim_mkdir(anim_dir);

    for (int i = 0; i < 24; i++) {
        float t = (float)i / 23.0f;
        float psi = -14.5f + (22.0f + 14.5f) * (0.5f + 0.5f * sinf(t * 6.2831853f - 1.5707963f));
        shot_state_t st = { psi, 14.0f, "frame" };
        apply_state(&st);
        char path[512];
        snprintf(path, sizeof(path), "%s/frame_%02d.raw", anim_dir, i);
        if (!snapshot_screen(path)) {
            return 3;
        }
    }
    printf("wrote %s/frame_*.raw\n", anim_dir);

    /* TPMS page, one shot per mock scenario. */
    boost_tpms_init();
    static const struct {
        boost_tpms_mock_scenario_t scenario;
        const char *name;
    } tpms_states[] = {
        { BOOST_TPMS_MOCK_NORMAL, "normal" },
        { BOOST_TPMS_MOCK_STALE, "stale" },
        { BOOST_TPMS_MOCK_DISCONNECTED, "disconnected" },
    };
    for (size_t i = 0; i < sizeof(tpms_states) / sizeof(tpms_states[0]); i++) {
        if (!render_tpms_state(out_dir, tpms_states[i].scenario, tpms_states[i].name)) {
            return 4;
        }
    }
    boost_page_show(BOOST_PAGE_BOOST);
    return 0;
}

/*
 * Drive the mock TPMS provider long enough for its scenario to settle, then
 * snapshot the TPMS page. NORMAL needs a few ticks for the wobble to land;
 * STALE publishes once and must age past the 5 s window; DISCONNECTED never
 * publishes. The tick cadence matches the firmware's 250 ms TPMS timer.
 */
static bool render_tpms_state(const char *out_dir, boost_tpms_mock_scenario_t scenario,
                              const char *name)
{
    boost_tpms_mock_set_scenario(scenario);
    for (uint32_t t = 0; t < 7000; t += 250) {
        boost_tpms_mock_tick(t);
        lv_tick_inc(250);
        lv_timer_handler();
        usleep(1000);
    }
    boost_page_show(BOOST_PAGE_TPMS);
    /* The page coordinator only forwards a snapshot while the TPMS page is
     * active, so push the now-settled snapshot after switching to it. */
    boost_tpms_snapshot_t snapshot;
    boost_tpms_get_snapshot(&snapshot);
    boost_page_update_tpms(&snapshot);
    pump_lvgl(100);
    char path[512];
    snprintf(path, sizeof(path), "%s/tpms_%s.raw", out_dir, name);
    if (!snapshot_screen(path)) {
        return false;
    }
    printf("wrote %s (scenario=%s)\n", path, name);
    return true;
}

#ifdef SIM_HAVE_SDL
static int run_window(void)
{
    lv_display_t *disp = lv_sdl_window_create(DISP_W, DISP_H);
    if (disp == NULL) {
        fprintf(stderr, "lv_sdl_window_create failed (need display or xvfb-run)\n");
        return 1;
    }
    lv_sdl_window_set_title(disp, "Boost Gauge Sim");
    lv_indev_t *mouse = lv_sdl_mouse_create();
    (void)mouse;

    boost_sim_init();
    boost_tpms_init();
    boost_page_create();

    uint32_t now = 0;
    while (1) {
        const boost_sample_t sample = boost_sim_tick();
        boost_page_update(&sample);
        if ((now % 250) == 0) {
            boost_tpms_mock_tick(now);
            boost_tpms_snapshot_t snapshot;
            boost_tpms_get_snapshot(&snapshot);
            boost_page_update_tpms(&snapshot);
        }
        lv_timer_handler();
        lv_tick_inc(16);
        now += 16;
        usleep(16000);
    }
    return 0;
}
#endif /* SIM_HAVE_SDL */

/* Wall-clock helper for per-render-cycle cost in --audit (host raster speed,
 * used for RELATIVE A/B comparisons, not as device milliseconds). */
static double sim_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/**
 * Headless path: custom memory display, no SDL window.
 */
static uint8_t s_fb[DISP_W * DISP_H * 4];
/* Flushed-pixel accounting for --audit. Reset by the caller each render cycle. */
static uint64_t s_flush_px;

static void headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LV_UNUSED(disp);
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    s_flush_px += (uint64_t)w * (uint64_t)h;
    uint8_t *dst = s_fb + ((area->y1 * DISP_W + area->x1) * 4);
    const uint8_t *src = px_map;
    for (int32_t y = 0; y < h; y++) {
        memcpy(dst, src, (size_t)w * 4);
        dst += DISP_W * 4;
        src += w * 4;
    }
    lv_display_flush_ready(disp);
}

static void setup_headless_display(void)
{
    lv_display_t *disp = lv_display_create(DISP_W, DISP_H);
    lv_display_set_flush_cb(disp, headless_flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_ARGB8888);
    /* partial buffer: one tenth of the screen */
    static uint8_t buf1[DISP_W * 40 * 4];
    lv_display_set_buffers(disp, buf1, NULL, sizeof(buf1), LV_DISPLAY_RENDER_MODE_PARTIAL);
}

/*
 * Trail and cost audit.
 *
 * The screenshot path uses lv_snapshot_take(), which re-renders the whole tree
 * into a fresh buffer - so it can never show a stale pixel, because a stale
 * pixel is by definition something the partial-refresh path failed to repaint.
 * This mode compares the two: `s_fb` is the accumulated output of the real
 * partial pipeline, and a snapshot at the same instant is ground truth. Any
 * pixel where they disagree is a region an invalidation failed to cover.
 *
 * It also totals flushed pixels per render cycle, which is the throughput
 * figure AGENTS.md asks for (`pixelsPerSecond`), measurable without hardware.
 */
static int run_audit(const char *theme_id, int seconds)
{
    boost_sim_init();
    boost_page_create();
    if (theme_id != NULL) {
        const boost_theme_t *t = boost_theme_find(theme_id);
        if (t == NULL) {
            fprintf(stderr, "unknown theme: %s\n", theme_id);
            return 1;
        }
        if (t->style == BOOST_STYLE_VAULT) {
            /* Reproduce switching into Vault-Tec while the reading is already
             * nonzero. The scene must render the committed needle position
             * before the next sample arrives. */
            boost_sample_t pressure = { .psi = 8.0f, .peak_psi = 8.0f, .demo = true };
            hold_state(&pressure, 64);
            boost_gauge_apply_theme(t);
            pump_lvgl(50);
            const float initial_deg = boost_gauge_host_vault_needle_deg();
            boost_gauge_update(&pressure);
            const float sampled_deg = boost_gauge_host_vault_needle_deg();
            const float jump_deg = fabsf(sampled_deg - initial_deg);
            printf("theme switch vault-tec first-sample needle jump=%.3f deg\n",
                   (double)jump_deg);
            if (jump_deg > 0.001f) return 2;
        } else if (t->style == BOOST_STYLE_HUD) {
            /* Reproduce switching into Night City while already in vacuum.
             * Pump the rebuild before its first sample, matching the ordering
             * that exposed a missing initial full-span invalidation on-device. */
            boost_sample_t vacuum = { .psi = -8.0f, .demo = true };
            hold_state(&vacuum, 64);
            boost_gauge_apply_theme(t);
            pump_lvgl(50);
            hold_state(&vacuum, 64);
        } else {
            boost_gauge_apply_theme(t);
        }
    }
    /* Let the build settle so the scene-build repaint is not charged to the
     * steady-state figures (AGENTS.md: discard the first samples after a PUT). */
    boost_sample_t warm = { 0 };
    if (theme_id != NULL && strcmp(theme_id, "night-city") == 0) warm.psi = -8.0f;
    warm.demo = true;
    hold_state(&warm, 500);

    const int frames = seconds * 62;      /* 16 ms sample cadence */
    uint64_t px_total = 0;
    uint64_t px_max = 0;
    uint32_t rendered = 0;
    /* Per-cycle (ms, px) pairs for render-cost A/B. Host raster speed is not
     * device speed, but the RELATIVE mix (which cycles are expensive) and the
     * delta when a draw path changes are meaningful. */
    typedef struct { double ms; uint64_t px; } cycle_t;
    cycle_t *cycles = NULL;
    size_t cycles_cap = 0, cycles_n = 0;
    uint32_t over16 = 0;
    double ms_total = 0.0, ms_max = 0.0;
    uint64_t stale_px_total = 0;
    uint32_t stale_frames = 0;
    uint64_t stale_worst = 0;
    uint32_t compares = 0;
    uint64_t severe = 0;
    int reported = 0;
#if BOOST_NEON_DRAW_STATS
    /* Flushed pixels measure the dirty AREA. These measure the work done inside
     * it: callback invocations (one per dirty region) and ring segments
     * submitted (three arc primitives each). */
    extern uint32_t g_neon_cb_calls, g_neon_arcs, g_neon_labels;
    extern uint32_t g_neon_sign_bars, g_neon_sprite_blits;
    uint64_t cb_total = 0, arc_total = 0, label_total = 0;
    uint64_t sign_total = 0, sprite_total = 0;
    uint32_t cb_max = 0, arc_max = 0, label_max = 0;
    uint32_t sign_max = 0, sprite_max = 0;
#endif

    for (int i = 0; i < frames; ++i) {
        const float t = (float)i / 62.0f;
        /* Full-range sweep in both directions, crossing zero and the overboost
         * threshold repeatedly, plus a faster ripple so digits change at
         * different rates - which is exactly what a per-slot invalidation gets
         * wrong when it is wrong. */
        const float span = 19.0f;
        float psi = -14.0f + span * (0.5f + 0.5f * sinf(t * 1.35f));
        psi += 0.9f * sinf(t * 11.0f) + 0.35f * sinf(t * 23.0f);

        boost_sample_t sample = { 0 };
        sample.psi = psi;
        sample.peak_psi = psi > 0.0f ? psi : 0.0f;
        sample.demo = true;
        boost_gauge_update(&sample);

        s_flush_px = 0;
#if BOOST_NEON_DRAW_STATS
        g_neon_cb_calls = 0; g_neon_arcs = 0; g_neon_labels = 0;
        g_neon_sign_bars = 0; g_neon_sprite_blits = 0;
#endif
        lv_tick_inc(16);
        const double t0 = sim_now_ms();
        lv_timer_handler();
        const double dt = sim_now_ms() - t0;
        if (s_flush_px > 0) {
            if (cycles_n == cycles_cap) {
                cycles_cap = cycles_cap ? cycles_cap * 2 : 1024;
                cycles = (cycle_t *)realloc(cycles, cycles_cap * sizeof(cycle_t));
            }
            cycles[cycles_n].ms = dt;
            cycles[cycles_n].px = s_flush_px;
            cycles_n++;
            ms_total += dt;
            if (dt > ms_max) ms_max = dt;
            if (dt > 16.0) over16++;
        }
#if BOOST_NEON_DRAW_STATS
        if (s_flush_px > 0) {
            cb_total += g_neon_cb_calls;
            arc_total += g_neon_arcs;
            label_total += g_neon_labels;
            sign_total += g_neon_sign_bars;
            sprite_total += g_neon_sprite_blits;
            if (g_neon_cb_calls > cb_max) cb_max = g_neon_cb_calls;
            if (g_neon_arcs > arc_max) arc_max = g_neon_arcs;
            if (g_neon_labels > label_max) label_max = g_neon_labels;
            if (g_neon_sign_bars > sign_max) sign_max = g_neon_sign_bars;
            if (g_neon_sprite_blits > sprite_max) sprite_max = g_neon_sprite_blits;
        }
#endif
        if (s_flush_px > 0) {
            px_total += s_flush_px;
            if (s_flush_px > px_max) px_max = s_flush_px;
            rendered++;
        }

        if ((i % 3) == 0) {
            lv_draw_buf_t *truth = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
            if (truth != NULL) {
                uint64_t bad = 0;
                for (int32_t p = 0; p < DISP_W * DISP_H; ++p) {
                    /* Compare RGB only: the snapshot's alpha channel is its own
                     * composition result and is not what reaches the panel. */
                    const uint8_t *a = s_fb + (size_t)p * 4;
                    const uint8_t *b = truth->data + (size_t)p * 4;
                    /* Compare as the panel sees it. The framebuffer here is
                     * ARGB8888, but the CO5300 is RGB565: a 1-2/255 difference
                     * on an antialiased edge - which is what a dirty-region
                     * boundary bisecting an AA pixel produces - quantises to
                     * the same 565 value and never reaches the glass. Judging
                     * in 8-bit reports those as failures and buries a real
                     * trail in the noise. */
                    const uint16_t a565 = (uint16_t)(((a[2] & 0xF8) << 8) |
                                                     ((a[1] & 0xFC) << 3) | (a[0] >> 3));
                    const uint16_t b565 = (uint16_t)(((b[2] & 0xF8) << 8) |
                                                     ((b[1] & 0xFC) << 3) | (b[0] >> 3));
                    if (a565 != b565) {
                        bad++;
                        /* Severity, not just a count. A trail is unrepainted
                         * *content* - a needle's worth of ink left behind, many
                         * channel steps away from the truth. A boundary that
                         * bisects an antialiased edge produces a single 565
                         * step on one or two isolated pixels. Both are
                         * "mismatch"; only the first is a bug, and a bare count
                         * cannot tell them apart. */
                        const int dr = abs((int)(a565 >> 11) - (int)(b565 >> 11));
                        const int dg = abs((int)((a565 >> 5) & 0x3F) - (int)((b565 >> 5) & 0x3F));
                        const int db = abs((int)(a565 & 0x1F) - (int)(b565 & 0x1F));
                        const int worst_ch = dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
                        if (worst_ch > 1) severe++;
                        if (reported < 12) {
                            const int32_t x = p % DISP_W, y = p / DISP_W;
                            const double dx = x - (DISP_W / 2.0);
                            const double dy = y - (DISP_H / 2.0);
                            printf("    mismatch frame=%d at (%d,%d) r=%.1f bearing=%.1f deg "
                                   "d565=%d/%d/%d %s\n",
                                   i, x, y, sqrt(dx * dx + dy * dy),
                                   atan2(dy, dx) * 180.0 / M_PI, dr, dg, db,
                                   worst_ch > 1 ? "SEVERE" : "(1-step AA seam)");
                            reported++;
                        }
                    }
                }
                lv_draw_buf_destroy(truth);
                compares++;
                if (bad > 0) {
                    stale_frames++;
                    stale_px_total += bad;
                    if (bad > stale_worst) stale_worst = bad;
                }
            }
        }
    }

    printf("audit theme=%s seconds=%d\n", theme_id ? theme_id : "(default)", seconds);
    printf("  render cycles      : %u of %d samples\n", rendered, frames);
    if (cycles_n > 0) {
        /* percentile sort on ms */
        cycle_t *tmp = (cycle_t *)malloc(cycles_n * sizeof(cycle_t));
        memcpy(tmp, cycles, cycles_n * sizeof(cycle_t));
        for (size_t a = 1; a < cycles_n; a++) {
            cycle_t k = tmp[a]; size_t b = a;
            while (b > 0 && tmp[b - 1].ms > k.ms) { tmp[b] = tmp[b - 1]; b--; }
            tmp[b] = k;
        }
        printf("  cycle ms           : p50 %.2f  p90 %.2f  max %.2f  over-16ms %u\n",
               tmp[cycles_n / 2].ms, tmp[cycles_n * 9 / 10].ms, ms_max, over16);
        printf("  cost rate          : %.1f us/kpx (host, A/B only)\n",
               ms_total / (double)px_total * 1000.0);
        /* top-5 cycles by flushed pixels with their time */
        cycle_t *bp = (cycle_t *)malloc(cycles_n * sizeof(cycle_t));
        memcpy(bp, cycles, cycles_n * sizeof(cycle_t));
        for (size_t a = 1; a < cycles_n; a++) {
            cycle_t k = bp[a]; size_t b = a;
            while (b > 0 && bp[b - 1].px < k.px) { bp[b] = bp[b - 1]; b--; }
            bp[b] = k;
        }
        printf("  top-5 px cycles    : ");
        for (size_t i = 0; i < (cycles_n < 5 ? cycles_n : 5); i++)
            printf("(px=%llu ms=%.2f) ", (unsigned long long)bp[i].px, bp[i].ms);
        printf("\n");
        free(tmp); free(bp);
    }
    free(cycles);
    printf("  flushed px/cycle   : mean %.0f  max %llu\n",
           rendered ? (double)px_total / (double)rendered : 0.0,
           (unsigned long long)px_max);
#if BOOST_NEON_DRAW_STATS
    printf("  draw callbacks/cyc : mean %.1f  max %u  (one per dirty region)\n",
           rendered ? (double)cb_total / (double)rendered : 0.0, cb_max);
    printf("  ring segments/cyc  : mean %.1f  max %u  (x3 arc primitives each)\n",
           rendered ? (double)arc_total / (double)rendered : 0.0, arc_max);
    printf("  readout labels/cyc : mean %.1f  max %u\n",
           rendered ? (double)label_total / (double)rendered : 0.0, label_max);
    printf("  sign bars/cyc      : mean %.1f  max %u\n",
           rendered ? (double)sign_total / (double)rendered : 0.0, sign_max);
    printf("  sprite blits/cyc   : mean %.1f  max %u  (A8 coverage, BOOST_NEON_GLYPH_SPRITES)\n",
           rendered ? (double)sprite_total / (double)rendered : 0.0, sprite_max);
#endif
    printf("  throughput         : %.3f Mpx/s at 62.5 Hz\n",
           (double)px_total / (double)frames * 62.5 / 1e6);
    printf("  severe mismatches  : %llu px (>1 step in any 565 channel)\n",
           (unsigned long long)severe);
    printf("  stale-pixel check  : %u compares, %u with any mismatch, "
           "total %llu px, worst %llu px\n",
           compares, stale_frames, (unsigned long long)stale_px_total,
           (unsigned long long)stale_worst);
    return (stale_frames == 0) ? 0 : 4;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--screenshot DIR]   headless snapshots (default DIR=preview/sim)\n"
            "  %s --window             SDL window (use xvfb-run if headless)\n"
            "  %s --audit [--seconds N] partial-refresh trail + cost audit\n"
            "  %s --tpms [normal|stale|disconnected]\n"
            "                          snapshot the TPMS page under a mock scenario\n"
            "  (all modes accept --theme ID, --neon-layout tube|segments|marquee,\n"
            "   and --neon-spin to enable the marquee chase for screenshots)\n",
            argv0, argv0, argv0, argv0);
}

/* Standalone TPMS-page mode: build the pages, force page 1, drive the mock
 * provider under the requested scenario and snapshot once settled. */
static int run_tpms(const char *out_dir, const char *scenario_name)
{
    sim_mkdir(out_dir);
    boost_sim_init();
    boost_tpms_init();
    boost_page_create();
    pump_lvgl(50);

    boost_tpms_mock_scenario_t scenario = BOOST_TPMS_MOCK_NORMAL;
    if (scenario_name != NULL) {
        if (strcmp(scenario_name, "stale") == 0) scenario = BOOST_TPMS_MOCK_STALE;
        else if (strcmp(scenario_name, "disconnected") == 0) scenario = BOOST_TPMS_MOCK_DISCONNECTED;
        else if (strcmp(scenario_name, "normal") != 0) {
            fprintf(stderr, "unknown tpms scenario: %s\n", scenario_name);
            return 1;
        }
    }
    if (!render_tpms_state(out_dir, scenario, scenario_name ? scenario_name : "normal")) {
        return 2;
    }
    return 0;
}

/* Fixed-psi marquee chase: hold the OVERBOOST reading (all three rings lit)
 * and snapshot at each 90 ms spin boundary so the accent bulbs walk through
 * all six phase states. The chase starts at phase 0 on scene build, so the
 * first frame is the static stagger. Requires --neon-layout marquee. */
static int run_chase(const char *out_dir)
{
    sim_mkdir(out_dir);
    boost_sim_init();
    boost_page_create();
    pump_lvgl(50);

    /* Reset the scene so the chase starts from phase 0 deterministically. */
    boost_theme_set_neon_marquee_spin(true);
    const boost_theme_t *t = boost_theme_find("neon");
    boost_gauge_apply_theme(t);
    pump_lvgl(50);

    boost_sample_t sample = { .psi = 19.5f, .peak_psi = 19.5f, .demo = true };
    const uint32_t step = 16;
    uint32_t last_tick = lv_tick_get();
    int shot = 0;
    for (uint32_t elapsed = 0; elapsed < 9 * 90 + 60; elapsed += step) {
        boost_gauge_update(&sample);
        lv_tick_inc(step);
        lv_timer_handler();
        usleep(step * 1000);
        const uint32_t now = lv_tick_get();
        if (now - last_tick >= 90) {
            char path[512];
            snprintf(path, sizeof(path), "%s/chase_%02d.raw", out_dir, shot++);
            if (!snapshot_screen(path)) {
                return 2;
            }
            last_tick = now;
            if (shot >= 8) break;
        }
    }
    printf("wrote %d chase frames to %s (90 ms per ring step)\n", shot, out_dir);
    return 0;
}

int main(int argc, char **argv)
{
    bool window = false;
    bool audit = false;
    bool tpms = false;
    bool chase = false;
    int audit_seconds = 20;
    const char *shot_dir = "preview/sim";
    const char *theme_id = NULL;
    const char *tpms_scenario = NULL;
    const char *chase_dir = NULL;

    /* Run the same theme initialisation the firmware does, BEFORE parsing the
     * options that set layout/preset. Without this the sim only ever saw
     * s_defaults[] - apply_neon_preset() is called from here, not from
     * ensure_loaded() - so neon always rendered its compiled-in palette and no
     * preset could be verified against a screenshot. The NVS half of this
     * function is already #ifdef ESP_PLATFORM, so on the host it reduces to
     * loading the defaults and applying the preset. */
    boost_theme_init();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0) {
            window = true;
        } else if (strcmp(argv[i], "--audit") == 0) {
            audit = true;
        } else if (strcmp(argv[i], "--tpms") == 0) {
            tpms = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') tpms_scenario = argv[++i];
        } else if (strcmp(argv[i], "--seconds") == 0) {
            if (i + 1 < argc) audit_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--theme") == 0) {
            if (i + 1 < argc) theme_id = argv[++i];
        } else if (strcmp(argv[i], "--neon-layout") == 0) {
            /* The neon layout is persisted, so without this the sim always
             * renders whichever layout happens to be stored - which silently
             * made a "tube" audit and a "marquee" audit produce byte-identical
             * numbers because both actually ran the same layout. */
            if (i + 1 < argc) {
                const char *v = argv[++i];
                if (strcmp(v, "tube") == 0) {
                    boost_theme_set_neon_layout(BOOST_NEON_TUBE);
                } else if (strcmp(v, "segments") == 0) {
                    boost_theme_set_neon_layout(BOOST_NEON_SEGMENTS);
                } else if (strcmp(v, "marquee") == 0) {
                    boost_theme_set_neon_layout(BOOST_NEON_MARQUEE);
                } else {
                    fprintf(stderr, "unknown neon layout: %s (tube|segments|marquee)\n", v);
                    return 1;
                }
            }
        } else if (strcmp(argv[i], "--neon-preset") == 0) {
            /* Same class of trap as --neon-layout above, and it bit harder:
             * boost_theme_find() reads s_themes directly and ensure_loaded()
             * only memcpys s_defaults into it. apply_neon_preset() runs from
             * boost_theme_init(), which the sim never called - so every sim
             * render showed the COMPILED-IN palette no matter which preset was
             * selected, and a preset's colours could not be checked here at
             * all. boost_theme_init() is now called below; this selects which
             * palette to render. */
            if (i + 1 < argc) {
                const char *v = argv[++i];
                const int n = atoi(v);
                if (n < 0 || n > 3) {
                    fprintf(stderr, "unknown neon preset: %s (0=violet 1=miami 2=toxic 3=bloodmoon)\n", v);
                    return 1;
                }
                boost_theme_set_neon_preset((boost_neon_preset_t)n);
            }
        } else if (strcmp(argv[i], "--neon-spin") == 0) {
            /* Enable the marquee chase so a screenshot sequence can show the
             * accent bulbs walking around the rings. The chase starts at
             * phase 0 on scene build, so the first frame is the static
             * stagger; later frames differ. */
            boost_theme_set_neon_marquee_spin(true);
    } else if (strcmp(argv[i], "--neon-chase") == 0) {
            /* Fixed-psi chase sequence: hold the OVERBOOST reading (all three
             * rings lit) and snapshot every 90 ms so the accent bulbs walk
             * through all 6 phase states. Requires --neon-layout marquee. */
            chase = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') chase_dir = argv[++i];
        } else if (strcmp(argv[i], "--screenshot") == 0) {
            if (i + 1 < argc) {
                shot_dir = argv[++i];
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown arg: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    lv_init();

    if (window) {
#ifdef SIM_HAVE_SDL
        return run_window();
#else
        fprintf(stderr, "built without SDL2: --window unavailable\n");
        return 1;
#endif
    }

    setup_headless_display();
    if (audit) {
        return run_audit(theme_id, audit_seconds);
    }
    if (tpms) {
        return run_tpms(shot_dir, tpms_scenario);
    }
    if (chase) {
        return run_chase(chase_dir ? chase_dir : "preview/chase");
    }
    return run_screenshots(shot_dir, theme_id);
}
