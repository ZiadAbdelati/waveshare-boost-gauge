#include "boost_gauge.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "boost_media_store.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif
#include "lvgl.h"
#include "boost_brightness.h"
#ifdef ESP_PLATFORM
#include "boost_model.h"
#include "boost_display.h"
#include "boost_sensors.h"
#endif
#if LV_USE_GIF
#include "libs/gif/lv_gif.h"
#endif

/* The cached face lives in PSRAM on device, plain heap on the simulator. */
#ifdef ESP_PLATFORM
#define BG_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define BG_FREE(p)  heap_caps_free(p)
#else
#define BG_ALLOC(n) malloc(n)
#define BG_FREE(p)  free(p)
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Multi-style gauge face.
 * ----------------------
 * A theme selects a `style`, and each style is a separate LVGL scene with its
 * own build/update/teardown. Changing theme tears the scene down and rebuilds
 * it. Only the moving parts invalidate per frame; static art (ticks, chevrons,
 * scanlines) is drawn by a face object that LVGL clips to the dirty region.
 *
 * The `arc` style is the original verified face and is preserved byte-for-byte
 * in its geometry/invalidation logic — it is the one gated by the 60 FPS
 * hardware cadence guard.
 */

#define COLOR_VOID   0x000000
#define FACE_BG      COLOR_VOID

#define DEFAULT_PSI_MIN       (-15.0f)
#define DEFAULT_PSI_MAX       (10.0f)
#define DEFAULT_PSI_OVERBOOST (8.0f)
#define DEFAULT_ZERO_ANGLE    236.25f
#define ARC_START     135
#define ARC_END       45
#define ARC_RANGE     270

#define DISP_SIZE     466
#define ARC_DIAMETER  462
#define ARC_WIDTH     45
#define ZERO_LINE_W   20
#define ZERO_GAP_VAC_DEG   3.6f
#define ZERO_GAP_BOOST_DEG 4.00f
#define TICK_FONT     (&lv_font_montserrat_20)
#define TICK_RADIUS   160.0f
#define VALUE_SLOT_WIDTH   26
#define VALUE_SLOT_HEIGHT  64
#define VALUE_SIGN_X       (-69)
#define VALUE_TENS_X       (-43)
#define VALUE_ONES_X       (-17)
#define VALUE_DECIMAL_X    8
#define VALUE_TENTHS_X     30
#define HOLD_DIM_MS   2000
#define WELL_SIZE     DISP_SIZE

/*
 * AMOLED burn-in countermeasure.
 * -----------------------------
 * One face is shown for hours at a time at 85-92% brightness with high-contrast
 * art pinned to fixed pixels — tick rings, "VAULT-TEC", reticle brackets, the
 * readout outline. Blue emitters age fastest, then green, then red, so those
 * shapes are exactly what would ghost. The whole scene therefore hangs off a
 * single container (`s_root`) that is nudged a pixel or two every so often,
 * which spreads each static edge over four columns and four rows.
 *
 * Range is [-2, +1], not a symmetric +/-3. The outermost art is drawn at an
 * OUTER radius of 231 about (233,233) on a 466 px panel (lv_draw_arc takes
 * `radius` as the outer edge), so it already reaches column 464 and row 464:
 * exactly one free pixel to the right/bottom and two to the left/top. A wider
 * excursion clips the vault bezel ring and the dyno-cell value arc against the
 * glass, which is far more visible than the aging it would have prevented.
 * Verified extents at full deflection: value arc 0..465, vault bezel 0..465,
 * vault ticks 7..458, hud ring 37..440. Nothing leaves the panel.
 *
 * The offsets walk a ring rather than a raster so consecutive steps are never
 * more than ~1.4 px apart, and so the sequence never dwells near one place.
 * Every dy in {-2,-1,0,1} appears, which is also every phase of the vault's
 * 4-row scanline overlay — without that the three rows between scanlines would
 * carry full duty forever and burn in as stripes.
 */
#define PXSHIFT_MIN         (-2)
#define PXSHIFT_MAX         (1)
static const int8_t k_pxshift[][2] = {
    {  0,  0 }, {  1, -1 }, {  1, -2 }, {  0, -2 },
    { -1, -2 }, { -2, -1 }, { -2,  0 }, { -1,  1 },
};
#define PXSHIFT_STEPS (sizeof(k_pxshift) / sizeof(k_pxshift[0]))

/* 90 s a step, so the eight-step ring closes in 12 minutes. Minutes, not
 * seconds: a step is a full-screen repaint, and one of those per minute is a
 * rounding error against the frame budget while still being far faster than
 * emitter aging. */
/* Period is user-set (settings page -> Display -> Pixel shift); see
 * BOOST_PXSHIFT_SEC_* in boost_theme.h for the range and the default. Read
 * per tick rather than cached, so a change over the API takes effect on the
 * next sample without a scene rebuild. */
/* A step dirties all 217k pixels — about 45 ms, three dropped frames. Held
 * back until the reading has been steady for a moment so the hitch lands where
 * nothing is moving; mid-sweep it would read as a stutter in the needle. */
#define PXSHIFT_SETTLE_MS   1500u
/* Above the signal's own noise floor. At 0.25 this never released: the demo
 * source alone carries 0.18 sin(17.3t) + 0.08 sin(31.1t), up to 0.52 psi
 * peak-to-peak, so the longest window inside +/-0.25 psi is 0.69 s against a
 * 1.5 s requirement - the gate passed 0.0000% of the time and every shift fell
 * through to the deadline, mid-sweep, which is exactly what it exists to
 * avoid. A real MAP sensor is noisier still. */
#define PXSHIFT_SETTLE_PSI  0.60f
/* ...but a signal that never settles must not starve the shift forever. */
/* A fixed grace, not a multiple of the period: the period is the user's own
 * choice now, and "every 90 s" that can silently mean every six minutes is
 * not the setting they asked for. 30 s is long enough to catch an idle
 * moment in ordinary driving and short enough that the worst case is still
 * recognisably the interval on the label. */
#define PXSHIFT_GRACE_MS    30000u

/* LVGL angles: 0 = east, clockwise (y down). Top of the face is 270. */
#define VAULT_A0   152.0f   /* 270 - 118 */
#define VAULT_A1   388.0f   /* 270 + 118 */
/* Dial pushed out toward the bezel; the needle stops short of the tick ring so
 * the two never share a dirty region. */
#define VAULT_TICK_OUT       224
#define VAULT_TICK_MAJOR_IN  194
#define VAULT_TICK_MINOR_IN  206
#define VAULT_NUM_R          170.0f
#define VAULT_NEEDLE_LEN     168
#define VAULT_NEEDLE_TAIL    26
#define VAULT_NEEDLE_HALFW   7
#define VAULT_HUB_R          15
/* Sits just inside the bezel ring (r=231, width 3, so its inner edge is at
 * 229.5): the tell-tale must stop where the green circle starts, not cross it. */
#define VAULT_PEAK_R         220
/* CRT overlay, mirroring the web canvas: a smooth radial darkening that starts
 * just past half radius, plus scanlines every 4th row clipped to the face. The
 * web draws both last, so they sit over the needle and digits too. */
#define VAULT_VIGN_R0        50.0f
#define VAULT_VIGN_R1        233.0f
#define VAULT_VIGN_MAX       0.60f
#define VAULT_FACE_R         231.0f
#define VAULT_SCAN_STEP      4
#define VAULT_SCAN_OPA       41 /* 0.16 * 255 */
/*
 * How many radial boxes the swept needle is invalidated in.
 *
 * The needle is a thin spoke, so its axis-aligned bounding box is mostly empty
 * air: at 45 degrees a 194 px spoke 14 px wide needs a 165 x 165 box, ~27k px
 * of which about 3k carries ink. Everything downstream is charged per box
 * pixel, not per lit pixel:
 *   - the cached face is re-blitted across the whole box;
 *   - lv_draw_sw_triangle() rasterises each of the three needle triangles over
 *     (triangle bbox AND clip area) and applies three line masks per row across
 *     the full row width, so a fat clip costs three times the box;
 *   - draw_vault_crt() emits one chord-clipped row per VAULT_SCAN_STEP rows
 *     across it;
 *   - and the panel is flushed with it.
 * Invalidating the spoke as a chain of small boxes cuts all four at once and
 * cannot change a pixel: the drawing code is untouched, only the dirty region
 * it gets clipped to.
 *
 * Measured on a host bench over a 900-render sweep, configured like the
 * firmware (466x466 RGB565, 20-line partial draw buffer, the CO5300 even/odd
 * area rounder, tile_cnt 2) - flushed px per render, panel flushes per render,
 * and host raster time:
 *     1   29913 px   4.42 flushes   0.143 ms
 *     2   15652 px   3.06 flushes   0.112 ms
 *     3   14043 px   3.99 flushes   0.116 ms
 *     4   13469 px   4.81 flushes   0.137 ms
 * (the shipped single box was 25126 px / 3.97 / 0.142 ms before the tail and
 * bulge coverage fixes below widened the one-box case.)
 * Past three the boxes stop shrinking - the fixed half-width padding dominates
 * - while the per-box object walk and an extra panel flush keep being paid.
 * Two is the pick if panel flush setup turns out to cost more than ~0.5 ms per
 * 1600 px composited. Set to 1 to restore the single whole-spoke box.
 */
#ifndef VAULT_NEEDLE_SEGS
#define VAULT_NEEDLE_SEGS    3
#endif
/* Vault-Tec mark: a ringed hub with three cog bars each side, sized to tuck
 * between the BOOST-O-METER line (y=-78) and the needle hub. */
#define VAULT_LOGO_Y         (-62)
#define VAULT_LOGO_R         15
#define VAULT_LOGO_RING_W    5
#define VAULT_LOGO_HUB_R     6
#define VAULT_LOGO_BAR_W     5
#define VAULT_LOGO_BAR_DY    10
#define VAULT_LOGO_BAR_IN    12
#define VAULT_LOGO_BAR_SHORT 34
#define VAULT_LOGO_BAR_LONG  45
#define HUD_A0     154.0f   /* 270 - 116 */
#define HUD_A1     386.0f   /* 270 + 116 */
#define HUD_BRACKET_X 126
/* Chromatic split of the ghost pass. Shared with the invalidation so the dirty
 * box can never be narrower than the pixels the ghosts actually touch. */
#define HUD_GLITCH_DX 6

/* Share Tech Mono (readouts) and Saira Condensed (labels) stand in for the
 * Consolas/Bahnschrift pairing used by the web mirror; both are OFL, so they
 * can ship in the firmware image. Compiled for host too so the simulator is
 * font-faithful. */
LV_FONT_DECLARE(alvida_big);
LV_FONT_DECLARE(font_mono_16);
LV_FONT_DECLARE(font_mono_40);
LV_FONT_DECLARE(font_cond_14);
LV_FONT_DECLARE(font_cond_18);
LV_FONT_DECLARE(font_cond_22);
LV_FONT_DECLARE(font_cond_32);
LV_FONT_DECLARE(font_cond_96);
LV_FONT_DECLARE(font_wide_22);
LV_FONT_DECLARE(font_wide_32);
#define BIGDIGIT_FONT  (&alvida_big)
#define F_MONO16 (&font_mono_16)
#define F_MONO40 (&font_mono_40)
#define F_COND14 (&font_cond_14)
#define F_COND18 (&font_cond_18)
#define F_COND22 (&font_cond_22)
#define F_COND32 (&font_cond_32)
#define F_COND96 (&font_cond_96)
#define F_WIDE22 (&font_wide_22)
#define F_WIDE32 (&font_wide_32)
#define HUD_VALUE_FONT F_COND96

static const char *TAG = "boost_gauge";

/* ---- shared scene state -------------------------------------------------- */
static lv_obj_t *s_well;
static boost_gauge_style_t s_built_style = BOOST_STYLE_ARC;

/* Every scene object is a child of this; moving it moves the whole face. It is
 * styleless and exactly screen-sized, so it costs one bounds test per redraw
 * and paints nothing. Offsets survive a theme rebuild deliberately: restarting
 * the ring on every theme change would park the new face back at (0,0). */
static lv_obj_t *s_root;
static int32_t s_px_dx;
static int32_t s_px_dy;
static uint8_t s_px_step;
static uint32_t s_px_step_ms;
static uint32_t s_px_settled_ms;
static float s_px_ref_psi;

/* ---- arc style ----------------------------------------------------------- */
static lv_obj_t *s_arc_track;
static lv_obj_t *s_arc_value_canvas;
static lv_obj_t *s_zero_notch;
static lv_obj_t *s_value_sign_label;
static lv_obj_t *s_value_tens_label;
static lv_obj_t *s_value_ones_label;
static lv_obj_t *s_value_decimal_label;
static lv_obj_t *s_value_tenths_label;
static lv_obj_t *s_unit_label;
static lv_obj_t *s_peak_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_zone_label;
static lv_obj_t *s_tick_labels[5];

/* ---- vault style --------------------------------------------------------- */
/* Fixed readout slots (montserrat_40: digit advance ~23 px, '.' ~11, '-' ~15)
 * so the value never slides horizontally as digits change. */
enum {
    VAULT_SLOT_SIGN = 0,
    VAULT_SLOT_TENS,
    VAULT_SLOT_ONES,
    VAULT_SLOT_DOT,
    VAULT_SLOT_TENTHS,
    VAULT_SLOT_HUNDREDTHS,
    VAULT_SLOT_COUNT,
};
/* Share Tech Mono advances a uniform 21.6 px at 40 px. */
/* IBM Plex Mono advances a uniform 24 px at 40 px. Every slot is always
 * filled (sign + 2 integer digits + 2 decimals), so the readout is centred
 * by construction and can never shift as the value changes. */
static const int k_vault_slot_x[VAULT_SLOT_COUNT] = { -60, -36, -12, 12, 36, 60 };

static lv_obj_t *s_vault_bg;
static uint8_t *s_vault_bg_buf;
static lv_obj_t *s_vault_crt;
static lv_obj_t *s_vault_peak_mark;
static float s_vault_peak_deg;
static lv_obj_t *s_vault_needle;
static lv_obj_t *s_vault_window;
static lv_obj_t *s_vault_slot[VAULT_SLOT_COUNT];
static lv_obj_t *s_vault_peak;
static lv_obj_t *s_vault_alert;
static lv_obj_t *s_vault_alert_marks;
static float s_vault_needle_deg;
/* Overboost recolours the needle. Without tracking it, a colour flip with a
 * near-static angle left the old yellow needle on screen. */
static bool s_vault_needle_over;

/* ---- hud style ----------------------------------------------------------- */
/* Right-aligned fixed slots (hud_big 76 px: digit advance ~51, '.' ~17,
 * '-' ~29). Decimal and tenths are pinned; integer digits grow left. */
enum {
    HUD_SLOT_TENS = 0,
    HUD_SLOT_ONES,
    HUD_SLOT_DOT,
    HUD_SLOT_TENTHS,
    HUD_SLOT_COUNT,
};
/* Saira Condensed 96: digit advance ~46 px, '.' ~21.5, '-' ~29. */
/* IBM Plex Sans Condensed BoldItalic 96: digit 52 px, '.' 27, '-' 34. */
static const int k_hud_slot_x[HUD_SLOT_COUNT] = { -50, 2, 42, 82 };
#define HUD_SIGN_ONES_X (-45)
#define HUD_SIGN_TENS_X (-97)
#define HUD_VALUE_Y     (-2)

static lv_obj_t *s_hud_face;
static lv_obj_t *s_hud_bg;
static uint8_t *s_hud_bg_buf;
static lv_obj_t *s_hud_fill;
static lv_obj_t *s_hud_slot[HUD_SLOT_COUNT];
static lv_obj_t *s_hud_sign;
static int s_hud_sign_x = HUD_SIGN_ONES_X;
static lv_obj_t *s_hud_glitch;
/* Angle/value the fill was last painted at. Drawing and invalidating must
 * agree: a skipped invalidation with a moved draw leaves stale pixels. */
static float s_hud_fill_deg;
static float s_hud_fill_psi;
static bool s_hud_fill_valid;
static char s_hud_val_str[12];
static float s_hud_prev_psi;
static lv_obj_t *s_hud_map;
static lv_obj_t *s_hud_pk;
static lv_obj_t *s_hud_sys;

/* ---- bigdigit style ------------------------------------------------------ */
static lv_obj_t *s_big_bg;
static lv_obj_t *s_big_minus;
static lv_obj_t *s_big_tens;
static lv_obj_t *s_big_ones;
static lv_obj_t *s_big_dot;
static lv_obj_t *s_big_tenths;
static lv_obj_t *s_big_unit;
static lv_obj_t *s_big_zone;
static lv_obj_t *s_big_peak;
static int s_big_bg_step = -1;
static int s_big_text_step = -1;
static uint32_t s_big_text_color;
/* Ground recolour is spread across these bands, one per update tick. */
#define BIG_BANDS 1
static lv_obj_t *s_big_band[BIG_BANDS];
static int s_big_band_next = BIG_BANDS;
static uint32_t s_big_band_color;

#if LV_USE_GIF
static lv_obj_t *s_media_gif;
static lv_image_dsc_t s_media_dsc;
#endif

static float s_display_psi;
static float s_peak_psi;
static bool s_ui_ready;
static bool s_hold_dim_fired;
static uint32_t s_press_start_ms;
static char s_theme_id[BOOST_THEME_ID_MAX];
static float s_psi_min = DEFAULT_PSI_MIN;
static float s_psi_max = DEFAULT_PSI_MAX;
static float s_psi_overboost = DEFAULT_PSI_OVERBOOST;
static float s_zero_angle = DEFAULT_ZERO_ANGLE;
static float s_tick_psi[5];

#ifndef ESP_PLATFORM
static const boost_theme_t *s_host_theme;
#endif

static float psi_to_angle(float psi);
static void build_scene(boost_gauge_style_t style);
static void destroy_scene(void);

static void load_range_from_config(void)
{
#ifdef ESP_PLATFORM
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    s_psi_min = cfg.psi_min;
    s_psi_max = cfg.psi_max;
    s_psi_overboost = cfg.psi_overboost;
    s_zero_angle = cfg.zero_angle;
#else
    s_psi_min = DEFAULT_PSI_MIN;
    s_psi_max = DEFAULT_PSI_MAX;
    s_psi_overboost = DEFAULT_PSI_OVERBOOST;
    s_zero_angle = DEFAULT_ZERO_ANGLE;
#endif
}

static lv_color_t c(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t);

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/*
 * Face centre in SCREEN coordinates, including the burn-in offset.
 *
 * Draw callbacks get a layer in absolute screen space, so a callback that
 * hardcodes DISP_SIZE/2 keeps painting at the unshifted centre while the object
 * it belongs to has moved — the moving parts detach from the static art. Every
 * such site, and every invalidation that pairs with one, must come through
 * these. Objects placed with lv_obj_align()/lv_obj_set_pos() are parent-
 * relative and must NOT add the offset again.
 *
 * At zero offset these are bit-identical to the constants they replaced, so the
 * default render is unchanged.
 */
static inline float px_cx(void) { return DISP_SIZE * 0.5f + (float)s_px_dx; }
static inline float px_cy(void) { return DISP_SIZE * 0.5f + (float)s_px_dy; }
static inline int32_t px_icx(void) { return DISP_SIZE / 2 + s_px_dx; }
static inline int32_t px_icy(void) { return DISP_SIZE / 2 + s_px_dy; }

static uint32_t scale_rgb(uint32_t rgb, float k)
{
    const uint32_t r = (uint32_t)lroundf((float)((rgb >> 16) & 0xFFu) * k);
    const uint32_t g = (uint32_t)lroundf((float)((rgb >> 8) & 0xFFu) * k);
    const uint32_t b = (uint32_t)lroundf((float)(rgb & 0xFFu) * k);
    return (r << 16) | (g << 8) | b;
}

static const boost_theme_t *active_theme(void)
{
#ifdef ESP_PLATFORM
    return boost_model_active_theme();
#else
    /* Host/sim: whichever theme was last applied, defaulting to the first. */
    return s_host_theme ? s_host_theme : boost_theme_at(0);
#endif
}

static bool midpoint_is_clear(float psi)
{
    const float rad = psi_to_angle(psi) * (float)M_PI / 180.0f;
    const float overboost_rad = psi_to_angle(s_psi_overboost) * (float)M_PI / 180.0f;
    const float dx = TICK_RADIUS * (cosf(rad) - cosf(overboost_rad));
    const float dy = TICK_RADIUS * (sinf(rad) - sinf(overboost_rad));
    return dx * dx + dy * dy >= 28.0f * 28.0f;
}

static void compute_tick_psis(void)
{
    s_tick_psi[0] = s_psi_min;
    s_tick_psi[1] = 0.0f;
    const float midpoint = s_psi_max * 0.5f;
    s_tick_psi[2] = midpoint_is_clear(midpoint) ? midpoint : NAN;
    s_tick_psi[3] = s_psi_overboost;
    s_tick_psi[4] = s_psi_max;
}

static void format_tick_text(char *buf, size_t len, float psi)
{
    if (fabsf(psi) < 0.05f) {
        snprintf(buf, len, "0");
        return;
    }
    const float rounded = roundf(psi);
    if (fabsf(psi - rounded) < 0.05f) {
        snprintf(buf, len, "%d", (int)rounded);
    } else {
        snprintf(buf, len, "%.1f", (double)psi);
    }
}

/* Arc face mapping: vacuum [min,0] -> [135, zero], boost [0,max] -> [zero,405]. */
static float psi_to_angle(float psi)
{
    psi = clampf(psi, s_psi_min, s_psi_max);
    if (psi < 0.0f) {
        const float span = 0.0f - s_psi_min;
        const float t = (span > 0.0f) ? (psi - s_psi_min) / span : 1.0f;
        return (float)ARC_START + t * (s_zero_angle - (float)ARC_START);
    }
    const float span = s_psi_max;
    const float t = (span > 0.0f) ? psi / span : 0.0f;
    return s_zero_angle + t * ((float)ARC_START + (float)ARC_RANGE - s_zero_angle);
}

/* Same zero-referenced scaling projected into an arbitrary sweep, so the
 * stylized faces honor the configured zero angle proportionally. */
static float psi_to_sweep(float psi, float a0, float a1)
{
    const float span = a1 - a0;
    const float zero_at = a0 + ((s_zero_angle - (float)ARC_START) / (float)ARC_RANGE) * span;
    psi = clampf(psi, s_psi_min, s_psi_max);
    if (psi < 0.0f) {
        const float d = 0.0f - s_psi_min;
        const float t = (d > 0.0f) ? (psi - s_psi_min) / d : 1.0f;
        return a0 + t * (zero_at - a0);
    }
    const float t = (s_psi_max > 0.0f) ? psi / s_psi_max : 0.0f;
    return zero_at + t * (a1 - zero_at);
}

/* Colour-sweep quantisation, shared by big-digit and the arc/hud gradients. */
#define BIG_STEPS 24
static uint32_t big_color_for_step(const boost_theme_t *theme, int step);

/* The vacuum->boost->overboost ramp Big Digit sweeps, quantised to BIG_STEPS so
 * a fill only recolours at a step boundary rather than every frame. Shared by
 * the arc and hud gradient-fill modes. */
static uint32_t gradient_rgb_for_psi(const boost_theme_t *theme, float psi)
{
    const float lo = s_psi_min;
    const float hi = s_psi_max;
    const float t = (hi > lo) ? (clampf(psi, lo, hi) - lo) / (hi - lo) : 0.0f;
    const int step = (int)lroundf(t * (float)(BIG_STEPS - 1));
    return big_color_for_step(theme, step);
}

static lv_color_t color_for_psi(const boost_theme_t *theme, float psi)
{
    if (boost_theme_arc_gradient()) return c(gradient_rgb_for_psi(theme, psi));
    if (psi >= s_psi_overboost) return c(theme->overboost);
    if (psi >= 0.35f) return c(theme->boost);
    if (psi > -0.35f) return c(theme->text);
    return c(theme->vacuum);
}

static const char *zone_for_psi(float psi)
{
    if (psi >= s_psi_overboost) return "OVER";
    if (psi >= 0.35f) return "BOOST";
    if (psi > -0.35f) return "ATMO";
    return "VAC";
}

static void format_value_slots(char *sign, char *tens, char *ones, char *tenths, float psi)
{
    const int tenths_psi = (int)lroundf(fabsf(psi) * 10.0f);
    const int whole = tenths_psi / 10;
    *sign = psi < -0.05f ? '-' : ' ';
    *tens = whole >= 10 ? (char)('0' + whole / 10) : ' ';
    *ones = (char)('0' + whole % 10);
    *tenths = (char)('0' + tenths_psi % 10);
}

static void reset_peak_ui(void)
{
    /* Reset both source peaks so the tap works whichever source is live. */
    boost_sim_reset_peak();
#ifdef ESP_PLATFORM
    boost_sensors_reset_peak();
#endif
    s_peak_psi = fmaxf(s_display_psi, 0.0f);
    ESP_LOGI(TAG, "peak reset");
}

static void on_screen_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_press_start_ms = lv_tick_get();
        s_hold_dim_fired = false;
        return;
    }
    if (code == LV_EVENT_PRESSING) {
        if (!s_hold_dim_fired && lv_tick_elaps(s_press_start_ms) >= HOLD_DIM_MS) {
            s_hold_dim_fired = true;
            boost_brightness_toggle_max_min();
            ESP_LOGI(TAG, "brightness toggle -> %d%%", boost_brightness_get());
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_hold_dim_fired && lv_tick_elaps(s_press_start_ms) < HOLD_DIM_MS) {
            reset_peak_ui();
        }
        s_hold_dim_fired = false;
        return;
    }
    if (code == LV_EVENT_LONG_PRESSED && !s_hold_dim_fired) {
        s_hold_dim_fired = true;
        boost_brightness_toggle_max_min();
        ESP_LOGI(TAG, "brightness toggle (long_pressed) -> %d%%", boost_brightness_get());
    }
}

#if LV_USE_GIF
static void set_gauge_hidden(bool hidden)
{
    lv_obj_t *scr = lv_screen_active();
    for (uint32_t i = 0; i < lv_obj_get_child_count(scr); ++i) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
        if (child != s_media_gif) lv_obj_set_flag(child, LV_OBJ_FLAG_HIDDEN, hidden);
    }
}

static void destroy_media_gif(void)
{
    if (s_media_gif != NULL) {
        lv_obj_delete(s_media_gif);
        s_media_gif = NULL;
    }
    memset(&s_media_dsc, 0, sizeof(s_media_dsc));
    set_gauge_hidden(false);
}

static bool load_media_gif_locked(void)
{
    const uint8_t *data = NULL;
    size_t size = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    if (boost_media_store_map(&data, &size, &width, &height) != ESP_OK) return false;
    set_gauge_hidden(true);
    /* Decoder state placement is handled inside the widget (main/gif/boost_gif.c),
     * not here: heap_caps_malloc_extmem_enable() cannot reach LVGL allocations
     * because LVGL uses its own builtin pool rather than malloc. */
    ESP_LOGI(TAG, "gif alloc: internal free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    s_media_gif = lv_gif_create(lv_screen_active());
    if (s_media_gif == NULL) {
        boost_media_store_unmap();
        set_gauge_hidden(false);
        return false;
    }
    s_media_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_media_dsc.header.cf = LV_COLOR_FORMAT_RAW;
    s_media_dsc.header.w = width;
    s_media_dsc.header.h = height;
    s_media_dsc.data_size = size;
    s_media_dsc.data = data;
    lv_obj_set_size(s_media_gif, DISP_SIZE, DISP_SIZE);
    lv_obj_set_style_bg_color(s_media_gif, c(COLOR_VOID), 0);
    /* A native 466x466 clip covers the object completely, so a COVER fill is
     * 434 KB of wasted internal-SRAM writes per frame. Safe only because the
     * GIF framebuffer is zero-initialised again (main/gif/boost_gif.c). */
    lv_obj_set_style_bg_opa(s_media_gif,
                            (width == DISP_SIZE && height == DISP_SIZE) ? LV_OPA_TRANSP
                                                                        : LV_OPA_COVER,
                            0);
    lv_gif_set_color_format(s_media_gif, LV_COLOR_FORMAT_RGB565);
    lv_image_set_inner_align(s_media_gif, LV_IMAGE_ALIGN_CENTER);
    lv_gif_set_src(s_media_gif, &s_media_dsc);
    /* Where the decoder object actually landed. The extmem threshold is a
     * request, not a guarantee, and a silent fall back to PSRAM costs the
     * whole point of the exercise. */
    ESP_LOGI(TAG, "gif widget object in %s RAM, framebuffer in %s RAM",
             esp_ptr_external_ram(s_media_gif) ? "EXTERNAL" : "internal",
             esp_ptr_external_ram(((lv_image_t *)s_media_gif)) ? "EXTERNAL" : "internal");
    ESP_LOGI(TAG, "gif alloc done: internal free %u B",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!lv_gif_is_loaded(s_media_gif)) {
        lv_obj_delete(s_media_gif);
        s_media_gif = NULL;
        memset(&s_media_dsc, 0, sizeof(s_media_dsc));
        boost_media_store_unmap();
        set_gauge_hidden(false);
        return false;
    }
    lv_obj_center(s_media_gif);
    lv_obj_move_foreground(s_media_gif);
    lv_obj_clear_flag(s_media_gif, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return true;
}
#endif

/* ========================================================================== */
/*  Style: arc  (original verified face)                                      */
/* ========================================================================== */

static void value_arc_angles(float psi, float *start, float *end)
{
    const float zero_a = psi_to_angle(0.0f);
    if (psi >= 0.0f) {
        *start = zero_a + ZERO_GAP_BOOST_DEG;
        *end = fmaxf(psi_to_angle(psi), *start);
    } else {
        *start = fminf(psi_to_angle(psi), zero_a - ZERO_GAP_VAC_DEG);
        *end = zero_a - ZERO_GAP_VAC_DEG;
    }
}

static void draw_value_arc(lv_event_t *event)
{
    const float psi = *(const float *)lv_event_get_user_data(event);
    float start;
    float end;
    value_arc_angles(psi, &start, &end);
    lv_draw_arc_dsc_t dsc;
    lv_draw_arc_dsc_init(&dsc);
    dsc.color = color_for_psi(active_theme(), psi);
    dsc.width = ARC_WIDTH;
    dsc.start_angle = start;
    dsc.end_angle = end;
    dsc.center.x = px_icx();
    dsc.center.y = px_icy();
    dsc.radius = ARC_DIAMETER / 2;
    dsc.opa = LV_OPA_COVER;
    dsc.rounded = true;
    lv_draw_arc(lv_event_get_layer(event), &dsc);
}

static void invalidate_value_arc(float start, float end)
{
    for (float segment_start = start; segment_start < end;) {
        const float boundary = (floorf(segment_start / 90.0f) + 1.0f) * 90.0f;
        const float segment_end = fminf(end, boundary);
        lv_area_t area;
        lv_draw_arc_get_area(px_icx(), px_icy(), ARC_DIAMETER / 2,
                             segment_start, segment_end, ARC_WIDTH, true, &area);
        lv_obj_invalidate_area(s_arc_value_canvas, &area);
        segment_start = segment_end;
    }
}

static void set_value_arc(float psi)
{
    float old_start, old_end, new_start, new_end;
    value_arc_angles(s_display_psi, &old_start, &old_end);
    value_arc_angles(psi, &new_start, &new_end);

    if ((s_display_psi < 0.0f) != (psi < 0.0f) ||
        !lv_color_eq(color_for_psi(active_theme(), s_display_psi), color_for_psi(active_theme(), psi))) {
        invalidate_value_arc(old_start, old_end);
        invalidate_value_arc(new_start, new_end);
    } else if (psi >= 0.0f) {
        invalidate_value_arc(fminf(old_end, new_end), fmaxf(old_end, new_end));
    } else {
        invalidate_value_arc(fminf(old_start, new_start), fmaxf(old_start, new_start));
    }
}

static void place_tick_label(int idx, float psi, const char *text)
{
    const boost_theme_t *theme = active_theme();
    const float deg = psi_to_angle(psi);
    const float rad = deg * (float)M_PI / 180.0f;
    /* Plain centre, no burn-in offset: lv_obj_set_pos() below is relative to
     * the parent, and the parent is the container that carries the offset. */
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;

    lv_obj_t *lab = s_tick_labels[idx];
    if (lab == NULL) {
        /* s_root, not the screen: a tick label parented straight to the screen
         * would be the one piece of the arc face that ignores the shift. */
        lab = lv_label_create(s_root);
        s_tick_labels[idx] = lab;
        lv_obj_set_style_text_font(lab, TICK_FONT, 0);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_label_set_text(lab, text);
    lv_obj_set_style_text_color(lab, c(theme->muted), 0);
    lv_obj_update_layout(lab);

    const lv_coord_t w = lv_obj_get_width(lab);
    const lv_coord_t h = lv_obj_get_height(lab);
    float r = TICK_RADIUS;
    if (fabsf(psi) < 0.01f) r = TICK_RADIUS - 18.0f;
    const float x = cx + r * cosf(rad) - w * 0.5f;
    const float y = cy + r * sinf(rad) - h * 0.5f;
    lv_obj_set_pos(lab, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
}

static void refresh_zero_notch(void)
{
    if (s_zero_notch == NULL) return;
    static lv_point_precise_t zero_pts[2];
    const float deg = psi_to_angle(0.0f);
    const float rad = deg * (float)M_PI / 180.0f;
    /* Line points are relative to the object, which sits at the parent origin,
     * so this is parent space and must not carry the burn-in offset. */
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;
    const float r_outer = (float)ARC_DIAMETER * 0.5f - 1.0f;
    const float r_inner = r_outer - (float)ARC_WIDTH + 1.0f;
    zero_pts[0].x = cx + r_inner * cosf(rad);
    zero_pts[0].y = cy + r_inner * sinf(rad);
    zero_pts[1].x = cx + r_outer * cosf(rad);
    zero_pts[1].y = cy + r_outer * sinf(rad);
    lv_line_set_points(s_zero_notch, zero_pts, 2);
}

static void refresh_tick_labels(void)
{
    compute_tick_psis();
    for (int i = 0; i < 5; ++i) {
        if (!isfinite(s_tick_psi[i])) {
            if (s_tick_labels[i] != NULL) lv_obj_add_flag(s_tick_labels[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        char text[12];
        format_tick_text(text, sizeof(text), s_tick_psi[i]);
        place_tick_label(i, s_tick_psi[i], text);
        lv_obj_remove_flag(s_tick_labels[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *add_value_slot(lv_obj_t *scr, const char *text, int x)
{
    const boost_theme_t *theme = active_theme();
    lv_obj_t *slot = lv_label_create(scr);
    lv_label_set_text(slot, text);
    lv_obj_set_size(slot, VALUE_SLOT_WIDTH, VALUE_SLOT_HEIGHT);
    lv_obj_set_style_text_font(slot, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(slot, c(theme->text), 0);
    lv_obj_set_style_text_align(slot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(slot, LV_ALIGN_CENTER, x, -22);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    return slot;
}

static void build_arc(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    s_arc_track = lv_arc_create(scr);
    lv_obj_set_size(s_arc_track, ARC_DIAMETER, ARC_DIAMETER);
    lv_obj_center(s_arc_track);
    lv_arc_set_rotation(s_arc_track, 0);
    lv_arc_set_bg_angles(s_arc_track, ARC_START, ARC_END);
    lv_arc_set_angles(s_arc_track, ARC_START, ARC_START);
    lv_arc_set_range(s_arc_track, 0, 1000);
    lv_arc_set_value(s_arc_track, 0);
    lv_obj_remove_style(s_arc_track, NULL, LV_PART_KNOB);
    lv_obj_set_style_arc_width(s_arc_track, ARC_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc_track, ARC_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc_track, c(theme->track), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc_track, LV_OPA_0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc_track, true, LV_PART_MAIN);
    lv_obj_clear_flag(s_arc_track, LV_OBJ_FLAG_CLICKABLE);

    s_arc_value_canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(s_arc_value_canvas);
    lv_obj_set_size(s_arc_value_canvas, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_arc_value_canvas);
    lv_obj_clear_flag(s_arc_value_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_arc_value_canvas, draw_value_arc, LV_EVENT_DRAW_MAIN, &s_display_psi);

    s_zero_notch = lv_line_create(scr);
    refresh_zero_notch();
    lv_obj_set_style_line_width(s_zero_notch, ZERO_LINE_W, 0);
    lv_obj_set_style_line_color(s_zero_notch, c(theme->zero), 0);
    lv_obj_set_style_line_rounded(s_zero_notch, true, 0);
    lv_obj_set_style_line_opa(s_zero_notch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_zero_notch, LV_OBJ_FLAG_CLICKABLE);

    refresh_tick_labels();

    s_zone_label = lv_label_create(scr);
    lv_label_set_text(s_zone_label, "ATMO");
    lv_obj_set_style_text_font(s_zone_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_zone_label, c(theme->text), 0);
    lv_obj_align(s_zone_label, LV_ALIGN_CENTER, 0, -88);
    lv_obj_clear_flag(s_zone_label, LV_OBJ_FLAG_CLICKABLE);

    s_value_sign_label = add_value_slot(scr, " ", VALUE_SIGN_X);
    s_value_tens_label = add_value_slot(scr, " ", VALUE_TENS_X);
    s_value_ones_label = add_value_slot(scr, "0", VALUE_ONES_X);
    s_value_decimal_label = add_value_slot(scr, ".", VALUE_DECIMAL_X);
    s_value_tenths_label = add_value_slot(scr, "0", VALUE_TENTHS_X);

    s_unit_label = lv_label_create(scr);
    lv_label_set_text(s_unit_label, "PSI");
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_unit_label, c(theme->muted), 0);
    lv_obj_align(s_unit_label, LV_ALIGN_CENTER, 0, 26);
    lv_obj_clear_flag(s_unit_label, LV_OBJ_FLAG_CLICKABLE);

    s_peak_label = lv_label_create(scr);
    lv_label_set_text(s_peak_label, "PEAK  0.0");
    lv_obj_set_style_text_font(s_peak_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_peak_label, c(theme->boost), 0);
    lv_obj_align(s_peak_label, LV_ALIGN_CENTER, 0, 54);
    lv_obj_clear_flag(s_peak_label, LV_OBJ_FLAG_CLICKABLE);

    s_mode_label = lv_label_create(scr);
    lv_label_set_text(s_mode_label, "DEMO");
    lv_obj_set_style_text_font(s_mode_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_mode_label, c(theme->muted), 0);
    lv_obj_align(s_mode_label, LV_ALIGN_CENTER, 0, 82);
}

static void update_arc(const boost_sample_t *sample, const boost_theme_t *theme)
{
    set_value_arc(sample->psi);

    const lv_color_t col = color_for_psi(theme, sample->psi);
    const char *zone = zone_for_psi(sample->psi);
    if (strcmp(lv_label_get_text(s_zone_label), zone) != 0) {
        lv_obj_set_style_text_color(s_zone_label, col, 0);
        lv_label_set_text(s_zone_label, zone);
    }
    const lv_color_t value_color = sample->psi >= s_psi_overboost ? c(theme->overboost) : c(theme->text);
    lv_obj_t *value_slots[] = {
        s_value_sign_label, s_value_tens_label, s_value_ones_label,
        s_value_decimal_label, s_value_tenths_label,
    };
    for (size_t i = 0; i < sizeof(value_slots) / sizeof(value_slots[0]); ++i) {
        if (!lv_color_eq(lv_obj_get_style_text_color(value_slots[i], 0), value_color)) {
            lv_obj_set_style_text_color(value_slots[i], value_color, 0);
        }
    }

    char sign[2] = {0}, tens[2] = {0}, ones[2] = {0}, tenths[2] = {0};
    format_value_slots(sign, tens, ones, tenths, sample->psi);
    if (strcmp(lv_label_get_text(s_value_sign_label), sign) != 0) lv_label_set_text(s_value_sign_label, sign);
    if (strcmp(lv_label_get_text(s_value_tens_label), tens) != 0) lv_label_set_text(s_value_tens_label, tens);
    if (strcmp(lv_label_get_text(s_value_ones_label), ones) != 0) lv_label_set_text(s_value_ones_label, ones);
    if (strcmp(lv_label_get_text(s_value_tenths_label), tenths) != 0) lv_label_set_text(s_value_tenths_label, tenths);

    char buf[32];
    snprintf(buf, sizeof(buf), "PEAK  %.1f", (double)s_peak_psi);
    if (strcmp(lv_label_get_text(s_peak_label), buf) != 0) lv_label_set_text(s_peak_label, buf);
    const lv_color_t peak_color = s_peak_psi >= s_psi_overboost ? c(theme->overboost) : c(theme->boost);
    if (!lv_color_eq(lv_obj_get_style_text_color(s_peak_label, 0), peak_color)) {
        lv_obj_set_style_text_color(s_peak_label, peak_color, 0);
    }

    /* Real-sensor mode carries no DEMO indicator; the sweep shows it as before. */
    const char *mode = sample->demo ? "DEMO" : "";
    if (strcmp(lv_label_get_text(s_mode_label), mode) != 0) lv_label_set_text(s_mode_label, mode);
}

/* True when the dirty region can reach `r` from the face centre. A digit
 * update dirties a rect near the middle, which cannot touch the tick ring, so
 * the static ring art can skip entirely instead of re-stroking every frame. */
static bool clip_reaches_radius(lv_layer_t *layer, float cx, float cy, float r)
{
    const lv_area_t *ca = &layer->_clip_area;
    const float dx = fmaxf(fabsf((float)ca->x1 - cx), fabsf((float)ca->x2 - cx));
    const float dy = fmaxf(fabsf((float)ca->y1 - cy), fabsf((float)ca->y2 - cy));
    return (dx * dx + dy * dy) >= (r * r);
}

/* ========================================================================== */
/*  Style: vault  (Vault-Tec phosphor needle dial)                            */
/* ========================================================================== */

/* Paint the whole static face into an off-screen canvas ONCE. Redraws then
 * become a blit instead of re-rasterising a bezel, 41 ticks, vignette rings and
 * scanlines inside every dirty region. This is what makes the needle smooth and
 * what makes the vignette/scanlines affordable at all. */
static void paint_vault_background(lv_obj_t *canvas, const boost_theme_t *theme)
{
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    /* face */
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = c(boost_theme_vault_face());
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = { 0, 0, DISP_SIZE - 1, DISP_SIZE - 1 };
    lv_draw_rect(&layer, &bg, &full);

    /* outer bezel ring */
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = c(theme->muted);
    arc.width = 3;
    arc.radius = 231;
    arc.start_angle = 0;
    arc.end_angle = 360;
    arc.center.x = DISP_SIZE / 2;
    arc.center.y = DISP_SIZE / 2;
    arc.opa = LV_OPA_40;
    lv_draw_arc(&layer, &arc);

    /* tick ring */
    lv_draw_line_dsc_t ln;
    for (int i = 0; i <= 40; ++i) {
        const float v = s_psi_min + (s_psi_max - s_psi_min) * (float)i / 40.0f;
        const bool major = (i % 5) == 0;
        const float rad = psi_to_sweep(v, VAULT_A0, VAULT_A1) * (float)M_PI / 180.0f;
        const float r0 = major ? (float)VAULT_TICK_MAJOR_IN : (float)VAULT_TICK_MINOR_IN;
        lv_draw_line_dsc_init(&ln);
        ln.color = (v >= s_psi_overboost) ? c(theme->overboost) : c(theme->text);
        ln.width = major ? 4 : 2;
        ln.opa = (v >= s_psi_overboost) ? LV_OPA_COVER : LV_OPA_80;
        ln.p1.x = cx + r0 * cosf(rad);
        ln.p1.y = cy + r0 * sinf(rad);
        ln.p2.x = cx + (float)VAULT_TICK_OUT * cosf(rad);
        ln.p2.y = cy + (float)VAULT_TICK_OUT * sinf(rad);
        lv_draw_line(&layer, &ln);
    }

    /* Zero notch: the thick round-capped phosphor tick the web draws at the
     * configured zero angle. Scaled from the web's 178..206 by 224/206. */
    {
        const float zrad = psi_to_sweep(0.0f, VAULT_A0, VAULT_A1) * (float)M_PI / 180.0f;
        lv_draw_line_dsc_init(&ln);
        ln.color = c(theme->text);
        ln.width = 10;
        ln.opa = LV_OPA_COVER;
        ln.round_start = true;
        ln.round_end = true;
        ln.p1.x = cx + 193.0f * cosf(zrad);
        ln.p1.y = cy + 193.0f * sinf(zrad);
        ln.p2.x = cx + (float)VAULT_TICK_OUT * cosf(zrad);
        ln.p2.y = cy + (float)VAULT_TICK_OUT * sinf(zrad);
        lv_draw_line(&layer, &ln);
    }

    /* Vault-Tec mark, sitting under the BOOST-O-METER line. Baked into the
     * cached face, so its cost is paid once at scene build and never again -
     * the needle sweeps over it as a foreground object. Drawn from primitives
     * rather than an image asset to keep it recolourable with the theme. */
    {
        const float lx = cx;
        const float ly = cy + (float)VAULT_LOGO_Y;
        /* Pre-blended and drawn OPAQUE. Stroking ring, hub and bars each at
         * 50% made every overlap composite twice and read as a darker seam;
         * mixing the colour up front gives one flat tone whatever overlaps. */
        const lv_color_t ink = c(lerp_rgb(boost_theme_vault_face(), theme->text, 0.5f));

        /* Outer ring of the vault door. */
        lv_draw_arc_dsc_init(&arc);
        arc.color = ink;
        arc.opa = LV_OPA_COVER;
        arc.width = VAULT_LOGO_RING_W;
        arc.radius = VAULT_LOGO_R;
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.center.x = (int32_t)lroundf(lx);
        arc.center.y = (int32_t)lroundf(ly);
        lv_draw_arc(&layer, &arc);

        /* Hub. */
        lv_draw_rect_dsc_t hub;
        lv_draw_rect_dsc_init(&hub);
        hub.bg_color = ink;
        hub.bg_opa = LV_OPA_COVER;
        hub.radius = LV_RADIUS_CIRCLE;
        const int32_t hr = VAULT_LOGO_HUB_R;
        lv_area_t ha = { (int32_t)lroundf(lx) - hr, (int32_t)lroundf(ly) - hr,
                         (int32_t)lroundf(lx) + hr, (int32_t)lroundf(ly) + hr };
        lv_draw_rect(&layer, &hub, &ha);

        /* Three cog bars each side; the middle one reaches furthest, which is
         * what gives the mark its silhouette. */
        static const int8_t bar_dy[3] = { -VAULT_LOGO_BAR_DY, 0, VAULT_LOGO_BAR_DY };
        static const int8_t bar_len[3] = { VAULT_LOGO_BAR_SHORT, VAULT_LOGO_BAR_LONG,
                                           VAULT_LOGO_BAR_SHORT };
        for (int side = 0; side < 2; ++side) {
            const float dir = side ? 1.0f : -1.0f;
            for (int i = 0; i < 3; ++i) {
                lv_draw_line_dsc_init(&ln);
                ln.color = ink;
                ln.opa = LV_OPA_COVER;
                ln.width = VAULT_LOGO_BAR_W;
                ln.round_start = true;
                ln.round_end = true;
                ln.p1.x = lx + dir * (float)VAULT_LOGO_BAR_IN;
                ln.p1.y = ly + (float)bar_dy[i];
                ln.p2.x = lx + dir * (float)bar_len[i];
                ln.p2.y = ly + (float)bar_dy[i];
                lv_draw_line(&layer, &ln);
            }
        }
    }

    lv_canvas_finish_layer(canvas, &layer);

    /* Vignette, applied straight to the pixels. Concentric arcs banded visibly;
     * a per-pixel pass reproduces the web's radial gradient exactly and costs
     * nothing after build. Scanlines are NOT baked here — they belong on top.
     *
     * The ramp is then dithered, and it has to be. The face is #02100a, whose
     * green sits at level 4 of 63 in RGB565, so darkening it toward black has
     * only four distinct values to land on — an exact gradient still resolves
     * to four flat rings. The web mirror looks smooth only because canvas is
     * 8-bit (green 16 -> 6). Ordered dithering trades spatial noise for tonal
     * resolution, which is the only way to get a smooth ramp out of a channel
     * this dark. The pattern is baked, so it never shimmers. */
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (db == NULL) return;

    /*
     * Serpentine error-diffused vignette.
     *
     * The face is #02100a, whose green sits at level 4 of 63 in RGB565, so the
     * whole ramp has about three output levels to work with. Ordered (Bayer)
     * dithering left faint rings because it quantises each pixel independently.
     * Error diffusion carries the rounding forward so the local average tracks
     * the ideal ramp continuously.
     *
     * Plain left-to-right Floyd-Steinberg pushes its error consistently one way,
     * which builds up diagonal "worm" trails - the pixelated texture. Scanning
     * alternate rows in opposite directions (boustrophedon) and mirroring the
     * kernel cancels that bias, so the residual reads as fine even grain rather
     * than a directional pattern. On a channel this coarse some grain is the
     * unavoidable price of not having rings; this makes it the least structured
     * grain available.
     *
     * A serial pass with two error rows, far too slow per frame - it runs once,
     * at scene build, into the cached face.
     */
    const float fcx = (float)(DISP_SIZE - 1) * 0.5f;
    const float fcy = (float)(DISP_SIZE - 1) * 0.5f;
    const float span = VAULT_VIGN_R1 - VAULT_VIGN_R0;
    const float vign_max = (float)boost_theme_vault_vignette_pct() / 100.0f;

    /* +1 pad each side so x-1 and x+1 index in bounds at both ends. */
    const size_t errbytes = sizeof(int16_t) * 3u * (size_t)(DISP_SIZE + 2);
    int16_t *err_cur = BG_ALLOC(errbytes);
    int16_t *err_nxt = BG_ALLOC(errbytes);
    if (err_cur == NULL || err_nxt == NULL) {
        BG_FREE(err_cur);
        BG_FREE(err_nxt);
        return; /* face is still correct, just undithered */
    }
    memset(err_cur, 0, errbytes);
    memset(err_nxt, 0, errbytes);

    for (int32_t y = 0; y < DISP_SIZE; ++y) {
        lv_color16_t *row = (lv_color16_t *)(db->data + (size_t)y * db->header.stride);
        const float dy = (float)y - fcy;
        const float dy2 = dy * dy;
        memset(err_nxt, 0, errbytes);

        /* Even rows L->R, odd rows R->L; the forward neighbour and the two
         * diagonal next-row weights flip with the direction. */
        const int dir = (y & 1) ? -1 : 1;
        const int32_t x_start = (dir > 0) ? 0 : DISP_SIZE - 1;
        const int32_t x_end = (dir > 0) ? DISP_SIZE : -1;

        for (int32_t x = x_start; x != x_end; x += dir) {
            const float dx = (float)x - fcx;
            const float r = sqrtf(dx * dx + dy2);

            float a = 0.0f;
            if (r > VAULT_VIGN_R0) {
                float t = (r - VAULT_VIGN_R0) / span;
                if (t > 1.0f) t = 1.0f;
                /* Smootherstep: zero first AND second derivative at both ends,
                 * so neither the onset nor the clamp leaves a visible edge. */
                a = vign_max * t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
            }
            const float keep = 1.0f - a;

            const uint16_t src[3] = { row[x].red, row[x].green, row[x].blue };
            const uint16_t maxv[3] = { 31u, 63u, 31u };
            uint16_t out[3];

            for (int ch = 0; ch < 3; ++ch) {
                const int32_t ideal = (int32_t)lroundf((float)src[ch] * keep * 256.0f);
                int32_t want = ideal + err_cur[(x + 1) * 3 + ch];
                int32_t q = (want + 128) >> 8;
                if (q < 0) q = 0;
                if (q > (int32_t)maxv[ch]) q = (int32_t)maxv[ch];
                out[ch] = (uint16_t)q;

                int32_t e = want - (q << 8);
                /* Clamp so a saturated channel cannot pump unbounded error into
                 * its neighbours and streak. */
                if (e > 512) e = 512;
                if (e < -512) e = -512;
                /* Weights follow the scan: fwd = x+dir, diagonals mirror. */
                err_cur[(x + 1 + dir) * 3 + ch] += (int16_t)((e * 7) / 16);
                err_nxt[(x + 1 - dir) * 3 + ch] += (int16_t)((e * 3) / 16);
                err_nxt[(x + 1) * 3 + ch]       += (int16_t)((e * 5) / 16);
                err_nxt[(x + 1 + dir) * 3 + ch] += (int16_t)((e * 1) / 16);
            }
            row[x].red = out[0];
            row[x].green = out[1];
            row[x].blue = out[2];
        }
        int16_t *swap = err_cur;
        err_cur = err_nxt;
        err_nxt = swap;
    }
    BG_FREE(err_cur);
    BG_FREE(err_nxt);
}

/* CRT scanlines, drawn last so they cross the needle and the digits the way
 * the web canvas does. Phase comes from absolute screen y, so neighbouring
 * dirty regions can never disagree and produce the banding seen when this was
 * drawn per-region. Chord-clipped to the face, and clipped again to the dirty
 * area so a needle-sized repaint only pays for its own rows. */
static void draw_vault_crt(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    const lv_area_t *clip = &layer->_clip_area;
    const float cx = (float)(DISP_SIZE - 1) * 0.5f + (float)s_px_dx;
    const float cy = (float)(DISP_SIZE - 1) * 0.5f + (float)s_px_dy;

    /* lv_draw_fill(), not lv_draw_rect(): a scanline is a flat 1 px bar with no
     * radius, border, outline, shadow or gradient, and lv_draw_rect() would run
     * all six of those tests and then emit exactly this fill task anyway. One
     * task per row is already the dominant cost here (a 100 px row is ~40 ns of
     * blending against a task allocation, a unit evaluate and a dispatch), so
     * the cheaper entry point is worth taking. Byte-for-byte the same task. */
    lv_draw_fill_dsc_t sl;
    lv_draw_fill_dsc_init(&sl);
    sl.color = lv_color_black();
    sl.opa = VAULT_SCAN_OPA;

    int32_t y0 = clip->y1 < 0 ? 0 : clip->y1;
    int32_t y1 = clip->y2 > DISP_SIZE - 1 ? DISP_SIZE - 1 : clip->y2;
    /* Phase follows the burn-in offset so the scanlines travel with the face
     * rather than staying pinned to absolute rows — otherwise the three rows
     * between scanlines would carry full duty forever. It is still a single
     * global phase, so neighbouring dirty regions cannot disagree and reproduce
     * the banding this had when the phase was per-region. */
    const int32_t phase = ((s_px_dy % VAULT_SCAN_STEP) + VAULT_SCAN_STEP) % VAULT_SCAN_STEP;
    y0 += (((phase - y0) % VAULT_SCAN_STEP) + VAULT_SCAN_STEP) % VAULT_SCAN_STEP;

    for (int32_t y = y0; y <= y1; y += VAULT_SCAN_STEP) {
        const float dy = (float)y - cy;
        const float h2 = VAULT_FACE_R * VAULT_FACE_R - dy * dy;
        if (h2 <= 0.0f) continue;
        const float hx = sqrtf(h2);
        int32_t xa = (int32_t)(cx - hx);
        int32_t xb = (int32_t)(cx + hx);
        if (xa < clip->x1) xa = clip->x1;
        if (xb > clip->x2) xb = clip->x2;
        if (xb < xa) continue;
        lv_area_t a = { xa, y, xb, y };
        lv_draw_fill(layer, &sl, &a);
    }
}

/* Peak tell-tale rides in its own small object so it can move without
 * disturbing the cached background. */
static void draw_vault_peak_mark(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const float mx = (float)(a.x1 + a.x2) * 0.5f;
    const float my = (float)(a.y1 + a.y2) * 0.5f;
    const float rad = s_vault_peak_deg * (float)M_PI / 180.0f;
    const float nx = cosf(rad);
    const float ny = sinf(rad);

    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = c(theme->overboost);
    /* Knocked back against the near-black face so it reads as a marker rather
     * than competing with the overboost ticks. */
    t.opa = LV_OPA_60;
    t.p[0].x = mx - 8.0f * nx;
    t.p[0].y = my - 8.0f * ny;
    t.p[1].x = mx + 8.0f * nx - 7.5f * ny;
    t.p[1].y = my + 8.0f * ny + 7.5f * nx;
    t.p[2].x = mx + 8.0f * nx + 7.5f * ny;
    t.p[2].y = my + 8.0f * ny - 7.5f * nx;
    lv_draw_triangle(layer, &t);
}

static void draw_vault_needle(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const float cx = px_cx();
    const float cy = px_cy();
    const float rad = s_vault_needle_deg * (float)M_PI / 180.0f;

    const lv_color_t needle_col =
        s_vault_needle_over ? c(theme->overboost) : c(theme->text);
    const float nx = cosf(rad);
    const float ny = sinf(rad);
    const float px = -ny; /* perpendicular */
    const float py = nx;
    const float hw = (float)VAULT_NEEDLE_HALFW;

    /* Tapered wedge: wide at the hub, a point at the tip. Two triangles form
     * the quad, and triangles are cheaper than a wide rounded line. */
    const float bx = cx - (float)VAULT_NEEDLE_TAIL * nx;
    const float by = cy - (float)VAULT_NEEDLE_TAIL * ny;
    const float tx = cx + (float)VAULT_NEEDLE_LEN * nx;
    const float ty = cy + (float)VAULT_NEEDLE_LEN * ny;

    /* Trapezoid: wide at the hub, blunt at the tip (a spike read as too sharp). */
    const float tipw = 2.5f;
    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = needle_col;
    tri.opa = LV_OPA_COVER;
    tri.p[0].x = bx + hw * px;   tri.p[0].y = by + hw * py;
    tri.p[1].x = bx - hw * px;   tri.p[1].y = by - hw * py;
    tri.p[2].x = tx - tipw * px; tri.p[2].y = ty - tipw * py;
    lv_draw_triangle(layer, &tri);
    tri.p[0].x = bx + hw * px;   tri.p[0].y = by + hw * py;
    tri.p[1].x = tx + tipw * px; tri.p[1].y = ty + tipw * py;
    tri.p[2].x = tx - tipw * px; tri.p[2].y = ty - tipw * py;
    lv_draw_triangle(layer, &tri);
    tri.p[0].x = bx + hw * px;   tri.p[0].y = by + hw * py;
    tri.p[1].x = bx - hw * px;   tri.p[1].y = by - hw * py;
    tri.p[2].x = tx + tipw * px; tri.p[2].y = ty + tipw * py;
    lv_draw_triangle(layer, &tri);

    /* Hub: dark centre with a ring, so the pivot reads as a cap. */
    lv_draw_rect_dsc_t hub;
    lv_draw_rect_dsc_init(&hub);
    hub.bg_color = c(boost_theme_vault_face());
    hub.bg_opa = LV_OPA_COVER;
    hub.radius = LV_RADIUS_CIRCLE;
    /* The pivot cap keeps the phosphor green even in overboost - only the
     * wedge changes colour, so the hub reads as part of the dial, not the
     * reading. */
    hub.border_color = c(theme->text);
    hub.border_width = 3;
    hub.border_opa = LV_OPA_COVER;
    lv_area_t hub_area = {
        .x1 = px_icx() - VAULT_HUB_R,
        .y1 = px_icy() - VAULT_HUB_R,
        .x2 = px_icx() + VAULT_HUB_R,
        .y2 = px_icy() + VAULT_HUB_R,
    };
    lv_draw_rect(layer, &hub, &hub_area);
}

static void vault_inv_box(float minx, float miny, float maxx, float maxy, float pad)
{
    lv_area_t a;
    a.x1 = (lv_coord_t)floorf(minx - pad);
    a.y1 = (lv_coord_t)floorf(miny - pad);
    a.x2 = (lv_coord_t)ceilf(maxx + pad);
    a.y2 = (lv_coord_t)ceilf(maxy + pad);
    lv_obj_invalidate_area(s_vault_needle, &a);
}

/* One box per sweep, spanning both ends of the needle at every sampled angle.
 * Kept for the rare large jump and as the VAULT_NEEDLE_SEGS=1 fallback.
 * `samples` > 2 walks the arc as well as its ends, which a sweep crossing an
 * axis needs - the bbox of just the two end positions falls inside the arc. */
static void invalidate_vault_needle_fan(float old_deg, float new_deg, int samples)
{
    const float cx = px_cx();
    const float cy = px_cy();
    float minx = cx, maxx = cx, miny = cy, maxy = cy;
    for (int i = 0; i < samples; ++i) {
        const float f = samples < 2 ? 0.0f : (float)i / (float)(samples - 1);
        const float rad = (old_deg + (new_deg - old_deg) * f) * (float)M_PI / 180.0f;
        const float ct = cosf(rad), st = sinf(rad);
        /* Both ends: the counterweight tail reaches VAULT_NEEDLE_TAIL past the
         * pivot, which is further than the hub-sized pad below allows for. */
        const float rs[2] = { -(float)VAULT_NEEDLE_TAIL, (float)VAULT_NEEDLE_LEN };
        for (int k = 0; k < 2; ++k) {
            const float x = cx + rs[k] * ct;
            const float y = cy + rs[k] * st;
            if (x < minx) minx = x;
            if (x > maxx) maxx = x;
            if (y < miny) miny = y;
            if (y > maxy) maxy = y;
        }
    }
    /* Hub is at the centre and already inside the box; the ends need only the
     * half-width plus an AA margin. Keeping this tight is what lets the ring
     * art skip. */
    vault_inv_box(minx, miny, maxx, maxy, (float)(VAULT_HUB_R + 5 + 3));
}

static void invalidate_vault_needle(float old_deg, float new_deg)
{
    if (s_vault_needle == NULL) return;

#if VAULT_NEEDLE_SEGS < 2
    invalidate_vault_needle_fan(old_deg, new_deg, 2);
#else
    /* Past a point the chain would crowd LV_INV_BUF_SIZE and the boxes would
     * overlap so heavily that one fanned box is smaller anyway. */
    const float sweep = fabsf(new_deg - old_deg);
    if (sweep > 45.0f) {
        /* One sample per 12 degrees, so the arc is covered, not just its ends. */
        int samples = (int)(sweep / 12.0f) + 2;
        if (samples > 24) samples = 24;
        invalidate_vault_needle_fan(old_deg, new_deg, samples);
        return;
    }

    const float cx = px_cx();
    const float cy = px_cy();

    /* Shaft: one box per radial slice, spanning both angles. The needle is at
     * most VAULT_NEEDLE_HALFW wide perpendicular to the spoke, and a
     * perpendicular offset can never exceed that on either axis, so a flat pad
     * of half-width plus an AA pixel covers the ink itself. */
    const float c0 = cosf(old_deg * (float)M_PI / 180.0f);
    const float s0 = sinf(old_deg * (float)M_PI / 180.0f);
    const float c1 = cosf(new_deg * (float)M_PI / 180.0f);
    const float s1 = sinf(new_deg * (float)M_PI / 180.0f);
    const float r_lo = -(float)VAULT_NEEDLE_TAIL;
    const float r_hi = (float)VAULT_NEEDLE_LEN;
    /* A slice box is built from its four corners, so the arc swept between the
     * two angles bulges outside it by r * (1 - cos(sweep/2)). Charge that to
     * the padding rather than capping the sweep: at the 0.35 degree gate it is
     * a millionth of a pixel, so the common frame pays nothing for it. */
    const float bulge = 1.0f - cosf(sweep * 0.5f * (float)M_PI / 180.0f);
    const float pad_base = (float)VAULT_NEEDLE_HALFW + 2.0f;
    /* The pivot cap is a rounded rect filling exactly VAULT_HUB_R either side
     * of the centre, plus a pixel of AA on the circle. It straddles the pivot,
     * so it folds into the innermost slice rather than costing a box of its
     * own - an extra box would add its full height back to the scanline and
     * triangle row counts, which is most of what those two cost. */
    const float hub = (float)VAULT_HUB_R + 2.0f - pad_base;
    for (int i = 0; i < VAULT_NEEDLE_SEGS; ++i) {
        const float ra = r_lo + (r_hi - r_lo) * ((float)i / (float)VAULT_NEEDLE_SEGS);
        const float rb = r_lo + (r_hi - r_lo) * ((float)(i + 1) / (float)VAULT_NEEDLE_SEGS);
        const float xs[4] = { cx + ra * c0, cx + rb * c0, cx + ra * c1, cx + rb * c1 };
        const float ys[4] = { cy + ra * s0, cy + rb * s0, cy + ra * s1, cy + rb * s1 };
        float minx = xs[0], maxx = xs[0], miny = ys[0], maxy = ys[0];
        for (int k = 1; k < 4; ++k) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
        if (i == 0) {
            /* r_lo is negative, so slice 0 already straddles the pivot. */
            if (cx - hub < minx) minx = cx - hub;
            if (cx + hub > maxx) maxx = cx + hub;
            if (cy - hub < miny) miny = cy - hub;
            if (cy + hub > maxy) maxy = cy + hub;
        }
        const float rmax = (ra < 0.0f ? -ra : ra) > rb ? (ra < 0.0f ? -ra : ra) : rb;
        vault_inv_box(minx, miny, maxx, maxy, pad_base + rmax * bulge);
    }
#endif
}

/* Hazard triangles flanking the over-pressure warning. */
static void draw_vault_alert_marks(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const float cx = px_cx();
    const float cy = px_cy() + 72.0f;
    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    t.color = c(theme->overboost);
    t.opa = LV_OPA_COVER;
    const float half = 9.0f;
    const float h = 15.0f;
    for (int side = 0; side < 2; ++side) {
        /* Just clear of the 157 px label at F_COND22 (half 78 + triangle 9 + gap). */
        const float x = cx + (side ? 96.0f : -96.0f);
        t.p[0].x = x;        t.p[0].y = cy - h * 0.5f;
        t.p[1].x = x - half; t.p[1].y = cy + h * 0.5f;
        t.p[2].x = x + half; t.p[2].y = cy + h * 0.5f;
        lv_draw_triangle(layer, &t);
    }
}

static void build_vault(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    /* Static face is rendered once into PSRAM and then only blitted. */
    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_vault_bg_buf = BG_ALLOC(bg_bytes);
    if (s_vault_bg_buf != NULL) {
        s_vault_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_vault_bg, s_vault_bg_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_vault_bg);
        lv_obj_clear_flag(s_vault_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        paint_vault_background(s_vault_bg, theme);
        /* The cached face is exactly screen-sized, so the burn-in shift slides
         * a margin of up to two pixels off one edge and exposes the screen's
         * own background at the other. By the time the face reaches its rim the
         * vignette has taken it down to (1 - VAULT_VIGN_MAX) of the face
         * colour, so match THAT, not the raw face — otherwise the margin reads
         * as a bright hairline tracing one side of the glass. The other three
         * styles need no such trick: hud's cached face and bigdigit's ground
         * are flat fills that already equal the screen background. */
        lv_obj_set_style_bg_color(lv_screen_active(),
                                  c(scale_rgb(boost_theme_vault_face(),
                                              1.0f - (float)boost_theme_vault_vignette_pct() / 100.0f)), 0);
    } else {
        ESP_LOGW(TAG, "vault background cache alloc failed (%u B)", (unsigned)bg_bytes);
    }

    s_vault_peak_mark = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_peak_mark);
    lv_obj_set_size(s_vault_peak_mark, 34, 34);
    lv_obj_clear_flag(s_vault_peak_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vault_peak_mark, draw_vault_peak_mark, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(s_vault_peak_mark, LV_OBJ_FLAG_HIDDEN);
    s_vault_peak_deg = psi_to_sweep(0.0f, VAULT_A0, VAULT_A1);

    /* dial numerals */
    const float mid = s_psi_max * 0.5f;
    const bool mid_clear = fabsf(mid - s_psi_overboost) > (s_psi_max - s_psi_min) * 0.08f;
    const float marks[5] = { s_psi_min, 0.0f, mid, s_psi_overboost, s_psi_max };
    for (int i = 0; i < 5; ++i) {
        if (i == 2 && !mid_clear) continue;
        lv_obj_t *lab = lv_label_create(scr);
        char txt[12];
        format_tick_text(txt, sizeof(txt), marks[i]);
        lv_label_set_text(lab, txt);
        lv_obj_set_style_text_font(lab, F_COND22, 0);
        lv_obj_set_style_text_color(lab, marks[i] >= s_psi_overboost ? c(theme->overboost) : c(theme->text), 0);
        lv_obj_update_layout(lab);
        const float rad = psi_to_sweep(marks[i], VAULT_A0, VAULT_A1) * (float)M_PI / 180.0f;
        const float x = DISP_SIZE * 0.5f + VAULT_NUM_R * cosf(rad) - lv_obj_get_width(lab) * 0.5f;
        const float y = DISP_SIZE * 0.5f + VAULT_NUM_R * sinf(rad) - lv_obj_get_height(lab) * 0.5f;
        lv_obj_set_pos(lab, (lv_coord_t)lroundf(x), (lv_coord_t)lroundf(y));
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "VAULT-TEC");
    lv_obj_set_style_text_font(title, F_COND22, 0);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    lv_obj_set_style_text_color(title, c(theme->text), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -114);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "BOOST-O-METER");
    lv_obj_set_style_text_font(sub, F_COND14, 0);
    lv_obj_set_style_text_letter_space(sub, 1, 0);
    lv_obj_set_style_text_color(sub, c(theme->muted), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, -94);

    s_vault_alert = lv_label_create(scr);
    lv_label_set_text(s_vault_alert, "OVER-PRESSURE");
    lv_obj_set_style_text_font(s_vault_alert, F_COND22, 0);
    lv_obj_set_style_text_letter_space(s_vault_alert, 1, 0);
    lv_obj_set_style_text_color(s_vault_alert, c(theme->overboost), 0);
    lv_obj_align(s_vault_alert, LV_ALIGN_CENTER, 0, 72);
    lv_obj_add_flag(s_vault_alert, LV_OBJ_FLAG_HIDDEN);

    s_vault_alert_marks = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_alert_marks);
    lv_obj_set_size(s_vault_alert_marks, 230, 30);
    lv_obj_align(s_vault_alert_marks, LV_ALIGN_CENTER, 0, 72);
    lv_obj_clear_flag(s_vault_alert_marks, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vault_alert_marks, draw_vault_alert_marks, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_flag(s_vault_alert_marks, LV_OBJ_FLAG_HIDDEN);

    s_vault_window = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_window);
    /* 59, not 60: centred alignment floors the top edge, so dropping one row
     * takes it off the top only and evens the 13/12 px ink margins. */
    lv_obj_set_size(s_vault_window, 190, 59);
    lv_obj_align(s_vault_window, LV_ALIGN_CENTER, 0, 126);
    lv_obj_set_style_border_width(s_vault_window, 2, 0);
    lv_obj_set_style_border_color(s_vault_window, c(theme->muted), 0);
    lv_obj_set_style_radius(s_vault_window, 5, 0);
    lv_obj_clear_flag(s_vault_window, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* Fixed slots so digits never slide left/right as the value changes. */
    for (int i = 0; i < VAULT_SLOT_COUNT; ++i) {
        s_vault_slot[i] = lv_label_create(scr);
        lv_label_set_text(s_vault_slot[i], i == VAULT_SLOT_DOT ? "." : "");
        lv_obj_set_style_text_font(s_vault_slot[i], F_MONO40, 0);
        lv_obj_set_style_text_color(s_vault_slot[i], c(theme->text), 0);
        lv_obj_set_size(s_vault_slot[i], 26, 34);
        lv_obj_set_style_text_align(s_vault_slot[i], LV_TEXT_ALIGN_CENTER, 0);
        /* Nudged below the window centre: the 40 px mono face carries more ascent
         * than descent, so a box-centred label reads a few pixels high. */
        lv_obj_align(s_vault_slot[i], LV_ALIGN_CENTER, k_vault_slot_x[i], 130);
        lv_obj_clear_flag(s_vault_slot[i], LV_OBJ_FLAG_CLICKABLE);
    }

    /* Restored: this label was dropped in the slot rework. */
    lv_obj_t *manifold = lv_label_create(scr);
    lv_label_set_text(manifold, "MANIFOLD  PSI");
    lv_obj_set_style_text_font(manifold, F_COND14, 0);
    lv_obj_set_style_text_color(manifold, c(theme->muted), 0);
    lv_obj_set_style_text_letter_space(manifold, 2, 0);
    lv_obj_align(manifold, LV_ALIGN_CENTER, 0, 178);

    s_vault_peak = lv_label_create(scr);
    lv_label_set_text(s_vault_peak, "PEAK  0.0");
    lv_obj_set_style_text_font(s_vault_peak, F_MONO16, 0);
    lv_obj_set_style_text_color(s_vault_peak, c(theme->overboost), 0);
    lv_obj_align(s_vault_peak, LV_ALIGN_CENTER, 0, 202);

    s_vault_needle = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_needle);
    lv_obj_set_size(s_vault_needle, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_vault_needle);
    lv_obj_clear_flag(s_vault_needle, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vault_needle, draw_vault_needle, LV_EVENT_DRAW_MAIN, NULL);
    s_vault_needle_deg = psi_to_sweep(0.0f, VAULT_A0, VAULT_A1);

    /* Created last so it draws over everything, matching the web's ordering. */
    s_vault_crt = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_crt);
    lv_obj_set_size(s_vault_crt, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_vault_crt);
    lv_obj_clear_flag(s_vault_crt, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vault_crt, draw_vault_crt, LV_EVENT_DRAW_MAIN, NULL);
}

static void update_vault(const boost_sample_t *sample, const boost_theme_t *theme)
{
    const float deg = psi_to_sweep(sample->psi, VAULT_A0, VAULT_A1);
    const bool needle_over = sample->psi >= s_psi_overboost;
    /* Sub-degree jitter isn't visible but costs a full needle-sized repaint. */
    if (fabsf(deg - s_vault_needle_deg) > 0.35f || needle_over != s_vault_needle_over) {
        const float old = s_vault_needle_deg;
        s_vault_needle_deg = deg;
        s_vault_needle_over = needle_over;
        invalidate_vault_needle(old, deg);
    }

    /* Peak tell-tale: reposition its little object instead of repainting the
     * cached background. */
    if (s_vault_peak_mark != NULL) {
        if (s_peak_psi >= 0.35f) {
            const float pdeg = psi_to_sweep(s_peak_psi, VAULT_A0, VAULT_A1);
            if (fabsf(pdeg - s_vault_peak_deg) > 0.2f ||
                lv_obj_has_flag(s_vault_peak_mark, LV_OBJ_FLAG_HIDDEN)) {
                s_vault_peak_deg = pdeg;
                const float rad = pdeg * (float)M_PI / 180.0f;
                lv_obj_align(s_vault_peak_mark, LV_ALIGN_CENTER,
                             (lv_coord_t)lroundf(VAULT_PEAK_R * cosf(rad)),
                             (lv_coord_t)lroundf(VAULT_PEAK_R * sinf(rad)));
                lv_obj_remove_flag(s_vault_peak_mark, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (!lv_obj_has_flag(s_vault_peak_mark, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_vault_peak_mark, LV_OBJ_FLAG_HIDDEN);
        }
    }

    const bool over = sample->psi >= s_psi_overboost;

    /* Per-slot digits: each stays put, only its glyph changes. */
    const int hundredths_total = (int)lroundf(fabsf(sample->psi) * 100.0f);
    const int whole = hundredths_total / 100;
    const int frac = hundredths_total % 100;
    char slot_txt[VAULT_SLOT_COUNT][2] = {
        { sample->psi < -0.005f ? '-' : '+', 0 },
        { (char)('0' + (whole / 10) % 10), 0 },
        { (char)('0' + whole % 10), 0 },
        { '.', 0 },
        { (char)('0' + frac / 10), 0 },
        { (char)('0' + frac % 10), 0 },
    };
    const lv_color_t vc = over ? c(theme->overboost) : c(theme->text);
    for (int i = 0; i < VAULT_SLOT_COUNT; ++i) {
        if (strcmp(lv_label_get_text(s_vault_slot[i]), slot_txt[i]) != 0) {
            lv_label_set_text(s_vault_slot[i], slot_txt[i]);
        }
        if (!lv_color_eq(lv_obj_get_style_text_color(s_vault_slot[i], 0), vc)) {
            lv_obj_set_style_text_color(s_vault_slot[i], vc, 0);
        }
    }
    if (!lv_color_eq(lv_obj_get_style_border_color(s_vault_window, 0),
                     over ? c(theme->overboost) : c(theme->muted))) {
        lv_obj_set_style_border_color(s_vault_window, over ? c(theme->overboost) : c(theme->muted), 0);
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "PEAK  %.1f", (double)s_peak_psi);
    if (strcmp(lv_label_get_text(s_vault_peak), buf) != 0) lv_label_set_text(s_vault_peak, buf);

    /* Steady popup while in overboost: blinking meant a repaint every 320 ms. */
    const bool show = over;
    const bool hidden = lv_obj_has_flag(s_vault_alert, LV_OBJ_FLAG_HIDDEN);
    if (show && hidden) {
        lv_obj_remove_flag(s_vault_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_vault_alert_marks, LV_OBJ_FLAG_HIDDEN);
    } else if (!show && !hidden) {
        lv_obj_add_flag(s_vault_alert, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_vault_alert_marks, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ========================================================================== */
/*  Style: hud  (Night City targeting HUD)                                    */
/* ========================================================================== */

/* Static face art. `cached` is set when painting into the background canvas at
 * build time: the clip early-out is meaningless there and everything must be
 * drawn once, in full.
 *
 * The centre is a parameter because the two callers work in different spaces.
 * Cached, it is the canvas's own centre and carries no burn-in offset — the
 * canvas is a bitmap that gets MOVED, and re-rasterising it on every shift
 * would throw away the whole point of caching it. Live, it is the shifted
 * screen centre. */
static void paint_hud_face(lv_layer_t *layer, const boost_theme_t *theme, bool cached,
                           float cx, float cy)
{
    const int32_t icx = (int32_t)lroundf(cx);
    const int32_t icy = (int32_t)lroundf(cy);
    lv_draw_line_dsc_t ln;

    /* hazard chevrons */
    for (int i = -3; i <= 3; ++i) {
        const float x = cx + (float)i * 30.0f;
        lv_draw_line_dsc_init(&ln);
        ln.color = (i % 2) ? c(theme->boost) : c(theme->track);
        ln.width = 4;
        ln.opa = LV_OPA_COVER;
        ln.p1.x = x - 10.0f;
        ln.p1.y = cy - 142.0f;
        ln.p2.x = x;
        ln.p2.y = cy - 130.0f;
        lv_draw_line(layer, &ln);
        ln.p1.x = x;
        ln.p1.y = cy - 130.0f;
        ln.p2.x = x + 10.0f;
        ln.p2.y = cy - 142.0f;
        lv_draw_line(layer, &ln);
    }

    /* Header arrows, drawn as shapes: glyph escapes proved fragile and this
     * gives exact size and placement. */
    {
        lv_draw_triangle_dsc_t ar;
        lv_draw_triangle_dsc_init(&ar);
        ar.color = c(theme->vacuum);
        ar.opa = LV_OPA_COVER;
        const float ay = cy - 108.0f;
        const float ah = 7.0f;
        const float aw = 9.0f;
        const float lx = cx - 92.0f;
        ar.p[0].x = lx - aw;
        ar.p[0].y = ay;
        ar.p[1].x = lx;
        ar.p[1].y = ay - ah;
        ar.p[2].x = lx;
        ar.p[2].y = ay + ah;
        lv_draw_triangle(layer, &ar);
        const float rx = cx + 92.0f;
        ar.p[0].x = rx + aw;
        ar.p[0].y = ay;
        ar.p[1].x = rx;
        ar.p[1].y = ay - ah;
        ar.p[2].x = rx;
        ar.p[2].y = ay + ah;
        lv_draw_triangle(layer, &ar);
    }

    /* Kiroshi reticle brackets */
    const int bx[4] = { -HUD_BRACKET_X, HUD_BRACKET_X, -HUD_BRACKET_X, HUD_BRACKET_X };
    const int by[4] = { -40, -40, 28, 28 };
    const int dy[4] = { -18, -18, 18, 18 };
    const int dx[4] = { 22, -22, 22, -22 };
    for (int i = 0; i < 4; ++i) {
        lv_draw_line_dsc_init(&ln);
        ln.color = c(theme->vacuum);
        ln.width = 3;
        ln.opa = LV_OPA_COVER;
        ln.p1.x = cx + bx[i];
        ln.p1.y = cy + by[i];
        ln.p2.x = cx + bx[i];
        ln.p2.y = cy + by[i] + dy[i];
        lv_draw_line(layer, &ln);
        ln.p1.x = cx + bx[i];
        ln.p1.y = cy + by[i] + dy[i];
        ln.p2.x = cx + bx[i] + dx[i];
        ln.p2.y = cy + by[i] + dy[i];
        lv_draw_line(layer, &ln);
    }

    /* Outer ring art (ticks, track, notch) lives at r >= 196. A digit update
     * dirties only the centre, so skip all of it in that common case. */
    if (!cached && !clip_reaches_radius(layer, cx, cy, 190.0f)) return;

    for (int i = 0; i <= 20; ++i) {
        const float v = s_psi_min + (s_psi_max - s_psi_min) * (float)i / 20.0f;
        const float rad = psi_to_sweep(v, HUD_A0, HUD_A1) * (float)M_PI / 180.0f;
        lv_draw_line_dsc_init(&ln);
        ln.color = (v >= s_psi_overboost) ? c(theme->overboost) : c(theme->vacuum);
        ln.width = (i % 2 == 0) ? 4 : 2;
        ln.opa = LV_OPA_COVER;
        ln.p1.x = cx + 198.0f * cosf(rad);
        ln.p1.y = cy + 198.0f * sinf(rad);
        ln.p2.x = cx + 214.0f * cosf(rad);
        ln.p2.y = cy + 214.0f * sinf(rad);
        lv_draw_line(layer, &ln);
    }

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = c(theme->track);
    arc.width = 10;
    arc.start_angle = HUD_A0;
    arc.end_angle = HUD_A1;
    arc.center.x = icx;
    arc.center.y = icy;
    arc.radius = 206;
    arc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &arc);

    const float zrad = psi_to_sweep(0.0f, HUD_A0, HUD_A1) * (float)M_PI / 180.0f;
    lv_draw_line_dsc_init(&ln);
    ln.color = c(theme->zero);
    ln.width = 5;
    ln.opa = LV_OPA_COVER;
    ln.round_start = true;
    ln.round_end = true;
    ln.p1.x = cx + 196.0f * cosf(zrad);
    ln.p1.y = cy + 196.0f * sinf(zrad);
    ln.p2.x = cx + 217.0f * cosf(zrad);
    ln.p2.y = cy + 217.0f * sinf(zrad);
    lv_draw_line(layer, &ln);
}

static void draw_hud_face(lv_event_t *e)
{
    paint_hud_face(lv_event_get_layer(e), active_theme(), false, px_cx(), px_cy());
}

/* Tech-noir glitch: on a fast spike, ghost the value in red/cyan either side of
 * the real digits for a beat. Drawn directly, so it needs no extra objects. */
static void draw_hud_glitch(lv_event_t *e)
{
    if (s_hud_val_str[0] == '\0') return;
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);

    lv_area_t area;
    area.x1 = px_icx() - 156;
    area.x2 = px_icx() + 105;
    area.y1 = px_icy() + HUD_VALUE_Y - 39;
    area.y2 = px_icy() + HUD_VALUE_Y + 39;

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = HUD_VALUE_FONT;
    d.text = s_hud_val_str;
    d.align = LV_TEXT_ALIGN_RIGHT;
    d.opa = LV_OPA_70;

    lv_area_t a1 = area;
    a1.x1 -= HUD_GLITCH_DX;
    a1.x2 -= HUD_GLITCH_DX;
    d.color = c(theme->overboost);
    lv_draw_label(layer, &d, &a1);

    lv_area_t a2 = area;
    a2.x1 += HUD_GLITCH_DX;
    a2.x2 += HUD_GLITCH_DX;
    d.color = c(theme->vacuum);
    lv_draw_label(layer, &d, &a2);
}

static void draw_hud_fill(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const float zero_a = psi_to_sweep(0.0f, HUD_A0, HUD_A1);
    const float val_a = s_hud_fill_valid ? s_hud_fill_deg : zero_a;
    const float lo = fminf(zero_a, val_a);
    const float hi = fmaxf(zero_a, val_a);
    if (hi - lo < 0.5f) return;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    const float drawn_psi = s_hud_fill_valid ? s_hud_fill_psi : 0.0f;
    if (boost_theme_hud_gradient()) arc.color = c(gradient_rgb_for_psi(theme, drawn_psi));
    else if (drawn_psi >= s_psi_overboost) arc.color = c(theme->overboost);
    else if (drawn_psi < 0.0f) arc.color = c(theme->vacuum);
    else arc.color = c(theme->boost);
    arc.width = 10;
    arc.start_angle = lo;
    arc.end_angle = hi;
    arc.center.x = px_icx();
    arc.center.y = px_icy();
    arc.radius = 206;
    arc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &arc);
}

static void invalidate_hud_fill(float a, float b)
{
    if (s_hud_fill == NULL) return;
    const float lo = fminf(a, b);
    const float hi = fmaxf(a, b);
    for (float seg = lo; seg < hi;) {
        const float boundary = (floorf(seg / 90.0f) + 1.0f) * 90.0f;
        const float seg_end = fminf(hi, boundary);
        lv_area_t area;
        lv_draw_arc_get_area(px_icx(), px_icy(), 206, seg, seg_end, 10, true, &area);
        lv_obj_invalidate_area(s_hud_fill, &area);
        seg = seg_end;
    }
}

static void build_hud(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    /* Chevrons, arrows, reticle brackets, tick ring, track and zero notch are
     * all static: rasterise once into PSRAM and blit thereafter, the same win
     * the vault face got. Only the fill arc, digits and glitch stay live. */
    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_hud_bg_buf = BG_ALLOC(bg_bytes);
    if (s_hud_bg_buf != NULL) {
        s_hud_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_hud_bg, s_hud_bg_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_hud_bg);
        lv_obj_clear_flag(s_hud_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_layer_t layer;
        lv_canvas_init_layer(s_hud_bg, &layer);
        lv_draw_rect_dsc_t bg;
        lv_draw_rect_dsc_init(&bg);
        bg.bg_color = c(theme->face);
        bg.bg_opa = LV_OPA_COVER;
        lv_area_t full = { 0, 0, DISP_SIZE - 1, DISP_SIZE - 1 };
        lv_draw_rect(&layer, &bg, &full);
        paint_hud_face(&layer, theme, true, DISP_SIZE * 0.5f, DISP_SIZE * 0.5f);
        lv_canvas_finish_layer(s_hud_bg, &layer);
    } else {
        ESP_LOGW(TAG, "hud background cache alloc failed (%u B)", (unsigned)bg_bytes);
        s_hud_face = lv_obj_create(scr);
        lv_obj_remove_style_all(s_hud_face);
        lv_obj_set_size(s_hud_face, DISP_SIZE, DISP_SIZE);
        lv_obj_center(s_hud_face);
        lv_obj_clear_flag(s_hud_face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s_hud_face, draw_hud_face, LV_EVENT_DRAW_MAIN, NULL);
    }

    s_hud_fill = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hud_fill);
    lv_obj_set_size(s_hud_fill, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_hud_fill);
    lv_obj_clear_flag(s_hud_fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hud_fill, draw_hud_fill, LV_EVENT_DRAW_MAIN, NULL);

    lv_obj_t *hdr = lv_label_create(scr);
    lv_label_set_text(hdr, "MANIFOLD PRESSURE");
    lv_obj_set_style_text_font(hdr, F_COND14, 0);
    lv_obj_set_style_text_letter_space(hdr, 2, 0);
    lv_obj_set_style_text_color(hdr, c(theme->vacuum), 0);
    lv_obj_align(hdr, LV_ALIGN_CENTER, 0, -108);

    /* Created before the digits so the red/cyan ghosts paint behind them. */
    s_hud_glitch = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hud_glitch);
    lv_obj_set_size(s_hud_glitch, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_hud_glitch);
    lv_obj_clear_flag(s_hud_glitch, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hud_glitch, draw_hud_glitch, LV_EVENT_DRAW_MAIN, NULL);

    for (int i = 0; i < HUD_SLOT_COUNT; ++i) {
        s_hud_slot[i] = lv_label_create(scr);
        lv_label_set_text(s_hud_slot[i], i == HUD_SLOT_DOT ? "." : (i == HUD_SLOT_ONES ? "0" : ""));
        lv_obj_set_style_text_font(s_hud_slot[i], HUD_VALUE_FONT, 0);
        lv_obj_set_style_text_color(s_hud_slot[i], c(theme->boost), 0);
        lv_obj_set_size(s_hud_slot[i], 56, 70);
        lv_obj_set_style_text_align(s_hud_slot[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_hud_slot[i], LV_ALIGN_CENTER, k_hud_slot_x[i], HUD_VALUE_Y);
        lv_obj_clear_flag(s_hud_slot[i], LV_OBJ_FLAG_CLICKABLE);
    }
    s_hud_sign = lv_label_create(scr);
    lv_label_set_text(s_hud_sign, "");
    lv_obj_set_style_text_font(s_hud_sign, HUD_VALUE_FONT, 0);
    lv_obj_set_style_text_color(s_hud_sign, c(theme->boost), 0);
    lv_obj_set_size(s_hud_sign, 38, 70);
    lv_obj_set_style_text_align(s_hud_sign, LV_TEXT_ALIGN_CENTER, 0);
    s_hud_sign_x = HUD_SIGN_ONES_X;
    lv_obj_align(s_hud_sign, LV_ALIGN_CENTER, s_hud_sign_x, HUD_VALUE_Y);
    lv_obj_clear_flag(s_hud_sign, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *unit = lv_label_create(scr);
    lv_label_set_text(unit, "PSI // FORCED INDUCTION");
    lv_obj_set_style_text_font(unit, F_COND14, 0);
    lv_obj_set_style_text_color(unit, c(theme->muted), 0);
    /* Clear of the lower reticle brackets, which end at +52. */
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 84);

    s_hud_map = lv_label_create(scr);
    lv_label_set_text(s_hud_map, "MAP 101kPa");
    lv_obj_set_style_text_font(s_hud_map, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_map, c(theme->vacuum), 0);
    lv_obj_align(s_hud_map, LV_ALIGN_CENTER, -100, 128);

    s_hud_pk = lv_label_create(scr);
    lv_label_set_text(s_hud_pk, "PK 0.0");
    lv_obj_set_style_text_font(s_hud_pk, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_pk, c(theme->vacuum), 0);
    lv_obj_align(s_hud_pk, LV_ALIGN_CENTER, 100, 128);

    s_hud_sys = lv_label_create(scr);
    lv_label_set_text(s_hud_sys, "SYS ONLINE");
    lv_obj_set_style_text_font(s_hud_sys, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_sys, c(theme->muted), 0);
    lv_obj_align(s_hud_sys, LV_ALIGN_CENTER, 0, 152);
}

static void update_hud(const boost_sample_t *sample, const boost_theme_t *theme)
{
    if (!s_hud_fill_valid) {
        s_hud_fill_deg = psi_to_sweep(sample->psi, HUD_A0, HUD_A1);
        s_hud_fill_psi = sample->psi;
        s_hud_fill_valid = true;
    }
    const float old_a = s_hud_fill_deg;
    const float new_a = psi_to_sweep(sample->psi, HUD_A0, HUD_A1);
    const float zero_a = psi_to_sweep(0.0f, HUD_A0, HUD_A1);
    /* In gradient mode the whole fill recolours whenever the quantised step
     * changes, not only at the vacuum/boost/overboost boundaries. */
    const bool grad_flip = boost_theme_hud_gradient() &&
        !lv_color_eq(c(gradient_rgb_for_psi(theme, s_hud_fill_psi)),
                     c(gradient_rgb_for_psi(theme, sample->psi)));
    const bool zone_flip = grad_flip ||
                           (s_hud_fill_psi < 0.0f) != (sample->psi < 0.0f) ||
                           (s_hud_fill_psi >= s_psi_overboost) != (sample->psi >= s_psi_overboost);
    if (zone_flip) {
        /* Colour of the whole fill changes: both spans must be repainted. */
        s_hud_fill_deg = new_a;
        s_hud_fill_psi = sample->psi;
        invalidate_hud_fill(fminf(old_a, zero_a), fmaxf(old_a, zero_a));
        invalidate_hud_fill(fminf(new_a, zero_a), fmaxf(new_a, zero_a));
    } else if (fabsf(new_a - old_a) > 0.30f) {
        s_hud_fill_deg = new_a;
        s_hud_fill_psi = sample->psi;
        /* Otherwise only the wedge between the old and new ends changed —
         * repainting the full arc every frame is what was costing cadence. */
        invalidate_hud_fill(old_a, new_a);
    }

    /* One decimal, fixed slots: decimal + tenths pinned, integer grows left. */
    const int tenths_total = (int)lroundf(fabsf(sample->psi) * 10.0f);
    const int whole = tenths_total / 10;
    const bool has_tens = whole >= 10;
    char slot_txt[HUD_SLOT_COUNT][2] = {
        { has_tens ? (char)('0' + (whole / 10) % 10) : '\0', 0 },
        { (char)('0' + whole % 10), 0 },
        { '.', 0 },
        { (char)('0' + tenths_total % 10), 0 },
    };
    const lv_color_t vc = sample->psi >= s_psi_overboost ? c(theme->overboost) : c(theme->boost);
    /* Track which slots actually moved: the ghost pass only has to repaint the
     * glyphs that changed, and in the common case that is the tenths alone. */
    int dirty_lo = HUD_SLOT_COUNT;
    int dirty_hi = -1;
    for (int i = 0; i < HUD_SLOT_COUNT; ++i) {
        bool changed = false;
        if (strcmp(lv_label_get_text(s_hud_slot[i]), slot_txt[i]) != 0) {
            lv_label_set_text(s_hud_slot[i], slot_txt[i]);
            changed = true;
        }
        if (!lv_color_eq(lv_obj_get_style_text_color(s_hud_slot[i], 0), vc)) {
            lv_obj_set_style_text_color(s_hud_slot[i], vc, 0);
            changed = true;
        }
        if (changed) {
            if (i < dirty_lo) dirty_lo = i;
            if (i > dirty_hi) dirty_hi = i;
        }
    }
    /* Keep the flat string for the ghost pass, and arm the glitch on a spike. */
    char prev_val[sizeof(s_hud_val_str)];
    snprintf(prev_val, sizeof(prev_val), "%s", s_hud_val_str);
    snprintf(s_hud_val_str, sizeof(s_hud_val_str), "%s%d.%d",
             sample->psi < -0.05f ? "-" : "", whole, tenths_total % 10);
    const bool value_changed = strcmp(prev_val, s_hud_val_str) != 0;
    /* Always on: the ghosts repaint together with the digits, so there is no
     * transition left to clear and no chance of a stranded frame. */
    s_hud_prev_psi = sample->psi;

    /* Sign is resolved before the ghost invalidation so a sign flip or a slide
     * between the ones/tens anchors can widen the dirty box. */
    const char *sign = sample->psi < -0.05f ? "-" : "";
    bool sign_changed = false;
    if (strcmp(lv_label_get_text(s_hud_sign), sign) != 0) {
        lv_label_set_text(s_hud_sign, sign);
        sign_changed = true;
    }
    if (!lv_color_eq(lv_obj_get_style_text_color(s_hud_sign, 0), vc)) {
        lv_obj_set_style_text_color(s_hud_sign, vc, 0);
        sign_changed = true;
    }
    const int sign_x = has_tens ? HUD_SIGN_TENS_X : HUD_SIGN_ONES_X;
    if (sign_x != s_hud_sign_x) {
        s_hud_sign_x = sign_x;
        lv_obj_align(s_hud_sign, LV_ALIGN_CENTER, sign_x, HUD_VALUE_Y);
        sign_changed = true;
    }

    if (value_changed && s_hud_glitch != NULL) {
        /* Repainting the whole 310x100 value box on every tenth was the single
         * biggest cost in this style. Bound it to the slots that changed, grown
         * by the ghost offset plus a pixel of AA. A sign flip is folded in via
         * dirty_lo, since the sign sits left of the leftmost digit. */
        const int grow = HUD_GLITCH_DX + 1;
        int lo = dirty_lo, hi = dirty_hi;
        if (sign_changed) lo = 0;
        if (hi < 0) { lo = 0; hi = HUD_SLOT_COUNT - 1; }
        lv_area_t ga;
        ga.x1 = px_icx() + k_hud_slot_x[lo] - 28 - grow;
        ga.x2 = px_icx() + k_hud_slot_x[hi] + 28 + grow;
        ga.y1 = px_icy() + HUD_VALUE_Y - 42;
        ga.y2 = px_icy() + HUD_VALUE_Y + 42;
        if (sign_changed) ga.x1 = px_icx() + HUD_SIGN_TENS_X - 24 - grow;
        lv_obj_invalidate_area(s_hud_glitch, &ga);
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "MAP %dkPa", (int)lroundf(101.0f + sample->psi * 6.895f));
    if (strcmp(lv_label_get_text(s_hud_map), buf) != 0) lv_label_set_text(s_hud_map, buf);
    snprintf(buf, sizeof(buf), "PK %.1f", (double)s_peak_psi);
    if (strcmp(lv_label_get_text(s_hud_pk), buf) != 0) lv_label_set_text(s_hud_pk, buf);
    /* Real-sensor mode carries no DEMO indicator; the sweep shows it as before. */
    const char *sys = sample->demo ? "SYS DEMO" : "";
    if (strcmp(lv_label_get_text(s_hud_sys), sys) != 0) lv_label_set_text(s_hud_sys, sys);
}

/* ========================================================================== */
/*  Style: bigdigit  (Alvida numeral on a color-sweeping ground)              */
/* ========================================================================== */

/* BIG_STEPS defined near the top so the shared gradient helper can use it. */

/* Slot geometry from the generated Alvida metrics: widest digit advance 81 px,
 * '.' 34 px. Centre of the face falls halfway between the ones digit and the
 * decimal; higher digits and the sign grow leftward like an odometer. */
#define BIG_SLOT        81
#define BIG_DOTW        34
#define BIG_ONES_X      (-29)
#define BIG_DOT_X       29
#define BIG_TENTHS_X    86
#define BIG_TENS_X      (-110)
#define BIG_MINUS_W     52
#define BIG_MINUS_H     15
#define BIG_MINUS_Y     (-14)
/* Sign hugs whichever integer digit is leftmost. */
#define BIG_MINUS_ONES_X (-98)
#define BIG_MINUS_TENS_X (-179)

static int s_big_minus_x = BIG_MINUS_ONES_X;

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
    t = clampf(t, 0.0f, 1.0f);
    const int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    const int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    const int r = ar + (int)lroundf((float)(br - ar) * t);
    const int g = ag + (int)lroundf((float)(bg - ag) * t);
    const int bl = ab + (int)lroundf((float)(bb - ab) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static int big_step_for(float psi)
{
    const float lo = s_psi_min;
    const float hi = s_psi_max;
    const float t = (hi > lo) ? (clampf(psi, lo, hi) - lo) / (hi - lo) : 0.0f;
    return (int)lroundf(t * (float)(BIG_STEPS - 1));
}

static uint32_t big_color_for_step(const boost_theme_t *theme, int step)
{
    const float t = (float)step / (float)(BIG_STEPS - 1);
    const float psi = s_psi_min + (s_psi_max - s_psi_min) * t;
    if (psi <= 0.0f) return theme->vacuum;
    /* The red ramp used to be squeezed into overboost..max (about 2 psi), so it
     * snapped. Start blending earlier so the approach is gradual. */
    const float red_start = s_psi_overboost * 0.55f;
    if (psi <= red_start) {
        return lerp_rgb(theme->vacuum, theme->boost, psi / fmaxf(0.001f, red_start));
    }
    const float span = fmaxf(0.001f, s_psi_max - red_start);
    return lerp_rgb(theme->boost, theme->overboost, (psi - red_start) / span);
}

/* Trapezoid sign: a straight bar read as too plain beside the fatface digits. */
static void draw_big_minus(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const float x1 = (float)a.x1, x2 = (float)a.x2;
    const float y1 = (float)a.y1, y2 = (float)a.y2;
    /* Slanted rectangle: both edges lean the same way, echoing the italic
     * stress of the digits. */
    const float slant = (y2 - y1) * 0.62f;

    lv_draw_triangle_dsc_t t;
    lv_draw_triangle_dsc_init(&t);
    /* Follows the readout when the colour cue is on the text. */
    t.color = boost_theme_bigdigit_color_text() && s_big_text_color != 0u
                  ? c(s_big_text_color)
                  : c(boost_theme_bigdigit_text_color());
    t.opa = LV_OPA_COVER;
    t.p[0].x = x1 + slant; t.p[0].y = y1;
    t.p[1].x = x2;         t.p[1].y = y1;
    t.p[2].x = x2 - slant; t.p[2].y = y2;
    lv_draw_triangle(layer, &t);
    t.p[0].x = x1 + slant; t.p[0].y = y1;
    t.p[1].x = x2 - slant; t.p[1].y = y2;
    t.p[2].x = x1;         t.p[2].y = y2;
    lv_draw_triangle(layer, &t);
}

static void build_bigdigit(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    /* The ground is the screen's own background. A full-screen child with
     * LV_RADIUS_CIRCLE re-evaluated a rounded mask inside every dirty region,
     * which alone cost more than half the frame budget. The glass is round, so
     * a plain fill looks identical. */
    s_big_bg = NULL;
    /* A static ground uses the face colour: white numerals on near-black is the
     * legible pairing, and the sweep colours assume they are being swept. */
    const uint32_t ground =
        boost_theme_bigdigit_static_bg() ? boost_theme_bigdigit_static_color()
                                         : theme->vacuum;
    /* Explicitly the screen, not `scr`: `scr` is the shifting container now,
     * and the whole point of this fill is that it does NOT shift, so it backs
     * the margin the shift opens at the edge. */
    lv_obj_set_style_bg_color(lv_screen_active(), c(ground), 0);
    s_big_bg_step = -1;

    /* Recolouring the ground in one go dirties all 217k pixels at once: a ~69 ms
     * stall that reads as a lurch every colour step. The work is unavoidable —
     * a flat fill is already the cheapest primitive there is, so there is
     * nothing to pre-render — but it does not have to land in a single cycle.
     * Four full-width bands each take their new colour on a later tick, turning
     * one long stall into four short ones. Adjacent steps differ by about a
     * twelfth of a colour transition, so the wipe is not visible as a seam. */
    for (int i = 0; i < BIG_BANDS; ++i) {
        const int y0 = DISP_SIZE * i / BIG_BANDS;
        const int y1 = DISP_SIZE * (i + 1) / BIG_BANDS;
        s_big_band[i] = lv_obj_create(scr);
        lv_obj_remove_style_all(s_big_band[i]);
        lv_obj_set_size(s_big_band[i], DISP_SIZE, y1 - y0);
        lv_obj_set_pos(s_big_band[i], 0, y0);
        lv_obj_set_style_bg_color(s_big_band[i], c(ground), 0);
        lv_obj_set_style_bg_opa(s_big_band[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_big_band[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    s_big_band_next = BIG_BANDS;
    s_big_band_color = ground;

    s_big_zone = lv_label_create(scr);
    lv_label_set_text(s_big_zone, "ATMO");
    lv_obj_set_style_text_font(s_big_zone, F_WIDE22, 0);
    lv_obj_set_style_text_letter_space(s_big_zone, 3, 0);
    lv_obj_set_style_text_color(s_big_zone, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_align(s_big_zone, LV_ALIGN_CENTER, 0, -150);

    /* Tabular slots sized from the real Alvida metrics (widest digit '0'
     * advances 80.6 px, '.' 33.9 px at this size). The face centre sits halfway
     * between the ones digit and the decimal, matching the web mirror. */
    const int ones_x = BIG_ONES_X;
    const int dot_x = BIG_DOT_X;
    const int tenths_x = BIG_TENTHS_X;
    const int tens_x = BIG_TENS_X;

    s_big_tens = lv_label_create(scr);
    lv_label_set_text(s_big_tens, "");
    lv_obj_set_style_text_font(s_big_tens, BIGDIGIT_FONT, 0);
    lv_obj_set_style_text_color(s_big_tens, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_set_size(s_big_tens, BIG_SLOT + 6, 96);
    lv_obj_set_style_text_align(s_big_tens, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_big_tens, LV_ALIGN_CENTER, tens_x, -8);

    s_big_ones = lv_label_create(scr);
    lv_label_set_text(s_big_ones, "0");
    lv_obj_set_style_text_font(s_big_ones, BIGDIGIT_FONT, 0);
    lv_obj_set_style_text_color(s_big_ones, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_set_size(s_big_ones, BIG_SLOT + 6, 96);
    lv_obj_set_style_text_align(s_big_ones, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_big_ones, LV_ALIGN_CENTER, ones_x, -8);

    s_big_dot = lv_label_create(scr);
    lv_label_set_text(s_big_dot, ".");
    lv_obj_set_style_text_font(s_big_dot, BIGDIGIT_FONT, 0);
    lv_obj_set_style_text_color(s_big_dot, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_set_size(s_big_dot, BIG_DOTW + 6, 96);
    lv_obj_set_style_text_align(s_big_dot, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_big_dot, LV_ALIGN_CENTER, dot_x, -8);

    s_big_tenths = lv_label_create(scr);
    lv_label_set_text(s_big_tenths, "0");
    lv_obj_set_style_text_font(s_big_tenths, BIGDIGIT_FONT, 0);
    lv_obj_set_style_text_color(s_big_tenths, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_set_size(s_big_tenths, BIG_SLOT + 6, 96);
    lv_obj_set_style_text_align(s_big_tenths, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_big_tenths, LV_ALIGN_CENTER, tenths_x, -8);

    /* Fat stylised minus: a chunky bar, not the font's hairline hyphen. */
    s_big_minus = lv_obj_create(scr);
    lv_obj_remove_style_all(s_big_minus);
    lv_obj_set_size(s_big_minus, BIG_MINUS_W, BIG_MINUS_H);
    s_big_minus_x = BIG_MINUS_ONES_X;
    lv_obj_align(s_big_minus, LV_ALIGN_CENTER, s_big_minus_x, BIG_MINUS_Y);
    lv_obj_add_event_cb(s_big_minus, draw_big_minus, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_clear_flag(s_big_minus, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_big_minus, LV_OBJ_FLAG_HIDDEN);

    s_big_unit = lv_label_create(scr);
    lv_label_set_text(s_big_unit, "PSI");
    lv_obj_set_style_text_font(s_big_unit, F_WIDE32, 0);
    lv_obj_set_style_text_letter_space(s_big_unit, 4, 0);
    lv_obj_set_style_text_color(s_big_unit, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_align(s_big_unit, LV_ALIGN_CENTER, 0, 118);

    s_big_peak = lv_label_create(scr);
    lv_label_set_text(s_big_peak, "PEAK 0.0");
    lv_obj_set_style_text_font(s_big_peak, F_MONO16, 0);
    lv_obj_set_style_text_color(s_big_peak, c(boost_theme_bigdigit_text_color()), 0);
    lv_obj_align(s_big_peak, LV_ALIGN_CENTER, 0, 168);
}

static void update_bigdigit(const boost_sample_t *sample, const boost_theme_t *theme)
{
    /* Static ground skips only the recolour, never the readout below it. The
     * sweep is the one thing that repaints this face full-screen, so switching
     * it off removes the stall - but returning early here also froze the
     * digits, which is not what "static background" means. */
    if (!boost_theme_bigdigit_static_bg()) {
        const int step = big_step_for(sample->psi);
        if (step != s_big_bg_step) {
            s_big_bg_step = step;
            /* Retarget: a step arriving mid-wipe simply restarts it at the
             * newest colour, so the ground can never settle on a stale one. */
            s_big_band_color = big_color_for_step(theme, step);
            s_big_band_next = 0;
        }
        if (s_big_band_next < BIG_BANDS) {
            lv_obj_set_style_bg_color(s_big_band[s_big_band_next], c(s_big_band_color), 0);
            s_big_band_next++;
            /* The bands are exactly screen-sized and travel with the burn-in
             * shift, so the screen fill behind them shows through a one or two
             * pixel margin at one edge and has to follow the same colour. Done
             * on the last band so a multi-band wipe stays banded: at
             * BIG_BANDS == 1 this lands on the same tick and unions into the
             * full-screen invalidation the band already caused, for free. */
            if (s_big_band_next == BIG_BANDS) {
                lv_obj_set_style_bg_color(lv_screen_active(), c(s_big_band_color), 0);
            }
        }
    }

    /* Colour on the readout instead of the ground. The glyphs cover ~31k px
     * against the ground's 217k, so a colour step repaints about a seventh as
     * much and never touches the full screen. */
    if (boost_theme_bigdigit_color_text()) {
        const int tstep = big_step_for(sample->psi);
        if (tstep != s_big_text_step) {
            s_big_text_step = tstep;
            s_big_text_color = big_color_for_step(theme, tstep);
            const lv_color_t tc = c(s_big_text_color);
            lv_obj_t *const slots[4] = { s_big_tens, s_big_ones, s_big_dot, s_big_tenths };
            for (int i = 0; i < 4; ++i) {
                if (slots[i] != NULL) lv_obj_set_style_text_color(slots[i], tc, 0);
            }
            /* The sign is drawn, not a label, so it needs an explicit repaint. */
            if (s_big_minus != NULL) lv_obj_invalidate(s_big_minus);
        }
    }

    const int tenths_total = (int)lroundf(fabsf(sample->psi) * 10.0f);
    const int whole = tenths_total / 10;
    const int tenth = tenths_total % 10;
    char d[2] = {0};

    d[0] = (char)('0' + (whole % 10));
    if (strcmp(lv_label_get_text(s_big_ones), d) != 0) lv_label_set_text(s_big_ones, d);
    d[0] = (char)('0' + tenth);
    if (strcmp(lv_label_get_text(s_big_tenths), d) != 0) lv_label_set_text(s_big_tenths, d);

    const bool has_tens = whole >= 10;
    if (has_tens) {
        d[0] = (char)('0' + (whole / 10));
        if (strcmp(lv_label_get_text(s_big_tens), d) != 0) lv_label_set_text(s_big_tens, d);
    } else if (lv_label_get_text(s_big_tens)[0] != '\0') {
        lv_label_set_text(s_big_tens, "");
    }

    /* Sign sits beside whichever integer digit is leftmost. */
    const int minus_x = has_tens ? BIG_MINUS_TENS_X : BIG_MINUS_ONES_X;
    if (minus_x != s_big_minus_x) {
        s_big_minus_x = minus_x;
        lv_obj_align(s_big_minus, LV_ALIGN_CENTER, minus_x, BIG_MINUS_Y);
    }

    const bool neg = sample->psi < -0.05f;
    const bool hidden = lv_obj_has_flag(s_big_minus, LV_OBJ_FLAG_HIDDEN);
    if (neg && hidden) lv_obj_remove_flag(s_big_minus, LV_OBJ_FLAG_HIDDEN);
    else if (!neg && !hidden) lv_obj_add_flag(s_big_minus, LV_OBJ_FLAG_HIDDEN);

    const char *zone = zone_for_psi(sample->psi);
    if (strcmp(lv_label_get_text(s_big_zone), zone) != 0) lv_label_set_text(s_big_zone, zone);

    char buf[32];
    /* Real-sensor mode drops the DEMO suffix and shows just the peak. */
    if (sample->demo) {
        snprintf(buf, sizeof(buf), "PEAK %.1f  DEMO", (double)s_peak_psi);
    } else {
        snprintf(buf, sizeof(buf), "PEAK %.1f", (double)s_peak_psi);
    }
    if (strcmp(lv_label_get_text(s_big_peak), buf) != 0) lv_label_set_text(s_big_peak, buf);
}

/* ========================================================================== */
/*  Scene lifecycle                                                           */
/* ========================================================================== */

static void destroy_scene(void)
{
    lv_obj_t *scr = lv_screen_active();
    uint32_t i = 0;
    while (i < lv_obj_get_child_count(scr)) {
        lv_obj_t *child = lv_obj_get_child(scr, i);
#if LV_USE_GIF
        if (child == s_media_gif) {
            ++i;
            continue;
        }
#endif
        lv_obj_delete(child);
        /* children shift down; do not advance */
    }

    s_well = NULL;
    s_root = NULL;
    s_arc_track = NULL;
    s_arc_value_canvas = NULL;
    s_zero_notch = NULL;
    s_value_sign_label = s_value_tens_label = s_value_ones_label = NULL;
    s_value_decimal_label = s_value_tenths_label = NULL;
    s_unit_label = s_peak_label = s_mode_label = s_zone_label = NULL;
    for (size_t k = 0; k < sizeof(s_tick_labels) / sizeof(s_tick_labels[0]); ++k) s_tick_labels[k] = NULL;

    if (s_vault_bg_buf != NULL) {
        BG_FREE(s_vault_bg_buf);
        s_vault_bg_buf = NULL;
    }
    s_vault_bg = s_vault_peak_mark = s_vault_crt = NULL;
    s_vault_needle = s_vault_window = NULL;
    s_vault_peak = s_vault_alert = s_vault_alert_marks = NULL;
    for (int k = 0; k < VAULT_SLOT_COUNT; ++k) s_vault_slot[k] = NULL;

    if (s_hud_bg_buf != NULL) {
        BG_FREE(s_hud_bg_buf);
        s_hud_bg_buf = NULL;
    }
    s_hud_bg = NULL;
    s_hud_face = s_hud_fill = s_hud_glitch = NULL;
    s_hud_sign = NULL;
    s_hud_val_str[0] = '\0';
    s_hud_fill_valid = false;
    s_hud_fill_deg = 0.0f;
    s_hud_fill_psi = 0.0f;
    for (int k = 0; k < HUD_SLOT_COUNT; ++k) s_hud_slot[k] = NULL;
    s_hud_sign_x = HUD_SIGN_ONES_X;
    s_hud_map = s_hud_pk = s_hud_sys = NULL;

    s_big_bg = s_big_minus = s_big_tens = s_big_ones = NULL;
    s_big_dot = s_big_tenths = s_big_unit = s_big_zone = s_big_peak = NULL;
    s_big_bg_step = -1;
    s_big_text_step = -1;
    for (int k = 0; k < BIG_BANDS; ++k) s_big_band[k] = NULL;
    s_big_band_next = BIG_BANDS;
}

static void build_scene(boost_gauge_style_t style)
{
    const boost_theme_t *theme = active_theme();
    lv_obj_t *scr = lv_screen_active();

    /* The screen's own opaque background is the face. A second full-screen
     * "well" object used to sit on top of it, which meant every dirty region
     * was filled twice per frame — pure cost on the needle sweep. */
    lv_obj_set_style_bg_color(scr, c(theme->face), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* One styleless, screen-sized container that every style builds into, so
     * the burn-in shift is a single lv_obj_set_pos() instead of a walk over
     * every object — and, more importantly, so the cached PSRAM faces move as
     * bitmaps rather than being re-rasterised. It draws nothing and adds one
     * bounds test per redraw. Children keep addressing the same coordinates
     * they always did, because it is exactly screen-sized and starts at the
     * origin. */
    s_root = lv_obj_create(scr);
    lv_obj_remove_style_all(s_root);
    lv_obj_set_size(s_root, DISP_SIZE, DISP_SIZE);
    lv_obj_set_pos(s_root, s_px_dx, s_px_dy);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    switch (style) {
        case BOOST_STYLE_VAULT:    build_vault(s_root); break;
        case BOOST_STYLE_HUD:      build_hud(s_root); break;
        case BOOST_STYLE_BIGDIGIT: build_bigdigit(s_root); break;
        case BOOST_STYLE_ARC:
        default:                   build_arc(s_root); break;
    }

    s_built_style = style;
#if LV_USE_GIF
    /* A rebuild creates fresh, visible objects. If media is playing they would
     * otherwise land on top of the GIF. */
    if (s_media_gif != NULL) {
        set_gauge_hidden(true);
        lv_obj_move_foreground(s_media_gif);
    }
#endif
    ESP_LOGI(TAG, "scene built: style=%s", boost_style_name(style));
}

void boost_gauge_create(void)
{
    load_range_from_config();
    const boost_theme_t *theme = active_theme();
    snprintf(s_theme_id, sizeof(s_theme_id), "%s", theme->id);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, DISP_SIZE, DISP_SIZE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(scr, on_screen_event, LV_EVENT_LONG_PRESSED, NULL);

    s_display_psi = 0.0f;
    s_peak_psi = 0.0f;
    s_px_step_ms = lv_tick_get();
    s_px_settled_ms = s_px_step_ms;
    s_px_ref_psi = 0.0f;
    build_scene(theme->style);
    s_ui_ready = true;
    ESP_LOGI(TAG, "UI ready (style %s)", boost_style_name(theme->style));
}

void boost_gauge_apply_theme(const boost_theme_t *theme)
{
    if (!s_ui_ready || theme == NULL) return;
#ifndef ESP_PLATFORM
    s_host_theme = theme;
#endif
    snprintf(s_theme_id, sizeof(s_theme_id), "%s", theme->id);
    /* A theme is a whole face, so rebuild rather than recolour in place. */
    destroy_scene();
    build_scene(theme->style);
    lv_obj_invalidate(lv_screen_active());
}

void boost_gauge_apply_config(void)
{
    if (!s_ui_ready) return;
    load_range_from_config();
    if (s_built_style == BOOST_STYLE_ARC) {
        refresh_zero_notch();
        refresh_tick_labels();
        if (s_arc_value_canvas) lv_obj_invalidate(s_arc_value_canvas);
    } else {
        /* Ranges move ticks/numerals on the stylised faces: rebuild. */
        destroy_scene();
        build_scene(s_built_style);
    }
    lv_obj_invalidate(lv_screen_active());
}

bool boost_gauge_media_load(void)
{
#if LV_USE_GIF && defined(ESP_PLATFORM)
    if (boost_display_lock(5000) != ESP_OK) return false;
    destroy_media_gif();
    boost_media_store_unmap();
    const bool loaded = load_media_gif_locked();
    boost_display_unlock();
    return loaded;
#else
    return false;
#endif
}

void boost_gauge_media_delete(void)
{
#if LV_USE_GIF && defined(ESP_PLATFORM)
    if (boost_display_lock(5000) != ESP_OK) return;
    destroy_media_gif();
    boost_display_unlock();
    boost_media_store_unmap();
#endif
}

/* Move the whole scene to a new burn-in offset. */
static void set_pixel_shift(int32_t dx, int32_t dy)
{
    /* Clamped rather than trusted: an offset outside this range puts the vault
     * bezel ring and the dyno-cell value arc off the edge of the glass, and the
     * table above is exactly the kind of thing that gets widened later by
     * someone who has not re-derived the radii. */
    dx = (int32_t)clampf((float)dx, (float)PXSHIFT_MIN, (float)PXSHIFT_MAX);
    dy = (int32_t)clampf((float)dy, (float)PXSHIFT_MIN, (float)PXSHIFT_MAX);
    if (dx == s_px_dx && dy == s_px_dy) return;
    s_px_dx = dx;
    s_px_dy = dy;
    if (s_root != NULL) {
        lv_obj_set_pos(s_root, dx, dy);
    }
    /* A shift moves every cached bitmap AND retargets every draw callback that
     * reads px_cx()/px_cy(), and it uncovers a margin of screen background at
     * one edge. Nothing narrower than the whole screen is provably free of
     * stale pixels here, and the cost is exactly the full repaint this design
     * already budgets for once every couple of minutes. */
    lv_obj_invalidate(lv_screen_active());
}

/*
 * Decide whether it is time to step the shift. Called once per sample, ahead of
 * the per-style update, so the invalidations that update issues are computed
 * against the offset that will actually be drawn.
 */
static void pixel_shift_tick(float psi)
{
    const uint32_t now = lv_tick_get();

    if (!boost_theme_pixel_shift()) {
        /* Park at the origin so switching it off restores the exact geometry
         * the faces were designed and screenshotted at. */
        set_pixel_shift(0, 0);
        s_px_step = 0;
        s_px_step_ms = now;
        s_px_settled_ms = now;
        s_px_ref_psi = psi;
        return;
    }

    /* "Settled" means the reading has not wandered more than a quarter psi for
     * a second and a half — not merely that this one tick was quiet, which a
     * slow sweep would also satisfy. */
    if (fabsf(psi - s_px_ref_psi) > PXSHIFT_SETTLE_PSI) {
        s_px_ref_psi = psi;
        s_px_settled_ms = now;
    }

    const uint32_t period_ms = (uint32_t)boost_theme_pixel_shift_sec() * 1000u;
    if (lv_tick_elaps(s_px_step_ms) < period_ms) return;
    if (lv_tick_elaps(s_px_settled_ms) < PXSHIFT_SETTLE_MS &&
        lv_tick_elaps(s_px_step_ms) < period_ms + PXSHIFT_GRACE_MS) {
        return;
    }

    s_px_step_ms = now;
    s_px_step = (uint8_t)((s_px_step + 1u) % PXSHIFT_STEPS);
    set_pixel_shift(k_pxshift[s_px_step][0], k_pxshift[s_px_step][1]);
}

void boost_gauge_update(const boost_sample_t *sample)
{
    if (!s_ui_ready || sample == NULL) return;
    const boost_theme_t *theme = active_theme();
    s_peak_psi = fmaxf(s_peak_psi, fmaxf(sample->peak_psi, 0.0f));

    /* Before the per-style update: those compute dirty areas from px_cx()/
     * px_cy(), and an offset that changed between the invalidation and the
     * draw is precisely how stale pixels get stranded. */
    pixel_shift_tick(sample->psi);

    switch (s_built_style) {
        case BOOST_STYLE_VAULT:    update_vault(sample, theme); break;
        case BOOST_STYLE_HUD:      update_hud(sample, theme); break;
        case BOOST_STYLE_BIGDIGIT: update_bigdigit(sample, theme); break;
        case BOOST_STYLE_ARC:
        default:                   update_arc(sample, theme); break;
    }

    s_display_psi = sample->psi;
}
