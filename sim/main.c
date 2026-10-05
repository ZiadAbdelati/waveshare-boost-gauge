/**
 * Desktop LVGL simulator for the boost gauge UI.
 *
 * Modes:
 *   --screenshot DIR   render fixed PSI states to DIR/*.raw + convert helper
 *   --window           open SDL window (needs display / xvfb)
 *   --stream           BGFR frames on stdout + line commands on stdin (panel)
 *   default            headless screenshot into ../preview/sim
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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
    { -12.0f, 0.0f, "vac" },
    { 5.0f, 5.0f, "boost" },
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
 * Compare two .raw frame captures: 1 identical, 0 different, -1 unreadable.
 * An unreadable capture is a harness error, never proof that the frames
 * differ. Used by --qr-test to prove the three overlay pages render distinct
 * frames.
 */
static int raw_files_cmp(const char *pa, const char *pb)
{
    FILE *a = fopen(pa, "rb");
    FILE *b = fopen(pb, "rb");
    if (a == NULL || b == NULL) {
        if (a != NULL) fclose(a);
        if (b != NULL) fclose(b);
        return -1;
    }
    int result = 1;
    for (;;) {
        const int ca = fgetc(a);
        const int cb = fgetc(b);
        if (ca != cb) { result = 0; break; }
        if (ca == EOF) break;
    }
    fclose(a);
    fclose(b);
    return result;
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
        boost_theme_set_vault_needle_red(true);
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
    float s_audit_peak = 0.0f;   /* ratcheted synthetic peak, see the sample loop */
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
            /* Reproduce switching into Night City while already in vacuum, as
             * well as switching into Night City at 0.0 PSI (atmosphere).
             * Pump the rebuild before its first sample, matching the ordering
             * that exposed missing initial invalidations on-device. */
            boost_sample_t zero_sample = { .psi = 0.0f, .demo = true };
            hold_state(&zero_sample, 64);
            boost_gauge_apply_theme(t);
            pump_lvgl(50);
            boost_gauge_update(&zero_sample);
            pump_lvgl(50);

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
    /* Flushed pixels measure the dirty AREA. These measure the work done inside
     * it: callback invocations (one per dirty region) and ring segments
     * submitted (three arc primitives each). */
    extern uint32_t g_neon_cb_calls, g_neon_arcs, g_neon_labels;
    extern uint32_t g_neon_sign_bars, g_neon_sprite_blits;
    extern bool g_neon_flip_pending;
    /* The neon ring band is exempted from the stale check on the frame where a
     * zone flip DEFERRED the full-run recolor (word-first, arc-next-frame):
     * that frame intentionally renders the old-colour ring for exactly one
     * sample. Everything outside the band, and every other frame, must still
     * be byte-clean. Band radii follow boost_gauge.c: inner edge of the halo
     * to the cap outer edge (tube halo inner 174..NEON_R 228, segments
     * NEON_R - NEON_SEG_BAND_DEPTH 179..228), padded 2 px for AA fringes. */
    const boost_neon_layout_t n_layout = boost_theme_neon_layout();
    const double n_lo = (n_layout == BOOST_NEON_SEGMENTS) ? 177.0 : 172.0;
    const double n_hi = 231.0;
    uint64_t cb_total = 0, arc_total = 0, label_total = 0;
    uint64_t sign_total = 0, sprite_total = 0;
    uint32_t cb_max = 0, arc_max = 0, label_max = 0;
    uint32_t sign_max = 0, sprite_max = 0;

    for (int i = 0; i < frames; ++i) {
        const float t = (float)i / 62.0f;
        /* Full-range sweep in both directions, crossing zero and the overboost
         * threshold repeatedly, plus a faster ripple so digits change at
         * different rates - which is exactly what a per-slot invalidation gets
         * wrong when it is wrong. The neon theme raises the base peak so psi
         * actually CROSSES the overboost threshold: that zone flip recolours
         * the whole lit run in one frame, and the full-run recolor can only be
         * stale-pixel-validated when the flip happens. */
        const float span = (theme_id != NULL &&
                            (strcmp(theme_id, "neon") == 0 ||
                             strcmp(theme_id, "dyno-cell") == 0))
            ? 22.0f : 19.0f;
        float psi = -14.0f + span * (0.5f + 0.5f * sinf(t * 1.35f));
        psi += 0.9f * sinf(t * 11.0f) + 0.35f * sinf(t * 23.0f);

        boost_sample_t sample = { 0 };
        sample.psi = psi;
        /* Ratchet the synthetic peak like the firmware's running max (a tap
         * reset is gesture-only), so the tube peak tell-tale HOLDS past the
         * descending run tip and the audit exercises the marker's VISIBLE
         * path, not just the ascent where peak == psi and the marker is
         * clamped under the run. */
        if (psi > s_audit_peak) s_audit_peak = psi;
        sample.peak_psi = s_audit_peak > 0.0f ? s_audit_peak : 0.0f;
        sample.demo = true;
        boost_gauge_update(&sample);
        /* Was this sample a deferred zone flip? The update sets the flag when
         * it defers the run recolor; the render then shows the old-colour ring
         * for this frame. Snapshot after render with the flag still set means
         * the ring-band mismatch is the DESIGNED one-frame lag, not a trail. */
        const bool n_deferred = g_neon_flip_pending;

        s_flush_px = 0;
        g_neon_cb_calls = 0; g_neon_arcs = 0; g_neon_labels = 0;
        g_neon_sign_bars = 0; g_neon_sprite_blits = 0;
        lv_tick_inc(16);
        const double t0 = sim_now_ms();
        lv_timer_handler();
        const double dt = sim_now_ms() - t0;
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
        if (s_flush_px > 0) {
            px_total += s_flush_px;
            if (s_flush_px > px_max) px_max = s_flush_px;
            rendered++;
            /* Record this cycle's (ms, px) so the cost report below has real
             * data (this array was declared but never populated). */
            if (cycles_n == cycles_cap) {
                size_t nc = cycles_cap ? cycles_cap * 2 : 4096;
                cycle_t *n = (cycle_t *)realloc(cycles, nc * sizeof(cycle_t));
                if (n == NULL) { fprintf(stderr, "cycle buf alloc failed\n"); exit(1); }
                cycles = n;
                cycles_cap = nc;
            }
            cycles[cycles_n].ms = dt;
            cycles[cycles_n].px = s_flush_px;
            cycles_n++;
            if (dt > ms_max) ms_max = dt;
            if (dt > 16.0) over16++;
            ms_total += dt;
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
                        /* Deferred-flip frame: the ring shows the old zone
                         * colour for this one frame by design (see above). Skip
                         * mismatches inside the ring band - but nothing else,
                         * and on any other frame skip nothing at all. */
                        if (n_deferred) {
                            const int32_t x = p % DISP_W, y = p / DISP_W;
                            const double dx = x - (DISP_W / 2.0);
                            const double dy = y - (DISP_H / 2.0);
                            const double r = sqrt(dx * dx + dy * dy);
                            if (r >= n_lo && r <= n_hi) continue;
                        }
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
                                   "d565=%d/%d/%d %s partial=#%02X%02X%02X truth=#%02X%02X%02X\n",
                                   i, x, y, sqrt(dx * dx + dy * dy),
                                   atan2(dy, dx) * 180.0 / M_PI, dr, dg, db,
                                   worst_ch > 1 ? "SEVERE" : "(1-step AA seam)",
                                   a[2], a[1], a[0], b[2], b[1], b[0]);
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
        /* Flip cycles (a zone crossing recolors the whole lit run, so flushed
         * px jumps to a large multiple of a normal tick) are the cost that
         * matters on the tube face. Report them separately so a draw-path
         * change can be judged on the worst cycle rather than the mean. */
        const uint64_t f_thresh = px_max * 3 / 4;
        size_t fn = 0; double f_ms = 0.0, f_ms_max = 0.0; uint64_t f_px = 0;
        for (size_t i = 0; i < cycles_n; ++i) {
            if (cycles[i].px < f_thresh) continue;
            fn++; f_ms += cycles[i].ms; f_px += cycles[i].px;
            if (cycles[i].ms > f_ms_max) f_ms_max = cycles[i].ms;
        }
        printf("  flip cycles        : %zu of %zu (px>=%llu) mean %.2f ms  max %.2f ms  mean px %.0f\n",
               fn, cycles_n, (unsigned long long)f_thresh,
               fn ? f_ms / (double)fn : 0.0, f_ms_max,
               fn ? (double)f_px / (double)fn : 0.0);
    }
    free(cycles);
    printf("  flushed px/cycle   : mean %.0f  max %llu\n",
           rendered ? (double)px_total / (double)rendered : 0.0,
           (unsigned long long)px_max);
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
            "  %s --stream             BGFR frames on stdout, commands on stdin\n"
            "                          (host panel: python3 tools/sim_panel.py)\n"
            "  (all modes accept --theme ID, --neon-layout tube|segments|marquee,\n"
            "   --neon-font 0|1 (0=SF Alien 1=Doto), and --neon-spin to enable\n"
            "   the marquee chase for screenshots)\n",
            argv0, argv0, argv0, argv0, argv0);
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

/* Report the QR overlay's widget geometry and return true if any label box
 * overlaps the qrcode widget's bounding box. The qrcode identifies the
 * overlay (it only exists on the QR page, never the toggles page); labels
 * under it are the AP-name/IP text and the swipe hint. This catches the
 * "IP line pushes the AP-name row up under the QR" regression that a pixel
 * scan cannot (the glyphs sit on the QR's white quiet zone). */
static bool qr_layout_overlap(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *overlay = NULL;
    lv_area_t qr = { 0, 0, -1, -1 };
    const uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n && overlay == NULL; i++) {
        lv_obj_t *c = lv_obj_get_child(scr, i);
        const uint32_t m = lv_obj_get_child_count(c);
        for (uint32_t j = 0; j < m; j++) {
            lv_obj_t *g = lv_obj_get_child(c, j);
            if (lv_obj_get_class(g) == &lv_qrcode_class) {
                overlay = c;
                qr.x1 = lv_obj_get_x(g);
                qr.y1 = lv_obj_get_y(g);
                qr.x2 = qr.x1 + lv_obj_get_width(g) - 1;
                qr.y2 = qr.y1 + lv_obj_get_height(g) - 1;
                break;
            }
        }
    }
    if (overlay == NULL) return false;
    printf("  QR overlay geometry:\n");
    printf("    qrcode box: x %d..%d y %d..%d\n", qr.x1, qr.x2, qr.y1, qr.y2);
    bool overlap = false;
    const uint32_t m = lv_obj_get_child_count(overlay);
    for (uint32_t i = 0; i < m; i++) {
        lv_obj_t *lbl = lv_obj_get_child(overlay, i);
        if (lv_obj_get_class(lbl) != &lv_label_class) continue;
        const int32_t lx1 = lv_obj_get_x(lbl);
        const int32_t ly1 = lv_obj_get_y(lbl);
        const int32_t lx2 = lx1 + lv_obj_get_width(lbl) - 1;
        const int32_t ly2 = ly1 + lv_obj_get_height(lbl) - 1;
        printf("    label \"%s\": x %d..%d y %d..%d\n",
               lv_label_get_text(lbl) ? lv_label_get_text(lbl) : "", lx1, lx2, ly1, ly2);
        if (ly1 <= qr.y2 && ly2 >= qr.y1 && lx1 <= qr.x2 && lx2 >= qr.x1) {
            overlap = true;
        }
    }
    return overlap;
}

/* --- Synthetic pointer indev: REAL taps through the LVGL dispatch ------------
 * The hooks above call the overlay's own callbacks directly, which SKIPS the
 * layer where the wiring lives: hit-testing, object ownership (square vs
 * overlay), PRESSED/PRESSING/RELEASED/CLICKED delivery and event routing. A
 * mis-targeted lv_obj_add_event_cb, a bubbling CLICKED, or a square that never
 * receives PRESSING would all still leave a hook-driven harness green. This
 * indEV feeds real press/move/release samples so everything downstream of
 * `lv_indev_read_timer_cb` runs the production path. */
typedef struct {
    lv_point_t p;
    lv_indev_state_t state;
} sim_pt_t;

#define SIM_PT_MAX 32
static sim_pt_t s_pt[SIM_PT_MAX];
static int s_pt_head, s_pt_tail;
static lv_point_t s_pt_last = { 0, 0 };
static lv_indev_state_t s_pt_last_state = LV_INDEV_STATE_RELEASED;

static void sim_pt_push(int32_t x, int32_t y, lv_indev_state_t state)
{
    const int next = (s_pt_tail + 1) % SIM_PT_MAX;
    if (next == s_pt_head) return;   /* full: drop (the harness never overruns) */
    s_pt[s_pt_tail].p.x = x;
    s_pt[s_pt_tail].p.y = y;
    s_pt[s_pt_tail].state = state;
    s_pt_tail = next;
}

static void sim_indev_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    if (s_pt_head != s_pt_tail) {
        s_pt_last = s_pt[s_pt_head].p;
        s_pt_last_state = s_pt[s_pt_head].state;
        s_pt_head = (s_pt_head + 1) % SIM_PT_MAX;
        data->point = s_pt_last;
        data->state = s_pt_last_state;
        data->continue_reading = (s_pt_head != s_pt_tail);
        return;
    }
    /* Idle: hold the last sample so LVGL sees a stable state between touches. */
    data->point = s_pt_last;
    data->state = s_pt_last_state;
    data->continue_reading = false;
}

static void setup_tap_indev(void)
{
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, sim_indev_read_cb);
    lv_indev_set_display(indev, lv_display_get_default());
}

/* One finger-down and up at a point, delivered as two separate read cycles (the
 * 33 ms indev timer, so a press and the release that produces CLICKED do not
 * share a cycle - the same shape a real touch takes). */
static void real_tap(int32_t x, int32_t y)
{
    sim_pt_push(x, y, LV_INDEV_STATE_PRESSED);
    pump_lvgl(45);
    sim_pt_push(x, y, LV_INDEV_STATE_RELEASED);
    pump_lvgl(45);
}

/* A real drag: down, one or more PRESSING samples, then up. */
static void real_drag(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    sim_pt_push(x0, y0, LV_INDEV_STATE_PRESSED);
    pump_lvgl(45);
    sim_pt_push((x0 + x1) / 2, (y0 + y1) / 2, LV_INDEV_STATE_PRESSED);
    pump_lvgl(45);
    sim_pt_push(x1, y1, LV_INDEV_STATE_PRESSED);
    pump_lvgl(45);
    sim_pt_push(x1, y1, LV_INDEV_STATE_RELEASED);
    pump_lvgl(45);
}

/* --qr-test: verify the two-finger QR overlay + the 3-page swipe cycle.
 *  1. show the overlay (QR page) -> snapshot qr_page0
 *  2. swipe left -> Connections page -> snapshot qr_page1
 *  3. swipe right wraps (0 -> 2) and two more rights return to page 0
 *  4. tap dismiss -> overlay gone, gauge still live (updates again)
 * Returns 0 only if every step observed. */
static int run_qr_test(const char *out_dir)
{
    sim_mkdir(out_dir);
    boost_sim_init();
    boost_tpms_init();
    boost_page_create();
    pump_lvgl(50);

    boost_sample_t sample = { .psi = 5.0f, .peak_psi = 5.0f, .demo = true };
    boost_page_update(&sample);
    pump_lvgl(50);

    int failures = 0;
    /* 1. QR page shows */
    boost_page_qr_show();
    pump_lvgl(100);
    if (!boost_page_qr_active()) { fprintf(stderr, "FAIL overlay did not show\n"); failures++; }
    char path[512];
    snprintf(path, sizeof(path), "%s/qr_page0.raw", out_dir);
    if (!snapshot_screen(path)) return 2;
    printf("wrote %s (QR page)\n", path);
    if (qr_layout_overlap()) {
        fprintf(stderr, "FAIL a label overlaps the QR code bounding box\n");
        failures++;
    }

    /* 2. Swipe left -> toggles page */
    boost_page_qr_swipe_left();
    pump_lvgl(100);
    if (!boost_page_qr_active()) { fprintf(stderr, "FAIL overlay lost after swipe\n"); failures++; }
    snprintf(path, sizeof(path), "%s/qr_page1.raw", out_dir);
    if (!snapshot_screen(path)) return 2;
    printf("wrote %s (toggles page)\n", path);

    /* 2a. Units page (holds the reference button). */
    boost_page_qr_show_page(2);
    pump_lvgl(100);
    if (boost_page_qr_page() != 2) {
        fprintf(stderr, "FAIL show_page(2) did not open the Units page (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    snprintf(path, sizeof(path), "%s/qr_page2.raw", out_dir);
    if (!snapshot_screen(path)) return 2;
    printf("wrote %s (Units page)\n", path);

    /* 2a-ii. The three pages must be MUTUALLY DISTINCT frames. Without this a
     * page-2 build that reused page 1's widgets would still pass: the capture
     * checks above only prove a frame was written, never that it differs. */
    {
        char p0[512], p1[512], p2[512];
        snprintf(p0, sizeof(p0), "%s/qr_page0.raw", out_dir);
        snprintf(p1, sizeof(p1), "%s/qr_page1.raw", out_dir);
        snprintf(p2, sizeof(p2), "%s/qr_page2.raw", out_dir);
        const int c01 = raw_files_cmp(p0, p1);
        const int c02 = raw_files_cmp(p0, p2);
        const int c12 = raw_files_cmp(p1, p2);
        if (c01 < 0 || c02 < 0 || c12 < 0) {
            fprintf(stderr, "FAIL could not compare the three page frames\n");
            failures++;
        } else if (c01 == 1 || c02 == 1 || c12 == 1) {
            fprintf(stderr, "FAIL overlay pages 0/1/2 do not render distinct frames "
                            "(page0==page1: %d, page0==page2: %d, page1==page2: %d)\n",
                    c01 == 1, c02 == 1, c12 == 1);
            failures++;
        } else {
            printf("page frames distinct: OK (pages 0/1/2 render differently)\n");
        }
    }

    /* 2b. Wraparound: from page 0 a RIGHT swipe is 'prev' and wraps to page 2. */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_swipe_right();
    pump_lvgl(80);
    if (!boost_page_qr_active()) { fprintf(stderr, "FAIL overlay lost on wraparound right\n"); failures++; }
    if (boost_page_qr_page() != 2) {
        fprintf(stderr, "FAIL swipe right on page0 did not wrap to page 2 (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    printf("wraparound right: OK\n");

    /* 3. Two more right swipes (2 -> 1 -> 0) return to the QR page. */
    boost_page_qr_swipe_right();
    pump_lvgl(80);
    if (boost_page_qr_page() != 1) {
        fprintf(stderr, "FAIL swipe right did not land on page 1 (got %d)\n", boost_page_qr_page());
        failures++;
    }
    boost_page_qr_swipe_right();
    pump_lvgl(100);
    if (!boost_page_qr_active()) { fprintf(stderr, "FAIL overlay lost after swipe right\n"); failures++; }
    if (boost_page_qr_page() != 0) {
        fprintf(stderr, "FAIL swipe right did not restore the QR page (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    /* The QR page is the only one with a qrcode widget; distinguish by
     * re-snapshotting and checking pixel content differs from page1. */
    snprintf(path, sizeof(path), "%s/qr_page0b.raw", out_dir);
    if (!snapshot_screen(path)) return 2;
    printf("wrote %s (back to QR page)\n", path);
    /* byte-compare page0b against page0: same page, same content */
    {
        char ref[512];
        snprintf(ref, sizeof(ref), "%s/qr_page0.raw", out_dir);
        FILE *a = fopen(ref, "rb");
        FILE *b = fopen(path, "rb");
        if (!a || !b) { fprintf(stderr, "FAIL cannot open page files\n"); failures++; }
        else {
            int ca, cb, same = 1;
            while ((ca = fgetc(a)) != EOF && (cb = fgetc(b)) != EOF) {
                if (ca != cb) { same = 0; break; }
            }
            if (!same) { fprintf(stderr, "FAIL swipe right did not restore the QR page\n"); failures++; }
            fclose(a); fclose(b);
        }
    }

    /* 2c. Square-button interaction: tapping the OBD square toggles the link.
     * The panel toggle must persist through the theme store, not just flip
     * RAM (reboot-lost regression, 2026-08-28): assert ON reaches BOTH the
     * store and the link, then tap again for OFF. */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_swipe_left();
    pump_lvgl(50);
    if (boost_page_qr_page() != 1) { fprintf(stderr, "FAIL toggles page for toggle test\n"); failures++; }
    {
        extern int g_sim_obd_set_calls;
        extern bool g_sim_obd_state;
        g_sim_obd_state = false;
        const int calls_before = g_sim_obd_set_calls;
        boost_page_qr_tap_switch(0);   /* OBD BLE square */
        for (int i = 0; i < 10; ++i) { lv_tick_inc(16); lv_timer_handler(); usleep(16000); }
        const int calls_after = g_sim_obd_set_calls;
        if (calls_after != calls_before + 1) {
            fprintf(stderr, "FAIL OBD square tap did not apply the toggle (%d -> %d)\n",
                    calls_before, calls_after);
            failures++;
        } else {
            printf("OBD square tap applied toggle: OK\n");
        }
        if (boost_page_qr_pending_toggle() != -1) {
            fprintf(stderr, "FAIL toggle request left pending\n");
            failures++;
        }
        if (!boost_theme_tpms_ble()) {
            fprintf(stderr, "FAIL OBD toggle did not persist via theme store\n");
            failures++;
        }
        if (!g_sim_obd_state) {
            fprintf(stderr, "FAIL persisted ON did not reach the live link\n");
            failures++;
        }
        /* Rebuild with the square now showing ON; a second tap turns it OFF. */
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show();
        boost_page_qr_swipe_left();
        pump_lvgl(50);
        boost_page_qr_tap_switch(0);
        for (int i = 0; i < 10; ++i) { lv_tick_inc(16); lv_timer_handler(); usleep(16000); }
        if (boost_theme_tpms_ble()) {
            fprintf(stderr, "FAIL second OBD tap did not clear tpmsBle\n");
            failures++;
        }
        if (g_sim_obd_state) {
            fprintf(stderr, "FAIL second OBD tap did not clear the live link\n");
            failures++;
        }
    }

    /* 2c-ii. A BLE toggle must REPAINT its own square. show_qr() reads the link
     * state only at build time, and nothing else ever repaints the overlay (the
     * 16 ms gauge path is gated on s_qr_active), so a toggle applied without a
     * rebuild left the button looking dead until the next page step - the
     * "BLE buttons don't respond to taps" report (2026-10-05). No page change
     * here: the square's ON/OFF line, glow and status LED must follow the tap on
     * the spot. The frame diff is the primary witness (the pixels on glass), the
     * text read-back names the mechanism. */
    {
        extern bool g_sim_obd_state;
        g_sim_obd_state = false;
        boost_page_qr_show_page(1);   /* rebuild on Connections, square reads OFF */
        pump_lvgl(50);
        if (boost_page_qr_page() != 1) {
            fprintf(stderr, "FAIL BLE repaint test did not reach the Connections page\n");
            failures++;
        }
        char ble_before[512], ble_after[512];
        snprintf(ble_before, sizeof(ble_before), "%s/qr_ble_before.raw", out_dir);
        if (!snapshot_screen(ble_before)) return 2;
        boost_page_qr_tap_switch(0);   /* OBD BLE square */
        for (int i = 0; i < 10; ++i) { lv_tick_inc(16); lv_timer_handler(); usleep(16000); }
        if (!g_sim_obd_state) {
            fprintf(stderr, "FAIL BLE repaint test: the tap did not enable the link\n");
            failures++;
        }
        const char *shown = boost_page_qr_switch_text(0);
        if (strcmp(shown, "ON") != 0) {
            fprintf(stderr, "FAIL OBD square still reads \"%s\" after its own tap "
                            "(the overlay did not repaint)\n", shown);
            failures++;
        } else {
            printf("OBD square repaints on tap: OK\n");
        }
        if (boost_page_qr_page() != 1) {
            fprintf(stderr, "FAIL a BLE toggle changed the overlay page (%d)\n",
                    boost_page_qr_page());
            failures++;
        }
        snprintf(ble_after, sizeof(ble_after), "%s/qr_ble_after.raw", out_dir);
        if (!snapshot_screen(ble_after)) return 2;
        const int ble_cmp = raw_files_cmp(ble_before, ble_after);
        if (ble_cmp < 0) {
            fprintf(stderr, "FAIL BLE repaint test: cannot compare the frames\n");
            failures++;
        } else if (ble_cmp == 1) {
            fprintf(stderr, "FAIL the OBD tap left the overlay pixels unchanged "
                            "(the square never repainted)\n");
            failures++;
        } else {
            printf("OBD tap repaints its square: OK (frames differ)\n");
        }
        boost_theme_set_tpms_ble(false);   /* leave the world as the 2c block did */

        /* The APP BLE square is a separate request code path
         * (boost_app_ble_set_enabled), so it needs its own witness. */
        extern bool g_sim_app_ble_state;
        g_sim_app_ble_state = false;
        boost_page_qr_show_page(1);   /* rebuild so the square reads OFF */
        pump_lvgl(50);
        boost_page_qr_tap_switch(1);   /* APP BLE square */
        for (int i = 0; i < 10; ++i) { lv_tick_inc(16); lv_timer_handler(); usleep(16000); }
        const char *app_shown = boost_page_qr_switch_text(1);
        if (!g_sim_app_ble_state || strcmp(app_shown, "ON") != 0) {
            fprintf(stderr, "FAIL APP BLE square reads \"%s\" (link %d) after its own tap\n",
                    app_shown, g_sim_app_ble_state ? 1 : 0);
            failures++;
        } else {
            printf("APP BLE square repaints on tap: OK\n");
        }
        g_sim_app_ble_state = false;
    }

    /* Reset stub link states so the round-trip determinism check holds. */
    {
        extern bool g_sim_obd_state, g_sim_app_ble_state;
        g_sim_obd_state = false;
        g_sim_app_ble_state = false;
        /* Re-show: show_qr() always opens on the QR page unchecked. */
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show();
        pump_lvgl(30);
        if (boost_page_qr_page() != 0) { fprintf(stderr, "FAIL re-show not on QR page\n"); failures++; }
    }

    /* 3b. Swipe left again -> toggles (back-and-forth round trip) */
    boost_page_qr_swipe_left();
    pump_lvgl(50);
    if (!boost_page_qr_active()) { fprintf(stderr, "FAIL overlay lost on second swipe left\n"); failures++; }
    snprintf(path, sizeof(path), "%s/qr_page1b.raw", out_dir);
    if (!snapshot_screen(path)) return 2;
    printf("wrote %s (toggles again)\n", path);
    {
        char ref[512];
        snprintf(ref, sizeof(ref), "%s/qr_page1.raw", out_dir);
        FILE *a = fopen(ref, "rb");
        FILE *b = fopen(path, "rb");
        if (a && b) {
            int ca, cb, same = 1;
            while ((ca = fgetc(a)) != EOF && (cb = fgetc(b)) != EOF) {
                if (ca != cb) { same = 0; break; }
            }
            if (!same) { fprintf(stderr, "FAIL second toggles render differs\n"); failures++; }
            fclose(a); fclose(b);
        }
    }

    /* 3c. Gesture classification on the overlay (board report 2026-10-05: "I
     * can't swipe on the settings page - it takes me out of settings and
     * changes themes"). Three properties, all driven through the PRODUCTION
     * classifier and state machine via the gesture-injection hooks:
     *   (a) a short background flick (past the 12 px tap slop, under the 48 px
     *       swipe threshold) is a DRAG, not a tap: it must not dismiss;
     *   (b) a vertical flick changes the theme, the overlay survives, and the
     *       very next swipe still steps a page (the latch that used to wedge
     *       the classifier for every later gesture);
     *   (c) a flick whose PRESSED was never delivered must not be measured
     *       from the previous gesture's touch-down point. */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    if (boost_page_qr_page() != 0) {
        fprintf(stderr, "FAIL gesture section did not open on the QR page\n");
        failures++;
    }

    /* (a) 40 px background flick: a drag, so no dismiss. */
    boost_page_qr_drag(150, 233, 190, 260);
    pump_lvgl(60);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL a 40 px drag dismissed the overlay (a drag is not a tap)\n");
        failures++;
    } else if (boost_page_qr_page() != 0) {
        fprintf(stderr, "FAIL a 40 px drag stepped the page\n");
        failures++;
    } else {
        printf("short drag does not dismiss: OK\n");
    }

    /* (b) A vertical flick while the overlay is open must change NOTHING: the
     * theme behind an opaque settings cover has no affordance, and the user
     * asked for the overlay to stop switching themes (it used to, documented,
     * and it made an accidental diagonal swipe look like the overlay "changing
     * themes"). The overlay must survive, stay on its page, and keep working.
     * Snapshot the theme-0 face with the overlay DOWN first, to prove it is
     * unchanged at the end. */
    boost_page_qr_dismiss();
    pump_lvgl(60);
    {
        char face_pre[512];
        snprintf(face_pre, sizeof(face_pre), "%s/qr_face_pre.raw", out_dir);
        if (!snapshot_screen(face_pre)) return 2;
    }
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_drag(233, 400, 233, 100);   /* flick up: must be ignored */
    pump_lvgl(120);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL a vertical flick on the overlay dismissed it\n");
        failures++;
    }
    if (boost_page_qr_page() != 0) {
        fprintf(stderr, "FAIL vertical flick stepped the page (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    boost_page_qr_swipe_left();
    pump_lvgl(80);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL overlay lost after the post-flick swipe\n");
        failures++;
    } else if (boost_page_qr_page() != 1) {
        fprintf(stderr, "FAIL swipe after a vertical flick did not step one page (got %d)\n",
                boost_page_qr_page());
        failures++;
    } else {
        printf("swipe after a vertical flick: OK\n");
    }

    /* (c) No touch-down event: the first sample must establish its own origin.
     * From page 1 a leftward flick steps forward to page 2. */
    boost_page_qr_drag_unseeded(406, 233, 60, 233);
    pump_lvgl(80);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL overlay lost on the unseeded flick\n");
        failures++;
    } else if (boost_page_qr_page() != 2) {
        fprintf(stderr, "FAIL unseeded flick did not step 1 -> 2 (got %d): stale origin\n",
                boost_page_qr_page());
        failures++;
    } else {
        printf("unseeded flick steps one page: OK\n");
    }

    /* (b, conclusion) the vertical flick changed nothing: the face must be
     * byte-identical to the pre-flick capture. */
    boost_page_qr_dismiss();
    pump_lvgl(60);
    {
        char face_post[512], face_pre[512];
        snprintf(face_post, sizeof(face_post), "%s/qr_face_post.raw", out_dir);
        snprintf(face_pre, sizeof(face_pre), "%s/qr_face_pre.raw", out_dir);
        if (!snapshot_screen(face_post)) return 2;
        /* raw_files_cmp: 0 = the files differ, 1 = identical, -1 = open error. */
        if (raw_files_cmp(face_pre, face_post) != 1) {
            fprintf(stderr, "FAIL a vertical flick on the overlay changed the theme face\n");
            failures++;
        } else {
            printf("vertical flick leaves the theme alone: OK\n");
        }
    }

    /* (d) A drag that starts ON a switch is a drag too - the square owns the
     * whole press stream on glass, and its tap callback must apply the same
     * "a drag is not a tap" rule as the overlay background: a 30 px flick must
     * not change the setting, while a 300 px horizontal flick steps the page.
     * The SETTING is the durable witness: the deferred request is consumed by
     * the next LVGL cycle, so a pending-request check would pass either way. */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_show_page(2);   /* Units: UNITS + REL/ABS */
    pump_lvgl(60);
    if (boost_page_qr_page() != 2) {
        fprintf(stderr, "FAIL switch test did not open the Units page (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    const boost_unit_t unit_before = boost_theme_pressure_unit();
    boost_page_qr_switch_gesture(0, 30, 12);   /* short flick on UNITS */
    pump_lvgl(60);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL a 30 px switch drag dismissed the overlay\n");
        failures++;
    } else if (boost_theme_pressure_unit() != unit_before) {
        fprintf(stderr, "FAIL a 30 px drag starting on a switch changed the unit\n");
        failures++;
    } else {
        printf("short switch drag does not toggle: OK\n");
    }
    boost_page_qr_tap_switch(0);   /* a real tap must still cycle the unit */
    for (int i = 0; i < 10; ++i) { lv_tick_inc(16); lv_timer_handler(); usleep(16000); }
    if (boost_theme_pressure_unit() == unit_before) {
        fprintf(stderr, "FAIL a tap on the switch no longer cycles the unit\n");
        failures++;
    } else {
        printf("switch tap still toggles: OK\n");
    }
    const boost_unit_t unit_after_tap = boost_theme_pressure_unit();
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_show_page(2);
    pump_lvgl(60);
    boost_page_qr_switch_gesture(0, -300, 0);   /* flick left from UNITS */
    pump_lvgl(60);
    if (!boost_page_qr_active()) {
        fprintf(stderr, "FAIL a switch flick dismissed the overlay\n");
        failures++;
    } else if (boost_page_qr_page() != 0) {
        fprintf(stderr, "FAIL switch flick did not step 2 -> 0 (got %d)\n",
                boost_page_qr_page());
        failures++;
    } else if (boost_theme_pressure_unit() != unit_after_tap) {
        fprintf(stderr, "FAIL a switch flick also changed the unit\n");
        failures++;
    } else {
        printf("switch flick steps the page without toggling: OK\n");
    }

    /* (d2) The same rule across a TOGGLE->TOGGLE step (1 -> 2), which is the case
     * where the overlay and the square are both re-created: the hook must not
     * raise the square's CLICKED on the rebuilt page, so the OBD link must stay
     * put. Durable witness: the persisted tpmsBle flag. */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_show_page(1);   /* Connections: OBD BLE + APP BLE */
    pump_lvgl(60);
    {
        const bool tpms_before = boost_theme_tpms_ble();
        const boost_unit_t unit_pre = boost_theme_pressure_unit();
        boost_page_qr_switch_gesture(0, -300, 0);   /* flick left from OBD BLE */
        pump_lvgl(60);
        if (!boost_page_qr_active()) {
            fprintf(stderr, "FAIL a toggle->toggle switch flick dismissed the overlay\n");
            failures++;
        } else if (boost_page_qr_page() != 2) {
            fprintf(stderr, "FAIL toggle->toggle switch flick did not step 1 -> 2 (got %d)\n",
                    boost_page_qr_page());
            failures++;
        } else if (boost_theme_tpms_ble() != tpms_before) {
            fprintf(stderr, "FAIL a switch flick toggled the OBD link of the page it left\n");
            failures++;
        } else if (boost_theme_pressure_unit() != unit_pre) {
            /* The rebuilt page's row-0 square is UNITS: a misplaced CLICKED would
             * cycle the unit here, which is the hazard this case exists to catch. */
            fprintf(stderr, "FAIL a switch flick tapped the rebuilt page's square\n");
            failures++;
        } else {
            printf("toggle->toggle switch flick steps without toggling: OK\n");
        }
    }

    /* (e) One action per gesture: further PRESSING samples after a
     * page-stepping flick must not step a second page (the mid-drag rebuild
     * drops the origin; the one-shot latch is what keeps it to one action). */
    boost_page_qr_dismiss();
    pump_lvgl(30);
    boost_page_qr_show();
    pump_lvgl(30);
    boost_page_qr_press(406, 233);
    boost_page_qr_move(60, 233);
    pump_lvgl(60);
    if (boost_page_qr_page() != 1) {
        fprintf(stderr, "FAIL the step flick did not reach page 1 (got %d)\n",
                boost_page_qr_page());
        failures++;
    }
    boost_page_qr_move(60, 233);
    boost_page_qr_move(406, 233);
    pump_lvgl(60);
    if (boost_page_qr_page() != 1) {
        fprintf(stderr, "FAIL two steps' worth of movement in one gesture re-stepped (got %d)\n",
                boost_page_qr_page());
        failures++;
    } else {
        printf("one page per gesture: OK\n");
    }
    boost_page_qr_release();
    pump_lvgl(30);

    /* 5. REAL taps through the LVGL dispatch. A synthetic pointer indev feeds
     * press/move/release samples, so hit-testing, object ownership (square vs
     * overlay), PRESSED/PRESSING/RELEASED/CLICKED delivery and event routing are
     * all the production paths - precisely the layer the injected hooks above
     * SKIP. This is the closest thing to a finger that exists on the host, and
     * it is the only place that can catch a mis-targeted registration, a
     * CLICKED that bubbles to the overlay, or a square that never sees PRESSING.
     * (The gesture machine itself still needs the hooks, because a long PRESSED
     * stream cannot be synthesised from a 33 ms read timer.) */
    setup_tap_indev();
#define RT(cond, label) do { \
        if (cond) { printf("real-tap %s: OK\n", label); } \
        else { fprintf(stderr, "FAIL real-tap %s\n", label); failures++; } \
    } while (0)
    {
        extern bool g_sim_obd_state, g_sim_app_ble_state;
        int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;

        /* 5a. Connections page: a real tap on each square toggles it AND repaints
         * it, without navigating - the board report, re-verified end-to-end. */
        g_sim_obd_state = false;
        g_sim_app_ble_state = false;
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        if (!boost_page_qr_switch_center(0, &cx0, &cy0) ||
            !boost_page_qr_switch_center(1, &cx1, &cy1)) {
            fprintf(stderr, "FAIL real-tap test could not locate the squares\n");
            failures++;
        } else {
            real_tap(cx0, cy0);
            RT(g_sim_obd_state && strcmp(boost_page_qr_switch_text(0), "ON") == 0 &&
               boost_page_qr_page() == 1,
               "OBD square: a real tap toggles, repaints and stays on the page");

            real_tap(cx1, cy1);
            RT(g_sim_app_ble_state && strcmp(boost_page_qr_switch_text(1), "ON") == 0 &&
               boost_page_qr_page() == 1,
               "APP BLE square: a real tap toggles and repaints");

            real_tap(cx0, cy0);
            RT(!g_sim_obd_state && strcmp(boost_page_qr_switch_text(0), "OFF") == 0,
               "OBD square: a second real tap turns it back off");

            boost_theme_set_tpms_ble(false);
            g_sim_obd_state = false;
            g_sim_app_ble_state = false;

            /* 5b. A real tap on a square must not ALSO reach the overlay as a
             * fresh tap (a bubbling CLICKED would dismiss the whole page). */
            boost_page_qr_show_page(1);
            pump_lvgl(60);
            real_tap(cx1, cy1);
            RT(boost_page_qr_active() && boost_page_qr_page() == 1,
               "a square tap does not bubble to the overlay and dismiss it");
            g_sim_app_ble_state = false;
        }

        /* 5c. A real tap on the background (away from the buttons) dismisses -
         * the documented fresh-tap rule, through real routing. */
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        real_tap(30, 430);
        RT(!boost_page_qr_active(), "a real tap on the background dismisses");

        /* 5d. A real SHORT drag is a drag, not a tap: no dismiss, no step. */
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        real_drag(60, 430, 90, 430);
        RT(boost_page_qr_active() && boost_page_qr_page() == 1,
           "a real 30 px drag neither dismisses nor steps");

        /* 5e. ...and a real long horizontal flick steps exactly one page. */
        real_drag(406, 430, 60, 430);
        RT(boost_page_qr_active() && boost_page_qr_page() == 2,
           "a real leftward flick steps exactly one page");

        /* 5f. A real vertical flick on the overlay does nothing at all. */
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        real_drag(233, 430, 233, 200);
        RT(boost_page_qr_active() && boost_page_qr_page() == 1,
           "a real vertical flick does nothing");

        /* 5g. A real drag that STARTS on a square must not toggle it. */
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        if (boost_page_qr_switch_center(0, &cx0, &cy0)) {
            real_drag(cx0 - 15, cy0, cx0 + 15, cy0);
            RT(!g_sim_obd_state && boost_page_qr_active(),
               "a real 30 px drag starting on a square does not toggle or dismiss");
        }

        /* 5h. A real tap carrying finger JITTER under the 12 px tap slop still
         * counts as a tap - the CST9217 drift risk the release notes name. */
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        if (boost_page_qr_switch_center(0, &cx0, &cy0)) {
            sim_pt_push(cx0, cy0, LV_INDEV_STATE_PRESSED);
            pump_lvgl(45);
            sim_pt_push(cx0 + 6, cy0 + 4, LV_INDEV_STATE_PRESSED);
            pump_lvgl(45);
            sim_pt_push(cx0 + 6, cy0 + 4, LV_INDEV_STATE_RELEASED);
            pump_lvgl(45);
            RT(g_sim_obd_state, "a real tap with 7 px of jitter still toggles");
            boost_theme_set_tpms_ble(false);
            g_sim_obd_state = false;
        }

        /* 5i. Units page: real taps cycle the unit and flip the reference. */
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show_page(2);
        pump_lvgl(60);
        {
            const boost_unit_t u0 = boost_theme_pressure_unit();
            const bool abs0 = boost_theme_pressure_absolute();
            if (boost_page_qr_switch_center(0, &cx0, &cy0) &&
                boost_page_qr_switch_center(1, &cx1, &cy1)) {
                real_tap(cx0, cy0);
                RT(boost_theme_pressure_unit() != u0 && boost_page_qr_page() == 2,
                   "UNITS square: a real tap cycles the unit");

                real_tap(cx1, cy1);
                RT(boost_theme_pressure_absolute() != abs0 &&
                   strcmp(boost_page_qr_switch_text(1),
                          boost_theme_pressure_absolute() ? "ABSOLUTE" : "RELATIVE") == 0,
                   "REL/ABS square: a real tap flips the reference and repaints");
            }
        }

        /* 5j. QR page: a real tap ON the QR code must fall through to the overlay
         * (a swallow here would make the overlay feel stuck), and so must one on
         * the bare background. */
        boost_page_qr_dismiss();
        pump_lvgl(30);
        boost_page_qr_show();
        pump_lvgl(60);
        real_tap(233, 233);   /* dead centre of the 320 px QR code */
        RT(!boost_page_qr_active(), "a real tap on the QR code falls through and dismisses");
        boost_page_qr_show();
        pump_lvgl(60);
        real_tap(30, 430);
        RT(!boost_page_qr_active(), "a real tap on the QR page background dismisses");

        /* Observation only, deliberately NOT pinned: what a tap in the 40 px gap
         * BETWEEN the two squares does (the fresh-tap rule says dismiss, which is
         * a plausible next board report). */
        boost_page_qr_show_page(1);
        pump_lvgl(60);
        real_tap(233, 215);
        printf("note: a real tap in the gap between the squares %s\n",
               boost_page_qr_active() ? "leaves the overlay open" : "dismisses the overlay");
        boost_page_qr_dismiss();
        pump_lvgl(30);
    }
#undef RT

    /* 4. Tap dismisses and gauge resumes */
    boost_page_qr_dismiss();
    pump_lvgl(50);
    if (boost_page_qr_active()) { fprintf(stderr, "FAIL overlay still active after tap\n"); failures++; }
    boost_page_update(&sample);   /* must run again with no overlay up */
    pump_lvgl(50);
    printf("qr-test: %s (%d failures)\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 5;
}

/*
 * --stream: host-only control-panel backend.
 *
 * Writes one BGFR frame per render cycle to `frame_fd` (the process's real
 * stdout, dup'd before everything else was redirected to stderr) and reads
 * one command per line from stdin without ever blocking the render loop.
 * Every command dispatches to the SAME firmware entry point the settings UI
 * (web/BLE/QR panel) drives, so the panel can only show what the device would.
 *
 * Framing (little-endian):
 *   "BGFR" | uint32 width | uint32 height | uint32 seq | RGBA pixels
 * RGBA is the ARGB8888 byte order measured by raw_to_png.py (B,G,R,A on LE).
 */

#define STREAM_DEFAULT_PSI 5.0f
#define STREAM_FRAME_PERIOD_MS 16

static char s_stream_theme[BOOST_THEME_ID_MAX] = "dyno-cell";
static const char *s_stream_start_theme;
static float s_stream_psi = STREAM_DEFAULT_PSI;
static float s_stream_peak;
static char s_stream_cmd[4096];
static size_t s_stream_cmd_len;
static bool s_stream_stdin_open = true;
static bool s_stream_quit;

static bool stream_write_all(int fd, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    while (len > 0) {
        const ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static bool stream_emit_frame(int fd, uint32_t seq)
{
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_ARGB8888);
    if (buf == NULL) {
        fprintf(stderr, "[stream] snapshot failed\n");
        return false;
    }
    const uint32_t w = (uint32_t)buf->header.w;
    const uint32_t h = (uint32_t)buf->header.h;
    uint8_t hdr[16];
    memcpy(hdr, "BGFR", 4);
    memcpy(hdr + 4, &w, 4);
    memcpy(hdr + 8, &h, 4);
    memcpy(hdr + 12, &seq, 4);
    const bool ok = stream_write_all(fd, hdr, sizeof(hdr)) &&
                    stream_write_all(fd, buf->data, (size_t)w * (size_t)h * 4);
    lv_draw_buf_destroy(buf);
    return ok;
}

/* Rebuild the active face, the way the settings UI does after a unit/layout/
 * font/preset/needle change (unit marks and tick numerals are baked at build). */
static void stream_rebuild(void)
{
    const boost_theme_t *t = boost_theme_find(s_stream_theme);
    if (t != NULL) {
        boost_gauge_apply_theme(t);
    }
}

static void stream_apply_theme(const char *id)
{
    const boost_theme_t *t = boost_theme_find(id);
    if (t == NULL) {
        fprintf(stderr, "[stream] unknown theme: %s\n", id);
        return;
    }
    snprintf(s_stream_theme, sizeof(s_stream_theme), "%s", t->id);
    boost_gauge_apply_theme(t);
}

static boost_sample_t stream_fixed_sample(void)
{
    if (s_stream_psi > s_stream_peak) s_stream_peak = s_stream_psi;
    boost_sample_t s = {
        .psi = s_stream_psi,
        .peak_psi = s_stream_peak,
        .demo = false,
    };
    return s;
}

static void stream_exec(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    char *end = line + strlen(line);
    while (end > line && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) {
        *--end = '\0';
    }
    if (*line == '\0') return;

    char *arg = line;
    while (*arg != '\0' && *arg != ' ' && *arg != '\t') arg++;
    if (*arg != '\0') {
        *arg++ = '\0';
        while (*arg == ' ' || *arg == '\t') arg++;
    }

    if (strcmp(line, "theme") == 0) {
        stream_apply_theme(arg);
    } else if (strcmp(line, "unit") == 0) {
        boost_unit_t u;
        if (boost_units_parse(arg, &u)) {
            boost_theme_set_pressure_unit(u);
            stream_rebuild();
        } else {
            fprintf(stderr, "[stream] unknown unit: %s\n", arg);
        }
    } else if (strcmp(line, "psi") == 0) {
        char *stop = NULL;
        const float v = strtof(arg, &stop);
        if (stop != arg) {
            s_stream_psi = v;
            if (v > s_stream_peak) s_stream_peak = v;
            /* A fixed reading means the sweep is frozen. */
            boost_theme_set_demo_mode(false);
        }
    } else if (strcmp(line, "demo") == 0) {
        if (strcmp(arg, "on") == 0) {
            boost_theme_set_demo_mode(true);
        } else if (strcmp(arg, "off") == 0) {
            boost_theme_set_demo_mode(false);
        }
    } else if (strcmp(line, "sweep") == 0) {
        if (strcmp(arg, "organic") == 0) {
            boost_theme_set_demo_fast_sweep(false);
        } else if (strcmp(arg, "fast") == 0) {
            boost_theme_set_demo_fast_sweep(true);
        }
    } else if (strcmp(line, "layout") == 0) {
        if (strcmp(arg, "tube") == 0) {
            boost_theme_set_neon_layout(BOOST_NEON_TUBE);
        } else if (strcmp(arg, "segments") == 0) {
            boost_theme_set_neon_layout(BOOST_NEON_SEGMENTS);
        } else if (strcmp(arg, "marquee") == 0) {
            boost_theme_set_neon_layout(BOOST_NEON_MARQUEE);
        } else {
            fprintf(stderr, "[stream] unknown layout: %s\n", arg);
            return;
        }
        stream_rebuild();
    } else if (strcmp(line, "neonfont") == 0) {
        const int n = atoi(arg);
        if (n >= 0 && n <= 1) {
            boost_theme_set_neon_font((boost_neon_font_t)n);
            stream_rebuild();
        }
    } else if (strcmp(line, "preset") == 0) {
        const int n = atoi(arg);
        if (n >= 0 && n <= 3) {
            boost_theme_set_neon_preset((boost_neon_preset_t)n);
            stream_rebuild();
        }
    } else if (strcmp(line, "page") == 0) {
        if (strcmp(arg, "boost") == 0) {
            boost_page_show(BOOST_PAGE_BOOST);
        } else if (strcmp(arg, "tpms") == 0) {
            boost_page_show(BOOST_PAGE_TPMS);
        }
    } else if (strcmp(line, "tpms") == 0) {
        if (strcmp(arg, "normal") == 0) {
            boost_tpms_mock_set_scenario(BOOST_TPMS_MOCK_NORMAL);
        } else if (strcmp(arg, "stale") == 0) {
            boost_tpms_mock_set_scenario(BOOST_TPMS_MOCK_STALE);
        } else if (strcmp(arg, "disconnected") == 0) {
            boost_tpms_mock_set_scenario(BOOST_TPMS_MOCK_DISCONNECTED);
        }
    } else if (strcmp(line, "needle") == 0) {
        if (strcmp(arg, "red") == 0) {
            boost_theme_set_vault_needle_red(true);
            stream_rebuild();
        } else if (strcmp(arg, "green") == 0) {
            boost_theme_set_vault_needle_red(false);
            stream_rebuild();
        }
    } else if (strcmp(line, "tail") == 0) {
        if (strcmp(arg, "on") == 0) {
            boost_theme_set_vault_needle_tail(true);
            stream_rebuild();
        } else if (strcmp(arg, "off") == 0) {
            boost_theme_set_vault_needle_tail(false);
            stream_rebuild();
        }
    } else if (strcmp(line, "dynoblack") == 0) {
        if (strcmp(arg, "on") == 0) {
            boost_theme_set_dyno_true_black(true);
            stream_rebuild();
        } else if (strcmp(arg, "off") == 0) {
            boost_theme_set_dyno_true_black(false);
            stream_rebuild();
        }
    } else if (strcmp(line, "overlay") == 0) {
        /* Physical settings overlay: show/hide, jump to a page, or step the
         * page cycle the same way a horizontal swipe does. */
        char *sub = arg;
        while (*sub == ' ' || *sub == '\t') sub++;
        char *rest = sub;
        while (*rest != '\0' && *rest != ' ' && *rest != '\t') rest++;
        if (*rest != '\0') {
            *rest++ = '\0';
            while (*rest == ' ' || *rest == '\t') rest++;
        }
        if (strcmp(sub, "show") == 0) {
            boost_page_qr_show();
        } else if (strcmp(sub, "hide") == 0) {
            boost_page_qr_dismiss();
        } else if (strcmp(sub, "page") == 0) {
            char *stop = NULL;
            const long n = strtol(rest, &stop, 10);
            if (stop != rest && *stop == '\0' && n >= 0 && n <= 2) {
                boost_page_qr_show_page((int)n);
            } else {
                fprintf(stderr, "[stream] overlay page out of range (0..2): %s\n", rest);
            }
        } else if (strcmp(sub, "next") == 0) {
            /* swipe_left advances: 0 -> 1 -> 2 -> 0 (see boost_page.c). */
            boost_page_qr_swipe_left();
        } else if (strcmp(sub, "prev") == 0) {
            /* swipe_right walks back: 0 -> 2 -> 1 -> 0. */
            boost_page_qr_swipe_right();
        } else {
            fprintf(stderr, "[stream] unknown overlay command: %s\n", arg);
        }
    } else if (strcmp(line, "ref") == 0) {
        bool known = true;
        if (strcmp(arg, "rel") == 0) {
            boost_theme_set_pressure_absolute(false);
        } else if (strcmp(arg, "abs") == 0) {
            boost_theme_set_pressure_absolute(true);
        } else {
            known = false;
            fprintf(stderr, "[stream] unknown ref: %s\n", arg);
        }
        if (known) {
            stream_rebuild();
            /* The overlay page was built before the mode changed, so its
             * REL/ABS square would keep the old state. Rebuild the
             * open page in place, exactly as the device's own reference button
             * does; without this the panel can command a mode the overlay
             * still contradicts. */
            const int page = boost_page_qr_page();
            if (page >= 0) boost_page_qr_show_page(page);
        }
    } else if (strcmp(line, "atmosphere") == 0) {
        char *stop = NULL;
        const float v = strtof(arg, &stop);
        if (stop != arg && v >= 50.0f && v <= 120.0f) {
            boost_sim_set_atmosphere_kpa(v);
            /* The dial numerals BAKE the reference at scene build, so a changed
             * baseline must rebuild exactly like a mode change does - otherwise
             * the scale keeps the old atmosphere while the readout moves, which
             * is the same disagreement the boot-order fix removes on the real
             * device. */
            stream_rebuild();
        } else {
            fprintf(stderr, "[stream] atmosphere out of range (50..120 kPa): %s\n", arg);
        }
    } else if (strcmp(line, "quit") == 0) {
        s_stream_quit = true;
    } else {
        fprintf(stderr, "[stream] unknown command: %s\n", line);
    }
}

static void stream_read_stdin(void)
{
    if (!s_stream_stdin_open) return;
    char tmp[512];
    for (;;) {
        const ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;   /* EAGAIN: no more input right now */
        }
        if (n == 0) {
            s_stream_stdin_open = false;
            break;
        }
        size_t room = sizeof(s_stream_cmd) - 1 - s_stream_cmd_len;
        size_t take = (size_t)n < room ? (size_t)n : room;
        memcpy(s_stream_cmd + s_stream_cmd_len, tmp, take);
        s_stream_cmd_len += take;
    }
}

static bool stream_drain_lines(void)
{
    bool handled = false;
    size_t start = 0;
    for (size_t i = 0; i < s_stream_cmd_len; i++) {
        if (s_stream_cmd[i] != '\n') continue;
        s_stream_cmd[i] = '\0';
        stream_exec(s_stream_cmd + start);
        handled = true;
        start = i + 1;
    }
    if (start > 0) {
        memmove(s_stream_cmd, s_stream_cmd + start, s_stream_cmd_len - start);
        s_stream_cmd_len -= start;
    }
    return handled;
}

static int run_stream(int frame_fd)
{
    /* stdin must never block the render loop. */
    const int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    boost_sim_init();
    boost_tpms_init();
    boost_page_create();
    snprintf(s_stream_theme, sizeof(s_stream_theme), "%s", boost_theme_default()->id);
    if (s_stream_start_theme != NULL) {
        stream_apply_theme(s_stream_start_theme);
    }
    pump_lvgl(50);

    uint32_t seq = 0;
    uint32_t now_ms = 0;
    uint32_t last_tpms = 0;
    double last_status = 0.0;
    bool ok = true;

    while (!s_stream_quit && ok) {
        stream_read_stdin();
        const bool handled = stream_drain_lines();

        const boost_sample_t sample = boost_theme_demo_mode()
            ? boost_sim_tick()
            : stream_fixed_sample();
        boost_page_update(&sample);

        /* Host sim has no BLE transport, so the mock is the only TPMS source
         * (same as --tpms/--screenshot), ticked on the firmware's 250 ms cadence. */
        if (now_ms - last_tpms >= 250u) {
            boost_tpms_mock_tick(now_ms);
            boost_tpms_snapshot_t snapshot;
            boost_tpms_get_snapshot(&snapshot);
            boost_page_update_tpms(&snapshot);
            last_tpms = now_ms;
        }

        lv_tick_inc(STREAM_FRAME_PERIOD_MS);
        lv_timer_handler();
        now_ms += STREAM_FRAME_PERIOD_MS;

        /* Machine-readable status on stderr, where the panel's stderr reader
         * picks it up: the exact reading on screen, which the panel cannot
         * decode from pixels. Written BEFORE the frame it describes so the
         * panel's /shot names the file from the same state it captures, and
         * forced (not just throttled) after a command so the new state is
         * known before the next frame lands. */
        const double t = sim_now_ms();
        if (handled || t - last_status >= 100.0) {
            printf("[PANEL] psi=%.2f demo=%d overlay=%d ref=%d\n",
                   (double)sample.psi, sample.demo ? 1 : 0,
                   boost_page_qr_page(), boost_theme_pressure_absolute() ? 1 : 0);
            fflush(stdout);
            last_status = t;
        }

        ok = stream_emit_frame(frame_fd, seq++);

        if (!s_stream_stdin_open) {
            usleep(STREAM_FRAME_PERIOD_MS * 1000);
            continue;
        }
        /* Wait for a command or the next frame deadline, whichever is first. */
        struct timeval tv = { 0, STREAM_FRAME_PERIOD_MS * 1000 };
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(STDIN_FILENO, &rfds);
        select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv);
    }

    close(frame_fd);
    /* A failed frame write means the reader went away; the process exiting is
     * the correct signal for the panel's supervisor. */
    return 0;
}

int main(int argc, char **argv)
{
    bool window = false;
    bool audit = false;
    bool tpms = false;
    bool chase = false;
    bool qr_test = false;
    bool stream = false;
    int audit_seconds = 20;
    const char *shot_dir = "preview/sim";
    const char *theme_id = NULL;
    const char *tpms_scenario = NULL;
    const char *chase_dir = NULL;

    /* --stream reserves fd 1 for binary BGFR frames and pushes EVERYTHING the
     * firmware prints (its host ESP_LOG shim is a plain printf) to stderr, so
     * no log line can ever corrupt the byte stream. This must happen before
     * boost_theme_init() logs anything, hence the pre-scan. */
    int stream_fd = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--stream") == 0) stream = true;
    }
    if (stream) {
        stream_fd = dup(STDOUT_FILENO);
        if (stream_fd < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            perror("stream: redirect stdout");
            return 1;
        }
    }

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
        } else if (strcmp(argv[i], "--neon-font") == 0) {
            /* Which readout typeface to render: 0=SF Alien (default),
             * 1=Doto. Same init trap as --neon-layout. */
            if (i + 1 < argc) {
                const char *v = argv[++i];
                const int n = atoi(v);
                if (n < 0 || n > 1) {
                    fprintf(stderr, "unknown neon font: %s (0=sf-alien 1=doto)\n", v);
                    return 1;
                }
                boost_theme_set_neon_font((boost_neon_font_t)n);
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
        } else if (strcmp(argv[i], "--unit") == 0) {
            /* Global pressure-display unit. Persisted on device; on the host
             * boost_theme_set_pressure_unit just sets the working value, so a
             * single build can render psi/bar/kPa faces for verification. */
            if (i + 1 < argc) {
                const char *v = argv[++i];
                boost_unit_t u;
                if (!boost_units_parse(v, &u)) {
                    fprintf(stderr, "unknown unit: %s (psi|bar|kPa)\n", v);
                    return 1;
                }
                boost_theme_set_pressure_unit(u);
            }
        } else if (strcmp(argv[i], "--qr-test") == 0) {
            qr_test = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') shot_dir = argv[++i];
        } else if (strcmp(argv[i], "--stream") == 0) {
            stream = true;   /* already pre-scanned above */
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
    if (stream) {
        s_stream_start_theme = theme_id;
        return run_stream(stream_fd);
    }
    if (audit) {
        return run_audit(theme_id, audit_seconds);
    }
    if (tpms) {
        return run_tpms(shot_dir, tpms_scenario);
    }
    if (chase) {
        return run_chase(chase_dir ? chase_dir : "preview/chase");
    }
    if (qr_test) {
        return run_qr_test(shot_dir);
    }
    return run_screenshots(shot_dir, theme_id);
}
