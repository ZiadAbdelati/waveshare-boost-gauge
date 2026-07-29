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
#include <unistd.h>

#include "lvgl.h"
#ifdef SIM_HAVE_SDL
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"
#endif

#include "boost_gauge.h"
#include "boost_sim.h"
#include "boost_theme.h"

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

static int run_screenshots(const char *out_dir, const char *theme_id)
{
    /* Portable: "mkdir -p" is not available on the Windows shell. */
    sim_mkdir(out_dir);

    boost_sim_init();
    boost_gauge_create();
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
    return 0;
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
    boost_gauge_create();

    while (1) {
        const boost_sample_t sample = boost_sim_tick();
        boost_gauge_update(&sample);
        lv_timer_handler();
        lv_tick_inc(16);
        usleep(16000);
    }
    return 0;
}
#endif /* SIM_HAVE_SDL */

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
    boost_gauge_create();
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
    uint64_t stale_px_total = 0;
    uint32_t stale_frames = 0;
    uint64_t stale_worst = 0;
    uint32_t compares = 0;
    uint64_t severe = 0;
    int reported = 0;

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
        lv_tick_inc(16);
        lv_timer_handler();
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
    printf("  flushed px/cycle   : mean %.0f  max %llu\n",
           rendered ? (double)px_total / (double)rendered : 0.0,
           (unsigned long long)px_max);
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
            "  (all modes accept --theme ID)\n",
            argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    bool window = false;
    bool audit = false;
    int audit_seconds = 20;
    const char *shot_dir = "preview/sim";
    const char *theme_id = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0) {
            window = true;
        } else if (strcmp(argv[i], "--audit") == 0) {
            audit = true;
        } else if (strcmp(argv[i], "--seconds") == 0) {
            if (i + 1 < argc) audit_seconds = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--theme") == 0) {
            if (i + 1 < argc) theme_id = argv[++i];
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
    return run_screenshots(shot_dir, theme_id);
}
