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
#include "drivers/sdl/lv_sdl_window.h"
#include "drivers/sdl/lv_sdl_mouse.h"

#include "boost_gauge.h"
#include "boost_sim.h"

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

static void apply_state(const shot_state_t *st)
{
    boost_sample_t sample = {
        .psi = st->psi,
        .peak_psi = st->peak,
        .demo = true,
    };
    boost_gauge_update(&sample);
    pump_lvgl(80);
}

static int run_screenshots(const char *out_dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", out_dir);
    if (system(cmd) != 0) {
        fprintf(stderr, "failed to create %s\n", out_dir);
        return 1;
    }

    boost_sim_init();
    boost_gauge_create();
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
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", anim_dir);
    system(cmd);

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

/**
 * Headless path: custom memory display, no SDL window.
 */
static uint8_t s_fb[DISP_W * DISP_H * 4];

static void headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LV_UNUSED(disp);
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
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

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s [--screenshot DIR]   headless snapshots (default DIR=preview/sim)\n"
            "  %s --window             SDL window (use xvfb-run if headless)\n",
            argv0, argv0);
}

int main(int argc, char **argv)
{
    bool window = false;
    const char *shot_dir = "preview/sim";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--window") == 0) {
            window = true;
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
        return run_window();
    }

    setup_headless_display();
    return run_screenshots(shot_dir);
}
