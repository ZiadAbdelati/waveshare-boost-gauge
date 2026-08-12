#include "boost_gauge.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "boost_media_store.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#define ESP_LOGE(tag, fmt, ...) printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

static double neon_now_ms(void)
{
#ifdef ESP_PLATFORM
    return (double)esp_timer_get_time() / 1000.0;
#else
    return (double)clock() * 1000.0 / (double)CLOCKS_PER_SEC;
#endif
}
#include "lvgl.h"
#include "boost_brightness.h"
#include "boost_neon_geom.h"
#ifdef ESP_PLATFORM
#include "boost_model.h"
#include "boost_display.h"
#include "boost_sensors.h"
#else
static inline void boost_display_gauge_update_begin(void) {}
static inline void boost_display_gauge_update_end(void) {}
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
 * in its geometry/invalidation logic â€” it is the one gated by the 60 FPS
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
#define WELL_SIZE     DISP_SIZE

/*
 * AMOLED burn-in countermeasure.
 * -----------------------------
 * One face is shown for hours at a time at 85-92% brightness with high-contrast
 * art pinned to fixed pixels â€” tick rings, "VAULT-TEC", reticle brackets, the
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
 * 4-row scanline overlay â€” without that the three rows between scanlines would
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
/* A step dirties all 217k pixels â€” about 45 ms, three dropped frames. Held
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
/* Optional counterweight geometry. The persisted theme preference resolves this
 * to either 0 or 26 px; draw and invalidation must always use the same value. */
#define VAULT_NEEDLE_TAIL_LEN 26
#define VAULT_NEEDLE_HALFW   7
/* Tapered-wedge tip half-width, shared by the draw (triangle tips) and the
 * invalidation (per-slice pad) so the two can never drift apart. */
#define VAULT_NEEDLE_TIP_HALF 2.5f
#define VAULT_HUB_R          15
/* Sits just inside the bezel ring (r=231, width 3, so its inner edge is at
 * 229.5): the tell-tale must stop where the green circle starts, not cross it. */
#define VAULT_PEAK_R         220
/* Size of the peak tell-tale's LVGL object box. The drawn triangle's vertices
 * sit at +-8 px along the radial axis and +-7.5 px tangential, so the ink's
 * furthest corner is sqrt(8^2 + 7.5^2) ~= 10.97 px from the centre (~22 px
 * across); with AA that needs at most ~24 px. The box was 34x34 for years,
 * flushing ~34x34 = 1156 px whenever the marker moved; 26 keeps a 2 px AA
 * margin over the worst-case ink extent and is byte-identical on the glass
 * (LV_ALIGN_CENTER keeps the centre fixed, so mx/my in draw_vault_peak_mark()
 * are unchanged). */
#define VAULT_PEAK_MARK_SIZE 26
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
/* The readout callback is clipped to this object. Keep the bounds in face-local
 * coordinates: draw_hud_readout() still emits absolute screen coordinates via
 * px_icx()/px_icy(), while the parent scene can move for burn-in protection. The
 * extra invalidation margin is included so lv_obj_invalidate_area() does not
 * silently clip the old/new union at the object edge. */
#define HUD_READOUT_OBJ_X1 (-162) /* left ghost edge: -156 - HUD_GLITCH_DX */
#define HUD_READOUT_OBJ_X2 (117)  /* right slot invalidation edge: 82 + 28 + 7 */
#define HUD_READOUT_OBJ_Y1 (HUD_VALUE_Y - 42)
#define HUD_READOUT_OBJ_Y2 (HUD_VALUE_Y + 42)
#define HUD_READOUT_OBJ_W  (HUD_READOUT_OBJ_X2 - HUD_READOUT_OBJ_X1 + 1)
#define HUD_READOUT_OBJ_H  (HUD_READOUT_OBJ_Y2 - HUD_READOUT_OBJ_Y1 + 1)

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
LV_FONT_DECLARE(neon_big);
LV_FONT_DECLARE(neon_label);
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
#define NEON_BIG   (&neon_big)
#define NEON_LABEL (&neon_label)
#define HUD_READOUT_GLYPH_COUNT 12

/* Production uses the immutable glyph cache. Set to 0 only for a matched,
 * compile-time source-font A/B; there is deliberately no runtime switch. */
#ifndef BOOST_HUD_READOUT_CACHE
#define BOOST_HUD_READOUT_CACHE 1
#endif
#if BOOST_HUD_READOUT_CACHE != 0 && BOOST_HUD_READOUT_CACHE != 1
#error "BOOST_HUD_READOUT_CACHE must be 0 or 1"
#endif

/* Pre-rendered opaque glyph sprites for the neon readout, baked once per zone
 * colour at scene build and blitted with lv_draw_image() (LV_OPA_COVER, no
 * recolor) instead of re-running lv_draw_label() every frame. Default on;
 * set to 0 for a matched compile-time A/B against the live-label path. */
#ifndef BOOST_NEON_GLYPH_SPRITES
#define BOOST_NEON_GLYPH_SPRITES 1
#endif
#if BOOST_NEON_GLYPH_SPRITES != 0 && BOOST_NEON_GLYPH_SPRITES != 1
#error "BOOST_NEON_GLYPH_SPRITES must be 0 or 1"
#endif

static const char *TAG = "boost_gauge";

/* ---- shared scene state -------------------------------------------------- */
static lv_obj_t *s_well;
/* Parent supplied by boost_page; the active screen remains the legacy default. */
static lv_obj_t *s_scene_parent;
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
/* Static furniture (unfilled track ring, scale numerals, "PSI" unit mark) is
 * rasterised once into a PSRAM canvas at scene build, the same win vault/hud
 * got. The zero notch stays live above the moving wedge so the wedge cannot
 * cover it. */
static lv_obj_t *s_arc_bg;
static uint8_t *s_arc_bg_buf;
static lv_obj_t *s_arc_value_canvas;
static lv_obj_t *s_zero_notch;
static lv_obj_t *s_value_sign_label;
static lv_obj_t *s_value_tens_label;
static lv_obj_t *s_value_ones_label;
static lv_obj_t *s_value_decimal_label;
static lv_obj_t *s_value_tenths_label;
static lv_obj_t *s_peak_label;
static lv_obj_t *s_mode_label;
static lv_obj_t *s_zone_label;

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
typedef struct {
    float psi_min;
    float psi_max;
    float psi_overboost;
    float zero_angle;
    uint32_t face;
    uint32_t text;
    uint32_t muted;
    uint32_t overboost;
    uint8_t vignette_pct;
    bool valid;
} vault_bg_key_t;
static vault_bg_key_t s_vault_bg_key;
static lv_obj_t *s_vault_crt;
static lv_obj_t *s_vault_peak_mark;
static float s_vault_peak_deg;
static lv_obj_t *s_vault_needle;
static lv_obj_t *s_vault_window;
/* The six fixed readout labels share one draw object. This keeps LVGL from
 * walking six independent label objects on every needle dirty region while
 * retaining each slot's exact area and text alignment. */
static lv_obj_t *s_vault_readout;
static char s_vault_slot_text[VAULT_SLOT_COUNT][2];
static lv_color_t s_vault_readout_color;
static bool s_vault_readout_color_valid;
static lv_obj_t *s_vault_peak;
static lv_obj_t *s_vault_alert;
static lv_obj_t *s_vault_alert_marks;
static float s_vault_needle_deg;
/* Track the body colour independently of angle so a colour-only setting change
 * clears the old pixels even when the needle is stationary. */
static bool s_vault_needle_red;
static bool s_vault_needle_over;
#define VAULT_NEEDLE_RED 0xFF3B30u

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
/* A BMP280 that answered at boot but stopped responding must not leave a stale
 * number on the corner readout, so the ATM line gates on read age, not on the
 * boot presence flag alone. */
#define HUD_BMP_FRESH_MS 2000u

static lv_obj_t *s_hud_face;
static lv_obj_t *s_hud_bg;
static uint8_t *s_hud_bg_buf;
static lv_obj_t *s_hud_fill;
/* The complete 96 px readout is one styleless draw object. Keeping the ghosts
 * and primary glyphs in one callback makes their order explicit while the
 * object's invalidation remains one exact old/new union. */
static lv_obj_t *s_hud_readout;
static char s_hud_slot_text[HUD_SLOT_COUNT][2];
static char s_hud_sign_text[2];
static int s_hud_sign_x = HUD_SIGN_ONES_X;
static lv_color_t s_hud_readout_color;
static bool s_hud_readout_color_valid;
/* The Night City readout uses a scene-owned immutable font. Its ten digits,
 * decimal point and minus sign are expanded once from the packed source font,
 * then read concurrently by LVGL's software draw units without changing image
 * sources at runtime. */
typedef struct {
    lv_font_t font;
    lv_font_glyph_dsc_t glyph[HUD_READOUT_GLYPH_COUNT];
    uint32_t offset[HUD_READOUT_GLYPH_COUNT];
    uint8_t *pixels;
    size_t pixels_size;
} hud_readout_font_t;
static hud_readout_font_t s_hud_readout_font;
/* Angle/value the fill was last painted at. Drawing and invalidating must
 * agree: a skipped invalidation with a moved draw leaves stale pixels. */
static float s_hud_fill_deg;
static float s_hud_fill_psi;
static float s_hud_fill_color_psi;
static bool s_hud_fill_valid;
static char s_hud_val_str[12];
static lv_obj_t *s_hud_map;
static lv_obj_t *s_hud_pk;
static lv_obj_t *s_hud_sys;
static lv_obj_t *s_hud_tag;

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

/* ---- neon style ---------------------------------------------------------- */
/* Arc `radius` in LVGL is the stroke's OUTER edge, not a centreline - so this
 * is exactly how far from centre the brightest pixel of the ring sits. Panel
 * radius is 233; 228 leaves a 5px margin to the bezel, matching marquee's own
 * outermost bulb ring's tips (centreline 224, tips at 228) closely enough to
 * carry no new risk. The tube's lit depth is NEON_TUBE_BAND_DEPTH (55, inner
 * edge 174) and the segments' NEON_SEG_BAND_DEPTH (49, inner edge 179); the
 * widest readout (-12.0, measured ink reaching ~170px at the current
 * NEON_FONT_PX / NEON_SLOT_W / NEON_SIGN_W) clears both. */
#define NEON_R          228     /* ring centre radius */
#define NEON_SEG_W      30      /* segment stroke width */
#define NEON_TUBE_W     26      /* nominal tube stroke width (halo is its own literal now) */
/* Segments: 45 wedges at a FIXED 6-degree pitch (4 lit + 2 gap). The pitch is
 * an exact division of the 270 sweep (45 x 6 = 270, no bare track at the
 * ends), and it must be whole degrees because LVGL rasterizes arcs at
 * whole-degree resolution: a fractional slot (270/41) rendered as a visible
 * mix of 4 and 5 degree wedges. Every segment boundary below is a whole
 * degree, so the ring is uniform. */
#define NEON_NSEG       45
/* Glow reach. The mockup's bloom was a gaussian blur composited additively,
 * which this pipeline cannot afford per frame - alpha is its most expensive
 * operation. It is approximated with concentric OPAQUE strokes of falling
 * brightness: NEON_GLOW_OUT px outside the ring and the inner band px inside
 * the body. Both feed the clip guard and the invalidation bounds, so widening
 * the glow automatically widens the dirty region. */
#define NEON_GLOW_OUT   0       /* no outward bleed: the segment is three flat bands */
#define NEON_CAP_W      6       /* white tip, the outermost of the four tube bands */
/* The inner (dimmed) band is the SAME width as the bloomed body, extending
 * inward from the body's inner edge - so the lit three-tone band is
 * symmetric about the body and its full depth is 2W - CAP. The unlit track
 * stays at the body width W with the same outer edge. One formula feeds the
 * draw, the bakes, the invalidation and the web mirror so they cannot drift. */
/* The inner (dimmed) band used to match the body width, so the lit depth was
 * 2W - CAP; the tube's halo is its own literal now (NEON_TUBE_HALO_W 20) and
 * the segments' is NEON_SEG_HALO_W 20 against a 25 px body. */
#define NEON_SEG_HALO_W     20
#define NEON_SEG_BAND_DEPTH (NEON_SEG_HALO_W + (NEON_SEG_W - NEON_CAP_W + 1) + NEON_CAP_W - 2)  /* 49 */
/* The tube's lit run is FOUR bands, one more than the segments: white cap
 * (skinny, back at NEON_R), then the TRACK split into two tones - the outer
 * half is lighter than the inner half, which keeps the current bloomed body
 * colour - and the innermost dark halo, which stays as it was. The track
 * half-width NEON_TUBE_TRACK_HALF is deliberately less than the halo
 * thickness (16 < 21), so the whole track (32) is less than twice the halo.
 * All radii are the band's OUTER edge (LVGL), abutting with the +1s. */
#define NEON_TUBE_TRACK_LIGHT 0.26f    /* outer half: HSL lightness step (keeps hue+saturation) */
/* The middle band is the bloomed accent scaled to 0.88 so the OUTER half has
 * chroma headroom. The bloom keeps hue+saturation at 1.0 and already pushes
 * the bright zone colours (yellow, magenta, cyan) near the top of the RGB
 * range, so lightening them toward the cap collapses into white - the "too
 * pale" wash. Scaling the middle down ~12% keeps it vivid while giving the
 * lighten step room to land as a clearly lighter, still-hued band. */
#define NEON_TUBE_MID_SCALE 0.88f
#define NEON_TUBE_TRACK_HALF 16        /* each track half, < NEON_TUBE_HALO_W (20) */
#define NEON_TUBE_HALO_W     20        /* innermost dark ring (was NEON_BODY_W = 21) */
#define NEON_TUBE_TRACK_OUTER_R (NEON_R - NEON_CAP_W + 1)                    /* 223 */
#define NEON_TUBE_TRACK_INNER_R (NEON_TUBE_TRACK_OUTER_R - NEON_TUBE_TRACK_HALF + 1)  /* 208 */
#define NEON_TUBE_HALO_R        (NEON_TUBE_TRACK_INNER_R - NEON_TUBE_TRACK_HALF + 1)  /* 193 */
/* Full lit depth on the tube: 228 outer to 174 inner = 55 px. The halo's
 * inner edge is haloR - haloW + 1, so depth = NEON_R - that + 1. */
#define NEON_TUBE_BAND_DEPTH (NEON_R - NEON_TUBE_HALO_R + NEON_TUBE_HALO_W)
/* Unlit track width on the tube (track + cap footprint, excluding the dark
 * halo): 228 - 193 + 1 = 36. This is the width of the unlit track bake AND
 * of the tube peak tell-tale: the peak marker must stay fully contained
 * within the track (user's 2026-08-11/12 bench requirement - it must NOT
 * bleed into the dark halo the way the white zero marker does). The zero
 * marker draws the full lit depth (NEON_TUBE_BAND_DEPTH, 55). The lit run
 * also paints the full depth. */
#define NEON_TUBE_TRACK_W (NEON_R - NEON_TUBE_HALO_R + 1)
/* The dark remainder between lit wedges. 2.0 is the ORIGINAL gap, kept
 * exactly: with the fixed NEON_SEG_PITCH below the lit wedge is 4.0 and the
 * gap 2.0, and both stay whole degrees so the ring renders uniformly. */
#define NEON_SEG_GAP    2.0f
/* Fixed segment pitch (slot = lit + gap), and the grid offset that centres
 * NSEG x PITCH within the 270-degree sweep. NEON_SEG_START is the leading
 * edge of segment 0's slot; each segment's LIT wedge is
 * NEON_SEG_START + i*PITCH + GAP/2 .. + NEON_SEG_LIT. */
#define NEON_SEG_PITCH  6.0f
#define NEON_SEG_LIT    (NEON_SEG_PITCH - NEON_SEG_GAP)                       /* 4.0 */
#define NEON_SEG_START  ((float)ARC_START + ((float)ARC_RANGE - NEON_SEG_PITCH * (float)NEON_NSEG) * 0.5f)  /* 135.0 */
static inline float neon_seg_start(int i)
{
    return NEON_SEG_START + (float)i * NEON_SEG_PITCH + NEON_SEG_GAP * 0.5f;
}
/* Tube run tiles: the continuous run is drawn as a_zero-aligned A8 wedge
 * tiles (NEON_TUBE_TILE_STEP each) plus a live arc for the partial tip, so a
 * zone flip that recolours the whole run repaints as blits instead of
 * rasterising the full run four times. Each tile is baked OVERLAP past both
 * edges and the tip arc starts the same overlap early, so adjacent AA
 * fringes always land under a neighbour's full-coverage interior and the run
 * reads as one continuous band. */
#define NEON_TUBE_TILE_STEP   (ARC_RANGE / (float)NEON_NSEG)   /* 270/45 = 6.0 (tube tiles only; not the segments' pitch) */
/* The tube tiles carry NO angular AA (they are cut from a full-ring radial
 * bake in neon_bake_tube_sprites), so they tile exactly with no overlap; the
 * live tip arc starts at the tile boundary. This constant is the tip
 * overhang and the clip-box extent and stays 0. */
#define NEON_TUBE_TILE_OVERLAP 0.0f
/* Radial-gradient arc image margin (px each side of the band). The image must
 * hold the baked band's own AA fringes (r=NEON_R + 1) plus the arc's slightly
 * larger mask, so the gradient arc is drawn at radius NEON_R + NEON_GRAD_MARGIN
 * and width NEON_BAND_DEPTH + 2*NEON_GRAD_MARGIN: the mask then clips far
 * outside the band and the image's own baked edges supply the ring's AA
 * uniformly (a bare 2*NEON_R image cut the cap's outer AA off at the axes). */
#define NEON_GRAD_MARGIN 4
/* Numerals sit INSIDE the ring. Outside would put them within ~13 px of the
 * glass on a 466 px round panel, where the bezel and the curve eat them. */
#define NEON_LABEL_R    134     /* scale numeral radius */
/* Slot/dot/sign kept close to proportional with NEON_FONT_PX, raised together
 * when the font grew 108->118 (regenerated metrics: line_height 76->83,
 * widest glyph ink 77->84). NEON_SLOT_W is the cell pitch - it must clear the
 * widest glyph's ink width (84) or adjacent opaque digit sprites overlap and
 * paint over each other; 88 leaves a 4 px margin above that floor. SIGN_W
 * is 42 rather than the exactly-proportional 43: at 43 a specific psi
 * trajectory through the audit's 20 s sweep produced a 1-step antialiasing
 * seam against the ring - purely a sub-pixel rounding coincidence at that
 * exact sign width, gone at 42 or 44, kept at 42 for the extra clearance
 * margin. */
#define NEON_SLOT_W     88
/* The decimal's cell, and the readout's main spacing lever.
 *
 * It was 45 for a glyph whose ink is only 22 px wide - 23 px of padding, more
 * than the digit cells carry in proportion. That was not a design choice: with
 * opaque tiles the decimal's neighbours had to be held far enough away that
 * their black margins could not reach its ink, and the decimal, sitting
 * between two of the widest glyphs on the face, was the most boxed-in cell of
 * all. It is also where the eye reads the readout's spacing from, since
 * "8.5" has no two digits adjacent to each other - so that padding is exactly
 * what made the readout look too widely spaced.
 *
 * A8 coverage removed the constraint, so this is now set from ink: 34 leaves
 * 6 px either side of a 22 px glyph. The digit cells are NOT tightened to
 * match - at 88 pitch two of the widest digits (ink 84) already clear each
 * other by only 4 px, so the digits are at their ink limit and the decimal
 * was the only slack left. */
#define NEON_DOT_W      34
#define NEON_SIGN_W     42
/* The negative readout (-12.0) is the widest the face ever gets. Measured on
 * the rendered screenshot at 118 px: readout ink reaches ~170 px against the
 * segments' 179 px inner edge (NEON_R 228 minus NEON_SEG_BAND_DEPTH 49),
 * clearing by ~9 px. */
#define NEON_FONT_PX    118
/* Half the slack the draw box adds around each cell. The box must be at least
 * the glyph's ADVANCE wide, because lv_draw_label centres on the advance and a
 * narrower box clips: 0.780 em is the widest advance in this face, so 84 px at
 * 108 and 120 px at 154. 18 gives boxes of 100 and 128, clearing both. It was
 * 30, which made a 124 px box for an 84 px glyph - and the readout is 64% of
 * segments' dirty area per cycle and 83% of marquee's, so that slack was the
 * single largest avoidable cost on the face. */
#define NEON_CELL_BLEED 18
#define NEON_SIGN_GAP   8       /* from the glyph's ink edge, not the cell's */
/* The minus mark's glow is NOT a constant here any more. It used to be a
 * second pass of the bar geometry inflated by a fixed 5 px, which at a 6 px
 * bar height on an 8 px pitch overlapped the two bars and filled the gap
 * between them solid. The mark is now baked through the same box blur as the
 * digits (neon_bake_sign_sprite()), so its reach is NEON_SPR_GLOW_MARGIN like
 * every other sprite's, and the two match without tuning. */
/* Marquee shares the ring layouts' readout metrics (NEON_FONT_PX /
 * NEON_SLOT_W / NEON_DOT_W / NEON_SIGN_W / NEON_SIGN_GAP / NEON_READOUT_TOP)
 * and the same lower-stack rhythm - it used to carry its own 130 px font and
 * wider cells, but the three-ring border needs the room that oversized
 * readout occupied, and one sprite set serving every layout is cheaper in
 * PSRAM and in scene-switch time than two. */
/* Top of the readout's draw box, per layout. lv_draw_label() draws from the TOP
 * of its area, so this - not a centre - is what positions the digits, and it is
 * also what the sign's vertical centre is derived from. Each is set so
 * top + line_height/2 keeps the same vertical anchor the old font used
 * (-1px off face centre for segments/tube, -26px for marquee) now that
 * line_height changed. */
#define NEON_READOUT_TOP     (-42)
/* One vertical rhythm for the lower stack, used by every layout: unit mark,
 * hairline rule and peak readout at fixed offsets from the centre. Marquee
 * shifts the whole block down as a unit rather than re-spacing it, so the three
 * layouts read identically - it needs the shift because its bar occupies the
 * space the other two leave empty above the unit mark.
 *
 * Moved down 40px as a unit (was 96/106/126) - the ring layouts had 55px+ of
 * empty panel below the old peak line before hitting the bezel, so there was
 * plenty of room to drop the whole assembly a good bit while preserving the
 * internal spacing (unit->rule 10px, rule->peak 20px). */
#define NEON_UNIT_Y     128
#define NEON_RULE_Y     138
#define NEON_PEAK_Y     158
/* The lower stack (unit mark, rule, peak) sits at one rhythm on every
 * layout, including marquee - the marquee-specific downward shift went away
 * with the oversized readout, and the bar at NEON_BAR_Y clears the unit mark
 * with the shared offsets. The whole assembly was lifted 8 px (136->128 /
 * 146->138 / 166->158) so the peak reads comfortably clear of the marquee's
 * inner ring. */
/* Marquee's linear bar, shared by the bake, the draw and the invalidation so
 * they cannot drift apart. Shortened from 150 so it reads as a ruler under
 * the digits rather than extending past them. */
#define NEON_BAR_HALF   132
#define NEON_BAR_Y      68
#define NEON_BAR_H      16
/* Zero mark on the bar, the linear equivalent of the ring's white zero segment.
 * Drawn live rather than baked because the fill passes over it and has to leave
 * it visible. Shorter and wider than the first pass (4x36): 7x27 reads as a
 * stout capsule marker instead of a thin sliver, closer in proportion to the
 * bar it marks (NEON_BAR_H 16). Capsule ends unchanged (LV_RADIUS_CIRCLE). */
#define NEON_BAR_TICK_W 7
#define NEON_BAR_TICK_H 27
/* Half-width, in degrees, of the tube layout's white zero marker.
 *
 * This is drawn TWICE and both have to use it. The marker is baked into the
 * static background so it survives with nothing lit, but the live tube run
 * starts exactly at a_zero and sweeps outward at the marker's own radius and
 * width - so it paints over whichever half of the marker lies on the value's
 * side. That left only half the marker visible (2 degrees of the 4 it was
 * baked at), and, because the buried half swaps sides at the zero crossing,
 * the surviving half appeared to JUMP sideways between vacuum and boost. That
 * is the shift on the panel, and it is also why widening the bake alone never
 * looked any wider. The fix is to redraw the marker live after the run, which
 * costs one arc per frame in this layout only. 3 gave a 6 degree marker; 2.25
 * makes it 25% narrower (4.5 degrees) as a visual tweak.
 *
 * The arc rasterizer does not paint an angle span exactly symmetric about its
 * midpoint: lv_draw_arc's trig-table quantises each edge to the nearest table
 * entry. Measured on the sim's atmo render, an uncompensated [zero-2.55,
 * zero+2.55] marker paints its centroid at 235.495 while the run that sweeps
 * out of the notch starts at ~236.00 (both ~0.25-0.75 deg shy of the nominal
 * 236.25) - so the marker read ~0.5 deg toward the vacuum side of the visible
 * zero. NEON_TUBE_ZERO_CENTER (+0.25 deg) shifts the drawn span so the
 * painted centroid lands on the run start; re-measured after the fix at
 * 235.981 vs the run start 236.00. The web mirror does NOT apply it: canvas
 * arcs are symmetric, so its marker is already centred on the true zero. */
#define NEON_TUBE_ZERO_DEG    2.25f
#define NEON_TUBE_ZERO_CENTER 0.25f
/* Half-width, in degrees, of the tube layout's peak tell-tale. The marker is
 * drawn from the CONTINUOUS peak angle - no segment snapping - and centred
 * exactly on it, so no NEON_TUBE_ZERO_CENTER-style compensation is needed
 * (that offset exists because the RUN that sweeps out of the notch starts at
 * a quantised edge; the peak marker has no anchored neighbour). The old
 * segment-quantised marker sat up to a full 6-degree pitch off the actual
 * measurement, which is what read as the tube never reaching the marked
 * point. Half-width 2.25 matches the white zero marker's NEON_TUBE_ZERO_DEG
 * exactly, so the two landmarks have the same TOTAL angular width (4.5 deg)
 * - the user's definition of the peak marker's "width" is how many degrees
 * it extends left/right of its centre line, and the 2026-08-12 bench
 * comparison against the 0 marker was about that angular span, not the
 * radial depth. The marker's RADIAL depth stays NEON_TUBE_TRACK_W (36,
 * contained in the track); only the angular span matches the zero marker. At
 * psiMax the dial clamp cuts the drawn band in half, the same natural
 * cut-off the user accepted at the end of the track. */
#define NEON_TUBE_PEAK_DEG 2.25f
/* Invalidation padding around the exact arc sector bbox: the tube's gradient
 * mask runs 4 px inside the geometric inner edge (NEON_GRAD_MARGIN) plus ~2 px
 * arc-mask AA fringe plus 2 px margin. See the use site for the full argument. */
#define NEON_INV_PAD 7
/* Bulb border. Three concentric rings, one zone each - innermost vacuum,
 * middle boost, outermost overboost. The dots are two-tone: most bulbs are
 * baked dead track, and every ring carries 24 ACCENT bulbs (every sixth pair)
 * that LIGHT UP LIVE - ring z's accents are coloured with that ring's OWN
 * zone colour only once the reading has REACHED that zone (zone id >= z). At
 * rest only the innermost ring shows colour; rev into boost and the middle
 * ring lights; hit overboost and all three are lit. So the border reads as a
 * stage ladder while staying two-tone at idle.
 *
 * The accent phase is ANCHORED per ring (NEON_BULB_ACCENT_OFFSET(z)): the
 * inner ring keeps its pairs at 0,1 (top centre), the OUTER ring is shifted
 * two bulbs on so its pairs sit OFF the vertical axis (flanking top and
 * bottom rather than straddling them) - the same pattern the inner ring
 * shows, as close as the different bulb counts allow. The middle ring's
 * pairs sit at the bottom centre - bulb N/2 is exactly at 6 o'clock and
 * carries the pair's FIRST residue, with its partner one dot
 * counterclockwise, so the pair leans left. The outer ring reads as one
 * ladder with the inner, and the middle ring's bottom pair marks the
 * boost stage.
 *
 * Invalidation on a zone flip is bounded: only the ACCENT bulbs (24 per
 * ring, 12 adjacent pairs) change colour, so one ring costs 12 pair-boxes and
 * even a two-zone jump (both rings 1 and 2) stays at 24 boxes - under LVGL's
 * 32-slot invalidation buffer. The dead track bulbs never change.
 *
 * NEON_BULB_R stays the outermost ring's centreline, so every old silhouette
 * measurement (tips at 228, margin to the bezel) still holds for it. The
 * 24 px centreline step (1.5x the 16 px the earlier spread used; 32 is the
 * 2x option, one constant away) is 9 px bulb + 15 px dark gap, wide enough
 * that the three rings read as three rings at arm's length. Zone-to-ring
 * mapping is 0 = innermost (vacuum) .. 2 = outermost (overboost), which is
 * why the ring index and the zone id are the same number. */
#define NEON_BULB_RINGS   3
#define NEON_BULB_RING_STEP 24
#define NEON_BULB_RING_R(z) (NEON_BULB_R - ((2 - (z)) * NEON_BULB_RING_STEP))
#define NEON_BULB_R     224
/* Bulb count PER RING. All three must stay divisible by 6 so the 2-lit/
 * 4-dark accent pattern wraps seamlessly and the invalidation groups of 6
 * stay whole. The counts are chosen so the CHORD spacing (2*pi*r/N) is
 * uniform across rings: outer 72 (chord 19.55), middle 66 (19.04), inner
 * 54 (20.48) - versus the old equal-72 counts, whose inner ring was 21%
 * tighter than the outer (15.36 vs 19.55). Multiples of 6 that bracket
 * the exact spacing are 54/66/72; the residual spread is under a pixel. */
#define NEON_BULB_N_INNER 54
#define NEON_BULB_N_MID   66
#define NEON_BULB_N_OUTER 72
#define NEON_BULB_N(z) ((z) == 0 ? NEON_BULB_N_INNER \
                       : (z) == 1 ? NEON_BULB_N_MID : NEON_BULB_N_OUTER)
#define NEON_BULB_HALF  4
/* Static accent anchor per ring: (i + offset) % 6 < 2. Ring 0 keeps offset 0
 * -> pairs at 0,1 (top centre, bulb 0 at 12 o'clock). Ring 2 is shifted two
 * bulbs on -> offset 2, so its pairs land at 2,3 and sit OFF the vertical
 * axis, flanking the top/bottom rather than straddling it - the same pattern
 * the inner ring shows, as close as the different bulb counts allow. Ring 1
 * uses offset 3 -> pairs at 3,4, and with NEON_BULB_N_MID = 66 its pair
 * (33,34) sits at the bottom centre (bulb 33 is exactly at 6 o'clock,
 * partner one dot counterclockwise). The optional marquee chase adds a
 * per-ring phase that advances one ring per spin tick (see s_neon_spin_phase
 * below) - the pattern repeats every 6 bulbs, so a ring has exactly 6 phase
 * states and one advance moves every accent pair by a single bulb. */
#define NEON_BULB_ACCENT_OFFSET(z) ((z) == 1 ? 3 : ((z) == 2 ? 2 : 0))
#define NEON_BULB_IS_ACCENT(i, z) (((((i) + NEON_BULB_ACCENT_OFFSET(z) + s_neon_spin_phase[z]) % 6) + 6) % 6 < 2)

/* Marquee border chase (neonMarqueeSpin, persisted in the theme store). One
 * ring advances per tick, round-robin, so a spin step costs the SAME 12
 * pair-boxes as a zone flip and never more than one ring's worth - advancing
 * all three at once would need 36 boxes and overflow LVGL's 32-slot
 * invalidation buffer, forcing a full-screen repaint. Ring z advances on
 * ticks where tick % 3 == z, so all three rotate at the same angular speed
 * (one ring per 3 ticks), just offset in time. The pattern period is 6, so
 * each ring has 6 phase states - one advance = one bulb of motion. Direction
 * per ring: inner and outer clockwise, middle counterclockwise. */
#define NEON_MARQUEE_SPIN_MS   90
#define NEON_SPIN_DIR_INNER    (-1)  /* clockwise  */
#define NEON_SPIN_DIR_MID      (1)   /* counterclockwise */
#define NEON_SPIN_DIR_OUTER    (-1)  /* clockwise  */
static int8_t s_neon_spin_phase[NEON_BULB_RINGS];
static uint8_t s_neon_spin_tick;      /* round-robin: ring (tick % 3) advances */
static uint32_t s_neon_spin_last_ms;  /* lv_tick_get() at the last advance   */
static const int8_t s_neon_spin_dir[NEON_BULB_RINGS] = {
    NEON_SPIN_DIR_INNER, NEON_SPIN_DIR_MID, NEON_SPIN_DIR_OUTER,
};

/* Committed marquee bar fill extent: pixel x of the zero end and of the
 * value end, as drawn. The bar invalidation is gated on these (plus a zone
 * colour flip) instead of firing on every 16 ms sample - when neither end
 * moved by a full pixel and the zone colour is unchanged, the painted bar is
 * pixel-identical, so repainting would only flush the same box at 62.5 Hz
 * forever. That is what lets the marquee go idle at a static reading like
 * every other face. Reset to impossible sentinels at scene build so the
 * first update after a rebuild always repaints. */
static int s_neon_bar_lo = INT_MIN;
static int s_neon_bar_hi = INT_MAX;

/* Precomputed bulb centre offsets, shared by the bake, the live draw and both
 * invalidation paths. The old per-call neon_bulb_pos() ran two trig calls and
 * a rounding per bulb, and the live draw plus every spin/zone-flip callback
 * invocation scans every accent bulb - so that cost repeated once per dirty
 * region. The offsets depend only on the ring constants, so they are computed
 * once and the per-bulb call becomes one table load and an integer centre
 * add. Bit identical: same lroundf(cosf/sinf(rad) * r), same centre. The
 * array is sized to the OUTER ring's count (the largest) and only the used
 * prefix of each ring is filled. */
static int16_t s_neon_bulb_ox[NEON_BULB_RINGS][NEON_BULB_N_OUTER];
static int16_t s_neon_bulb_oy[NEON_BULB_RINGS][NEON_BULB_N_OUTER];
static bool s_neon_bulb_table_ready;

/* Residue where ring z's accent pair starts at its CURRENT spin phase:
 * (i + NEON_BULB_ACCENT_OFFSET(z) + phase) % 6 < 2 lights residues base and
 * base+1. Shared by the live draw (via NEON_BULB_IS_ACCENT) and BOTH
 * invalidation paths (zone flip and spin step) so they cannot disagree about
 * where the accents are. */
static int neon_accent_base(int z)
{
    return ((6 - NEON_BULB_ACCENT_OFFSET(z) - s_neon_spin_phase[z]) % 6 + 6) % 6;
}

/* The marquee's centre draws the SHARED 118 px readout sprites at 0.87 scale
 * so the composition (readout, bar, stack) fits inside the spread rings: the
 * innermost ring's inner edge is 224 - 2*STEP - 4 = 172, and the full-size
 * readout ink+glow reaches ~184, so the whole centre scales about the face
 * centre by this factor. One sprite set, drawn smaller - not a second font. */
#define NEON_MARQUEE_CENTER_SCALE 0.87f
/* Extra lift for the marquee readout only, on top of the center scale - the
 * scale pulls everything toward the face centre, so without it the readout
 * would sit LOWER at 0.87 than it did at 0.91. Lifting it keeps the bar and
 * the peak/psi stack below with more room to breathe. */
#define NEON_MARQUEE_READOUT_LIFT 12

static void *s_neon_bg_buf;
/* Everything paint_neon_background() reads, so a repaint happens exactly when
 * one of them changes and not on every scene build. Mirrors vault_bg_key_t. */
typedef struct {
    uint8_t layout;
    uint32_t track;
    uint32_t zero;
    /* Where zero lands on the sweep, which is the ONLY way the psi settings
     * reach the background - it places the white zero segment and nothing
     * else. One number instead of psiMin/psiMax/zeroAngle separately, and it
     * cannot disagree with what the paint actually computes because it is the
     * same call. */
    float zero_sweep;
} neon_bg_key_t;
static neon_bg_key_t s_neon_bg_key;
static bool s_neon_bg_key_valid;
static bool s_neon_bg_reused;
static lv_obj_t *s_neon_bg;
static lv_obj_t *s_neon_face;
static lv_obj_t *s_neon_zone;
static lv_obj_t *s_neon_unit;
static lv_obj_t *s_neon_peak;
static float s_neon_psi;          /* committed value the art is drawn from */
static float s_neon_peak_value;
/* The value the committed *colour* came from. Tracked separately because the
 * zone colour changes at thresholds, not continuously: when it flips, every
 * lit segment restyles and the whole run has to be repainted, not just the
 * segments the value moved across. Mirrors s_arc_color_psi. */
static float s_neon_color_psi;
/* Colour-flip deferral (word-first, arc-next-frame): the zone word updates on
 * the flip frame (cheap, now that its invalidation box is cropped to the word
 * sprite and no longer reaches the ring), and the full-run recolor is deferred
 * to the next sample. The flip frame therefore carries the word + readout +
 * peak but NOT the arc repaint, so its dirty region is smaller. DELIBERATE
 * one-frame visual lag (the ring shows the old zone colour for one frame after
 * the word flips) - NOT byte-identical, so the sim stale-pixel audit reports
 * those transition frames as mismatches by design. */
bool g_neon_flip_pending;
static float s_neon_flip_lo, s_neon_flip_hi;
/* Segment index currently carrying the peak tell-tale on the SEGMENTS layout,
 * -1 for none. The peak sits outside the run between the notch and the value,
 * so it is invalidated on its own when it moves. The tube layout tracks its
 * own continuous marker state (s_neon_tube_peak_angle) instead - see below. */
static int s_neon_peak_idx = -1;
/* Tube layout: the peak tell-tale is a continuous arc centred exactly on the
 * peak's sweep angle (no segment snapping), always drawn while a peak is
 * recorded. These two hold where it was last drawn so update_neon() can
 * invalidate exactly the old/new spans when it moves. */
static bool s_neon_tube_peak_vis;
static float s_neon_tube_peak_angle = NAN;
/* Whether the lit run covered the peak segment last frame (segments layout
 * only). The overlay is suppressed while it does, so this transition changes
 * the segment's appearance without changing its index - the one case a
 * "did the index move?" test would miss. The tube never suppresses: its
 * marker is a permanent landmark in a fixed dim colour, and popping it in
 * and out as the run crossed a segment boundary was the flicker at the
 * peak. */
static bool s_neon_peak_in_run;
/* Last readout composition actually painted. The tenths cell changes every
 * sample while the higher cells almost never do, so repainting the whole
 * 350x160 readout box each frame cost more than the entire ring. */
static char s_neon_cell_ch[BOOST_NEON_MAX_CELLS];
static int16_t s_neon_cell_x[BOOST_NEON_MAX_CELLS];
static uint8_t s_neon_cell_n;
static bool s_neon_sign_drawn;
static int16_t s_neon_sign_x;   /* where the sign was last painted */
static boost_neon_layout_t s_neon_layout;

/* Scale one centre-geometry value about the face centre on the marquee
 * layout only (identity elsewhere). Every readout/bar/stack/word constant
 * that positions the centre composition goes through this so the spread
 * rings and the centre agree about how much room there is. */
static inline int neon_mq(int v)
{
    return (s_neon_layout == BOOST_NEON_MARQUEE)
        ? (int)lroundf((float)v * NEON_MARQUEE_CENTER_SCALE)
        : v;
}

/* The marquee readout's vertical offset from centre: the scaled NEON_READOUT_TOP
 * plus the lift. The centre scale pulls everything toward the face centre, so
 * at 0.87 the readout would sit LOWER than it did at 0.91 without this - the
 * lift keeps it up so the bar and the peak/psi stack below have room to
 * breathe. Shared by the draw and the invalidation so they cannot disagree. */
static int neon_readout_top_off(void)
{
    return neon_mq(NEON_READOUT_TOP) - (s_neon_layout == BOOST_NEON_MARQUEE
        ? NEON_MARQUEE_READOUT_LIFT : 0);
}

#if BOOST_NEON_GLYPH_SPRITES
/* One PSRAM block holding every baked glyph, for every zone colour, at the
 * one font size every layout now shares (NEON_BIG, 118 px): 112x111 per
 * tile - the font's own measured union ink box (84x83, read straight off
 * the glyph_dsc tables) plus a 14 px glow margin on every side so the
 * NEON_GLOW_BLUR_R glow has room to read before the tile edge clips it.
 * The width covers the union of every glyph centred on its own ADVANCE
 * (advances differ: '3' leans to -37.9, '5' to +55.2 = 93.1 px) plus
 * margin, not the widest single ink. 12 glyphs (0-9, '.', '-') x 3 zones.
 * '-' is never blitted - the sign mark stays the existing triangle
 * geometry - but is baked anyway to keep one uniform glyph table. Marquee
 * used to carry its own 130 px font and sprite set; unifying the readout
 * across layouts removed it, saving ~1.16 MB of PSRAM and the scene-switch
 * rebake. PSRAM cost at this margin, measured via BOOST_NEON_DRAW_STATS
 * logging in neon_bake_glyph_sprites(): ~874 KB - comfortably under the
 * 1.6 MB/font budget against 8 MB of PSRAM total. */
#define NEON_SPR_GLOW_MARGIN 14
#define NEON_SPR_BIG_W  112
#define NEON_SPR_BIG_H  111
/* 12 digits/punctuation plus one slot for the minus mark, which is drawn from
 * baked bar geometry rather than from a font glyph (this typeface's '-' has no
 * contour) - see neon_bake_sign_sprite(). */
#define NEON_GLYPH_COUNT 12
#define NEON_SIGN_SLOT   NEON_GLYPH_COUNT
#define NEON_SPRITE_COUNT (NEON_GLYPH_COUNT + 1)
/* Glow strength. Gain was 60% of core coverage; pushed to 90% so the halo
 * reads as bright neon spill instead of a faint rim. The CORE ink itself is
 * unaffected: cov = max(core, glow), and core coverage saturates at 255
 * inside the glyph's own solid ink, so the interior stays exactly
 * neon_lit()'s colour regardless of this gain - only pixels OUTSIDE the raw
 * glyph (where core is 0 or partial) can be lifted by the glow term. */
#define NEON_GLOW_GAIN_PCT 115
/* Box-blur radius for the glow, applied twice (see neon_bake_glyph_sprites).
 * Was 3 (effective spread ~2*3=6 px into the margin); 5 spreads further
 * (~10 px) for a visibly bigger halo, still inside the 14 px margin above
 * with room to spare. */
#define NEON_GLOW_BLUR_R 5
static const char k_neon_glyph_chars[NEON_GLYPH_COUNT] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '-'
};
/* ONE coverage plane per glyph, shared by every zone.
 *
 * These were opaque RGB565 tiles, baked once per zone colour. That could not
 * work: an opaque tile paints its own black margin, so two neighbouring cells
 * could not overlap, and at the readout's cell pitch they always do - the
 * later blit erased the earlier glyph's glow and, for the widest digit pairs,
 * several columns of its actual ink. Every workaround pushed the same lever:
 * widen the cells until the black margins stopped colliding. That is what made
 * the readout too wide, and it is why the decimal had to have its glow margin
 * cropped to zero, which is what cut its halo off square at the top and sides.
 *
 * A8 removes the cause rather than trading against it. LVGL blends an
 * untransformed A8 image as a solid colour through this plane as a MASK, so
 * zero coverage contributes nothing and overlapping glows composite the way
 * light actually does. Three consequences follow: cells can be packed as
 * tightly as the ink allows, the decimal keeps a full glow margin, and the
 * plane is colour-independent - so the zone dimension disappears (~6x less
 * PSRAM) and the ink colour becomes a per-blit recolor that tracks the live
 * accent exactly instead of a value frozen at bake time. */
static void *s_neon_glyph_buf;
static lv_image_dsc_t s_neon_glyph_img[NEON_SPRITE_COUNT];
static bool s_neon_glyph_sprites_ready;
/* Memoisation, the same trick s_vault_bg_buf uses for its own expensive static
 * art: keep the baked tiles across scene switches and rebake only when an
 * input to them changes. Measured on the board, switching to neon cost ~350 ms
 * against 45-100 ms for every other theme, which is the delay that made it
 * feel like the slow one.
 *
 * The cache key is just the LAYOUT, which it can be only because the tiles are
 * A8 coverage: they carry no colour, so a palette edit, a preset change or a
 * zone flip cannot invalidate them. Layout is the one input that matters,
 * because it selects the font. Switching away to another theme and back is
 * therefore free. */
static boost_neon_layout_t s_neon_sprite_layout = (boost_neon_layout_t)0xFF;
static bool s_neon_sprites_reused;
static int s_neon_spr_w, s_neon_spr_h;
static uint32_t s_neon_spr_stride;
/* Screen-space offset from a cell's live draw anchor (cx + cell.x, top) to
 * the sprite's top-left corner. Computed once at bake time from the same
 * centred layout lv_draw_label() already used, so a sprite blit lands on the
 * identical pixels a label draw would, plus the baked-in glow. */
static int s_neon_spr_dx, s_neon_spr_dy;
/* Per-glyph tight bbox (own ink + glow margin, NOT the full uniform tile),
 * in tile-local coordinates, shared by all 3 zones since it is pure
 * geometry. The full tile is sized to the WIDEST glyph so every character
 * shares one anchor; blitting that full tile for every cell would paint each
 * glyph's black margin over its neighbour whenever the cell pitch (as little
 * as 33 px for a '.' cell) is narrower than the tile (93/125 px) - which it
 * always is. Blitting only this tight bbox keeps each glyph's opaque paint
 * inside its own actual content, matching what the alpha-blended label draw
 * already painted (nothing outside the glyph's own ink/glow). */
static int16_t s_neon_glyph_bbox[NEON_SPRITE_COUNT][4]; /* x0,y0,x1,y1 inclusive */

/* Marquee-only pre-scaled copies of the tiles above. The shared 118 px set is
 * drawn at NEON_MARQUEE_CENTER_SCALE through LVGL's per-frame transform, which
 * host A/B measured at ~35-40% of every readout repaint (plain blit p50
 * 0.09 ms vs transform p50 0.14 ms on the same dirty regions, max 0.45 vs
 * 2.15 ms). So the marquee bake produces a second 0.87-size A8 set ONCE per
 * scene build and the live blits become plain stride-copy blends. The scaled
 * pixels are captured by running the SAME lv_draw_image transform into a
 * scratch canvas, so they are identical to what the per-frame transform used
 * to paint; each slot's bbox_s is the tile's screen-space offset from the
 * blit anchor (anchor + bbox_s), pre-baked so the blit carries no transform
 * machinery. NULL set = allocation failed, keep the per-frame transform. */
static void *s_neon_glyph_buf_s;
static lv_image_dsc_t s_neon_glyph_img_s[NEON_SPRITE_COUNT];
static int16_t s_neon_glyph_bbox_s[NEON_SPRITE_COUNT][4];

/* Segments layout only: colour-independent A8 coverage for each segment's
 * three lit bands (halo/body/cap), baked at scene build so a changed segment
 * repaints as three recolored blits instead of three lv_draw_arc primitives.
 * Each tile is cropped to that band's actual painted extent (ink bbox of the
 * arc rendered in white, plus its AA fringe) and positioned by the offset
 * from the face centre to the tile's top-left corner - so a blit at
 * (px_icx() + off_x, px_icy() + off_y) lands on the exact pixels the arc
 * rasterized, at any burn-in shift. Coverage carries no colour, so the ink is
 * a per-blit recolor like the glyph tiles. A NULL data pointer means that
 * band has no tile and the segment falls back to the live arcs.
 *
 * The buffer is a sibling of the glyph cache and shares its lifecycle:
 * freed in neon_free_glyph_sprites() (which runs only from the scene-build
 * path, after destroy_scene() has drained draw units), so it outlives every
 * draw callback that references it. */
typedef struct {
    lv_image_dsc_t img;
    int16_t off_x, off_y;
} neon_seg_band_tile_t;
static neon_seg_band_tile_t s_neon_seg_tile[NEON_NSEG][3];
static void *s_neon_seg_buf;
static bool s_neon_seg_sprites_ready;
/* Tube layout only, same shape as the segment tiles: a_zero-aligned wedges,
 * direction 0 counterclockwise (boost side), 1 clockwise (vacuum side). */
#define NEON_TUBE_TILES NEON_NSEG
static neon_seg_band_tile_t s_neon_tube_tile[2][NEON_TUBE_TILES][4];
static void *s_neon_tube_buf;
static bool s_neon_tube_sprites_ready;
static lv_area_t s_neon_tube_box[2][NEON_TUBE_TILES];
/* Tube layout only: one radial-gradient image PER ZONE COLOUR, drawn by a
 * single lv_draw_arc with img_src instead of the three live band arcs. The
 * image is a square of the three abutting bands (dim, body, white cap) baked
 * from the exact same arc geometry as the live arcs, so the zone-flip
 * recolor - which re-rasterises the whole run - costs one arc mask pass
 * instead of three (~3x less raster), and every normal tube frame costs one
 * arc too. ALL THREE zone images are baked at scene build so a zone flip
 * only SWITCHES the image - rebuilding on the flip frame would re-pay the
 * three full-ring rasterisations that this feature exists to avoid. */
#define NEON_GRAD_ZONES 3
static void *s_neon_grad_buf[NEON_GRAD_ZONES];
static lv_image_dsc_t s_neon_grad_img[NEON_GRAD_ZONES];
static uint32_t s_neon_grad_key[NEON_GRAD_ZONES];
/* Window padding around the band bbox when rendering the arc for extraction:
 * the rasteriser can paint its AA fringe a pixel or so outside
 * lv_draw_arc_get_area()'s trig-table bound, and the crop is taken from the
 * ACTUAL painted ink, so this only has to be generous enough that nothing
 * reaches the scratch edge. */
#define NEON_SEG_SPR_MARGIN 6
/* Clip-skip box inflation: the arcs' AA fringe paints ~1-2 px past the wedge
 * and the end corner reaches sin(step)*(R-rin) beyond the geometric bbox
 * (5 px for the 54-deep band). 8 px covers both so a segment whose fringe
 * falls inside a dirty region is submitted and repaints it. */
#define NEON_BAND_SPR_MARGIN 8
/* The segments' three bands' geometry, one source for the bake and the blit
 * path so they cannot drift from each other (or from the constants the
 * fallback arcs still spell out inline). Index order inner, body, cap - the
 * draw order. The inner halo (NEON_SEG_HALO_W 20) is narrower than the body
 * (25) - the user asked for it shorter than the middle band - so the bands
 * abut 20:25:6. The tube has its own four-band table below. */
typedef struct { int radius; int width; } neon_seg_band_geom_t;
static const neon_seg_band_geom_t k_neon_seg_band_geom[3] = {
    { NEON_R - NEON_SEG_W + 1, NEON_SEG_HALO_W },          /* inner (halo, shorter than body) */
    { NEON_R - NEON_CAP_W + 1, NEON_SEG_W - NEON_CAP_W + 1 },    /* body  */
    { NEON_R, NEON_CAP_W },                                      /* cap   */
};
/* The tube's four bands' geometry, one source for the bake, the gradient
 * image and the blit path so they cannot drift from each other (or from the
 * constants the fallback arcs still spell out inline). Index order inner to
 * outer - the draw order. The halo stays NEON_TUBE_HALO_W, the track is split
 * into two equal halves, and the cap is skinny at NEON_R again. */
static const neon_seg_band_geom_t k_neon_tube_band_geom[4] = {
    { NEON_TUBE_HALO_R, NEON_TUBE_HALO_W },                 /* inner dark halo */
    { NEON_TUBE_TRACK_INNER_R, NEON_TUBE_TRACK_HALF },      /* track inner half */
    { NEON_TUBE_TRACK_OUTER_R, NEON_TUBE_TRACK_HALF },      /* track outer half */
    { NEON_R, NEON_CAP_W },                                 /* cap (skinny)    */
};

/* The zone word (VACUUM / BOOST / OVERBOOST) gets the same treatment.
 *
 * It was a plain lv_label, so it was the one lit element on the face with no
 * glow at all - flat next to a readout that blooms. It cannot simply be given
 * one: LVGL has no text glow, and stamping the label at offsets reads as
 * fringing. Baking it through the glyphs' own blur is the same trick, and at
 * this size it is nearly free (three words, one byte per pixel, ~69 KB).
 *
 * Drawn from the face's draw callback rather than as a child object, because
 * a glow has to composite against the face beneath it. That means this code
 * owns its own invalidation on a zone flip - see update_neon(). Indexed by
 * neon_zone_id(). */
#define NEON_WORD_COUNT 3
#define NEON_WORD_W 320
#define NEON_WORD_H 72
/* The layout box the word is centred in, unchanged from the lv_label it
 * replaced - the bake records its crop offsets against this box, so the two
 * have to agree. */
#define NEON_WORD_BOX_W 300
#define NEON_WORD_BOX_H 34
/* Centre of the zone word, off face centre. One value for every layout now:
 * marquee shares the ring layouts' readout metrics, so the word sits at the
 * same height on all three faces. Upward clearance is to the ring's inner
 * glow edge at radius 174 on the ring layouts, and to the innermost bulb
 * ring's inner edge (188) on marquee - "OVERBOOST" is the widest word at
 * ~200 px of ink, so its top corners sit sqrt(100^2 + 112^2) = 150 from
 * centre, ~24 px clear either way. */
#define NEON_WORD_Y (-95)
static const char *const k_neon_zone_words[NEON_WORD_COUNT] = {
    "VACUUM", "BOOST", "OVERBOOST"
};
static void *s_neon_word_buf;
static lv_image_dsc_t s_neon_word_img[NEON_WORD_COUNT];
/* Offset from the word box's own left/top corner to the tile's, per word -
 * each word is cropped to its own ink, so unlike the glyphs they do not share
 * one anchor. */
static int16_t s_neon_word_dx[NEON_WORD_COUNT], s_neon_word_dy[NEON_WORD_COUNT];
static bool s_neon_words_ready;
/* Which word was last painted, so a zone flip can invalidate exactly once. */
static int s_neon_word_drawn = -1;

static int neon_glyph_index(char ch)
{
    for (int i = 0; i < NEON_GLYPH_COUNT; ++i) {
        if (k_neon_glyph_chars[i] == ch) return i;
    }
    return -1;
}

/* Every glyph gets the full margin, the decimal included.
 *
 * The decimal used to be forced to 0 because its tile is the one most boxed in
 * by its neighbours, and while the tiles were opaque a margin there meant its
 * black surround erased the adjacent digits' ink. But cropping the margin does
 * not remove the glow the bake already blurred INTO it - it just cuts that
 * glow off square at the ink bbox, which is the hard edge visible along the
 * decimal's top and sides. With A8 coverage there is nothing to erase, so the
 * margin is uniform and the falloff simply runs out on its own. */
static inline int neon_glyph_glow_margin(int gi)
{
    (void)gi;
    return NEON_SPR_GLOW_MARGIN;
}
#endif /* BOOST_NEON_GLYPH_SPRITES */

static void paint_neon_background(lv_obj_t *canvas, const boost_theme_t *theme);
static void neon_inv_span(float a0, float a1);
static int neon_seg_index(float angle);
static void neon_inv_seg(int index);
static void draw_neon_live(lv_event_t *e);
static void build_neon(lv_obj_t *scr);
static void update_neon(const boost_sample_t *sample, const boost_theme_t *theme);
static lv_color_t c(uint32_t rgb);
static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t);
static uint32_t neon_hsl_lighten(uint32_t rgb, float d_light);
static float clampf(float v, float lo, float hi);
static uint32_t scale_rgb(uint32_t rgb, float k);
/* The approved look is the mockup's BLOOMED appearance, not the raw palette
 * value: an additive gaussian lifts a #8B3DFF segment to about (255,113,255).
 * Everything lit on this face goes through here, so the ring, the readout and
 * the zone word bloom by the same amount. The palette entry stays the base the
 * bloom is derived from, so the colour editor still means what it says. */
/* Gain measured off the mockups, not guessed: sampling the readout ink for all
 * three zones gives 1.64, 1.63 and 1.67 on every channel that is not already
 * saturated, with saturated channels staying at 255 and zero channels at 0.
 * One multiplicative gain with clamping therefore reproduces the whole set.
 *
 * Clamped locally rather than via scale_rgb(), which does NOT clamp: at gain
 * 1.65 a channel already at 255 computes to 421 and the shift-OR folds it back
 * to 165, so boost and overboost came out darker instead of brighter. */
/* 1.65 reproduced the mockup's measured ink exactly, but the mockup also had a
 * gaussian bloom bleeding light between the segments, which this pipeline
 * cannot draw. Pushed past the measured value deliberately: the extra gain and
 * a saturation lift stand in for the light we are not spreading. Saturation is
 * applied about luma so hues open up rather than just clipping to white. */
#define NEON_BLOOM 1.92f
#define NEON_SAT   1.30f
/* How much of the gain that overflows 255 comes back as WHITE.
 *
 * This is the third attempt at handling overflow, and the first that does not
 * have to trade one defect for the other:
 *
 *   per-channel clip - every saturated palette entry pinned to the same corner
 *     of the colour cube, so vacuum, boost and overboost converged on nearly
 *     the same post-bloom colour. That was the original complaint.
 *   proportional scale - divides the whole vector by 255/peak, which preserves
 *     hue and fixed the convergence, but for a saturated colour that factor is
 *     about 0.46, so it hands back ALL the gain. The "bloomed" body came out
 *     DARKER than the raw palette halo behind it (overboost measured
 *     (255,85,0) against a raw (255,106,0)), and the ring's three bands
 *     collapsed into one flat colour. That was the two-tone going missing.
 *
 * Both treat overflow as something to squash. Real bloom does not: light past
 * full scale desaturates toward white. So normalise to peak 255 - keeping the
 * hue the proportional clamp got right - and then lift what was clipped back in
 * as a blend toward white, by the FRACTION of the gain that was lost
 * (1 - 255/peak). A colour that barely overflows stays its own hue; one that
 * overflows hard reads as a hot, whitened version of itself. The body ends up
 * clearly lighter than the unbloomed halo on every palette entry, which is what
 * makes the band structure read, while the hue stays distinct per zone, which
 * is what keeps the zones apart.
 *
 * Kept small (0.35) because it is NOT carrying the band structure on its own -
 * NEON_HALO_DIM below does that. Its only job here is to make the bloom
 * actually brighten: measured over all four palettes, the body's luminance now
 * exceeds the raw palette's in every zone, where the bare proportional clamp
 * had it lower in 11 of 12. Larger values buy contrast at the cost of zone
 * separation (at 0.75 the four palettes' worst zone pair falls from 158 to
 * 113), and separation is the thing that was hard to win back. */
#define NEON_WHITE_LIFT 0.35f
/* The ring's inner band is a DIMMED zone colour, not the raw palette entry.
 *
 * The three bands are meant to read dark -> bright -> white from the inside
 * out. They stopped doing that when neon_lit() started clamping
 * proportionally: for a saturated palette entry that divides the whole vector
 * by about 0.46, which made the "bloomed" body DARKER than the raw palette
 * colour drawn behind it. Measured across the four palettes, the ordering was
 * inverted in 11 of 12 zones - so the ring was not merely low-contrast, its
 * gradient ran backwards, which is what collapsed it into one flat colour on
 * the panel.
 *
 * Dimming the inner band fixes the ordering at its source and costs nothing in
 * zone separation, because it scales a colour rather than reshaping it. 0.55
 * matches the factor the marquee's accent bulbs already use for the same
 * "dimmed zone colour" job. Worst-case halo-to-body contrast over all four
 * palettes goes from 4 to 125. */
#define NEON_HALO_DIM 0.55f
static inline uint32_t neon_lit(uint32_t rgb)
{
    float r = (float)((rgb >> 16) & 0xFFu);
    float g = (float)((rgb >> 8) & 0xFFu);
    float b = (float)(rgb & 0xFFu);
    const float luma = 0.299f * r + 0.587f * g + 0.114f * b;
    r = (luma + (r - luma) * NEON_SAT) * NEON_BLOOM;
    g = (luma + (g - luma) * NEON_SAT) * NEON_BLOOM;
    b = (luma + (b - luma) * NEON_SAT) * NEON_BLOOM;
    r = clampf(r, 0.0f, 1.0e9f);
    g = clampf(g, 0.0f, 1.0e9f);
    b = clampf(b, 0.0f, 1.0e9f);
    const float peak = fmaxf(r, fmaxf(g, b));
    if (peak > 255.0f) {
        const float scale = 255.0f / peak;
        r *= scale;
        g *= scale;
        b *= scale;
        const float w = (1.0f - scale) * NEON_WHITE_LIFT;
        r += (255.0f - r) * w;
        g += (255.0f - g) * w;
        b += (255.0f - b) * w;
    }
    return ((uint32_t)lroundf(r) << 16) | ((uint32_t)lroundf(g) << 8)
         | (uint32_t)lroundf(b);
}

/* The tube's lit run is white + three tones, one colour per band (index order
 * inner to outer): dim halo, bloomed body, then the track's OUTER half
 * LIGHTENED via HSL lightness (hue and saturation preserved) - the band stack
 * must read as a smooth gradient dark -> bloomed -> lighter -> white, so the
 * outer half goes lighter than the middle, never darker (a deeper outer turns
 * the stack into a dark-bright-dark sandwich). One function feeds the live
 * arcs, the gradient bake and the tile blits so they cannot drift. */
static uint32_t neon_tube_band_color(uint32_t accent_rgb, int band)
{
    const uint32_t mid = scale_rgb(neon_lit(accent_rgb), NEON_TUBE_MID_SCALE);
    switch (band) {
        case 0: return scale_rgb(accent_rgb, NEON_HALO_DIM);
        case 1: return mid;
        case 2: return neon_hsl_lighten(mid, NEON_TUBE_TRACK_LIGHT);
        default: return 0xFFFFFFu;
    }
}

/* HSL hue-to-rgb component (the standard p/q/t algorithm). `t` may wrap. */
static float neon_hsl_hue2rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

/* Raise a colour's HSL lightness by d_light, keeping hue AND saturation. This
 * is how the tube's outer track half is made lighter than the bloomed middle
 * without washing it out: the rejected white-lift desaturated toward the cap,
 * which is what read as "too pale" and is also why a fixed lerp cannot work
 * for every user colour. Lightness is (max+min)/2, so a saturated colour still
 * has room to brighten even when one channel is already 255. Pure black/white
 * have no hue and just scale to the same neutral. */
static uint32_t neon_hsl_lighten(uint32_t rgb, float d_light)
{
    const float r = (float)((rgb >> 16) & 0xFFu) / 255.0f;
    const float g = (float)((rgb >> 8) & 0xFFu) / 255.0f;
    const float b = (float)(rgb & 0xFFu) / 255.0f;
    const float mx = fmaxf(r, fmaxf(g, b));
    const float mn = fminf(r, fminf(g, b));
    const float d = mx - mn;
    float h = 0.0f, s = 0.0f;
    const float l = (mx + mn) * 0.5f;
    if (d > 0.0f) {
        s = (l > 0.5f) ? d / (2.0f - mx - mn) : d / (mx + mn);
        if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
        else if (mx == g) h = (b - r) / d + 2.0f;
        else h = (r - g) / d + 4.0f;
        h *= 60.0f;
    }
    const float nl = clampf(l + d_light, 0.0f, 1.0f);
    const float q = (nl < 0.5f) ? nl * (1.0f + s) : nl + s - nl * s;
    const float p = 2.0f * nl - q;
    const float hk = h / 360.0f;
    const int nr = (int)lroundf(neon_hsl_hue2rgb(p, q, hk + 1.0f / 3.0f) * 255.0f);
    const int ng = (int)lroundf(neon_hsl_hue2rgb(p, q, hk) * 255.0f);
    const int nb = (int)lroundf(neon_hsl_hue2rgb(p, q, hk - 1.0f / 3.0f) * 255.0f);
    return ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
}
static uint32_t neon_zone_rgb(const boost_theme_t *t, float psi);
static float s_psi_min;
static float s_psi_max;
static inline int32_t px_icx(void);
static inline int32_t px_icy(void);
static const boost_theme_t *active_theme(void);
static float psi_to_sweep(float psi, float a0, float a1);
static lv_color_t color_for_psi(const boost_theme_t *theme, float psi);
static bool clip_reaches_radius(lv_layer_t *layer, float cx, float cy, float r);
static float s_display_psi;
static float s_peak_psi;
static float s_psi_overboost = DEFAULT_PSI_OVERBOOST;

/* x of a psi value along marquee's bar, mapped through the SAME rule the ring
 * layouts use. Feeding psi_to_sweep() a 0..width range instead of an angular one
 * makes the bar inherit zeroAngle, psiMin and psiMax from the settings exactly
 * as the arc does: a plain min..max lerp put zero at 60% of the bar while the
 * ring put it at 37.5%, so the two layouts disagreed about where zero was. */
static int neon_bar_x(int cx, float psi)
{
    const int half = neon_mq(NEON_BAR_HALF);
    return cx - half + (int)lroundf(psi_to_sweep(psi, 0.0f, (float)(half * 2)));
}

/* Bulb colour. The mockup dims the zone colour to 55% for the border so it
 * frames the readout instead of competing with it; the bloom then lifts it
 * back to roughly the palette value. Now baked into each of the three rings
 * with the ring's own zone colour. */
static uint32_t neon_bulb_accent(uint32_t accent_rgb)
{
    return neon_lit(scale_rgb(accent_rgb, 0.55f));
}

/* Centre of the i-th bulb of ring z (0 = innermost/vacuum .. 2 =
 * outermost/overboost). One place, so the bake, the live draw and the
 * invalidation cannot disagree about where the rings are.
 *
 * The offsets are precomputed once into s_neon_bulb_ox/oy (the -90 deg
 * offset puts bulb 0 at 12 o'clock on a y-down screen; every ring count is
 * even, so bulb N/2 lands exactly at 6 o'clock too). Computing lroundf(cos/sin)
 * per call made the live accent scan pay two trig calls and a rounding for
 * every accent bulb on every callback invocation - the table keeps the
 * arithmetic bit-identical but turns the call into one table load. */
static void neon_bulb_pos(int cx, int cy, int z, int i, int *bx, int *by)
{
    if (!s_neon_bulb_table_ready) {
        for (int tz = 0; tz < NEON_BULB_RINGS; ++tz) {
            const float r = (float)NEON_BULB_RING_R(tz);
            for (int ti = 0; ti < NEON_BULB_N(tz); ++ti) {
                const float rad = ((float)ti * (360.0f / (float)NEON_BULB_N(tz))
                                   - 90.0f) * (float)M_PI / 180.0f;
                s_neon_bulb_ox[tz][ti] = (int16_t)lroundf(cosf(rad) * r);
                s_neon_bulb_oy[tz][ti] = (int16_t)lroundf(sinf(rad) * r);
            }
        }
        s_neon_bulb_table_ready = true;
    }
    *bx = cx + s_neon_bulb_ox[z][i];
    *by = cy + s_neon_bulb_oy[z][i];
}

/* Bounding box of every ring segment at its widest stroke, in screen
 * coordinates. Built once at scene build.
 *
 * The draw callback runs once per DIRTY REGION, and it used to submit the whole
 * lit run every time - so a frame with three dirty regions and a 28-segment run
 * issued 28 x 3 x 3 = 252 arc primitives, of which all but a handful were
 * outside the region being repainted and got thrown away downstream. Measured
 * before this existed: 28.8 segments per cycle mean, 84 max. Tube and segments
 * flush byte-identical pixel counts, yet tube ran at 41 FPS against segments'
 * 22 on hardware, which isolates the cost to the primitive count rather than to
 * the fill. */
static lv_area_t s_neon_seg_box[NEON_NSEG];
static bool s_neon_seg_box_ready;

/* Local rather than LVGL's lv_area_is_on(), which lives in lv_area_private.h. */
static inline bool neon_area_overlaps(const lv_area_t *a, const lv_area_t *b)
{
    return a->x1 <= b->x2 && a->x2 >= b->x1 && a->y1 <= b->y2 && a->y2 >= b->y1;
}

static void neon_build_seg_boxes(void)
{
    for (int i = 0; i < NEON_NSEG; ++i) {
        const float s = neon_seg_start(i);
        lv_draw_arc_get_area(px_icx(), px_icy(), (uint16_t)NEON_R,
                             s, s + NEON_SEG_LIT,
                             (uint16_t)NEON_SEG_BAND_DEPTH,
                             false, &s_neon_seg_box[i]);
        /* Inflate by NEON_BAND_SPR_MARGIN so a dirty region that reaches only
         * the segment's AA fringe (the angle mask paints ~1-2 px past the
         * wedge's end edge, and the end corner reaches sin(step)*(R-rin)
         * beyond the bbox) still overlaps the box and the segment is
         * submitted. Without this, a neighbouring segment's region repaints
         * the canvas over the fringe and strands it: the canvas's track
         * fringe is the track colour at single-digit % coverage, which
         * RGB565 quantises to black, while the live cap arc's white fringe
         * is visible - so the partial pipeline showed black where the truth
         * showed the lit fringe. */
        s_neon_seg_box[i].x1 -= NEON_BAND_SPR_MARGIN;
        s_neon_seg_box[i].y1 -= NEON_BAND_SPR_MARGIN;
        s_neon_seg_box[i].x2 += NEON_BAND_SPR_MARGIN;
        s_neon_seg_box[i].y2 += NEON_BAND_SPR_MARGIN;
    }
    s_neon_seg_box_ready = true;
}

static void paint_neon_background(lv_obj_t *canvas, const boost_theme_t *theme)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = { 0, 0, DISP_SIZE - 1, DISP_SIZE - 1 };
    lv_draw_rect(&layer, &bg, &full);
    const int cx = px_icx() - s_px_dx;
    const int cy = px_icy() - s_px_dy;
    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = cx; arc.center.y = cy; arc.radius = NEON_R;
    arc.rounded = false; arc.opa = LV_OPA_COVER;
    /* The zero marker is the zero segment itself, painted white, rather than a
     * tick outside the ring. It reads at a glance and costs nothing extra. */
    const int zero_seg = neon_seg_index(psi_to_sweep(0.0f, (float)ARC_START,
                                                     (float)(ARC_START + ARC_RANGE)));
    if (s_neon_layout == BOOST_NEON_MARQUEE) {
        lv_draw_rect_dsc_t bulb;
        lv_draw_rect_dsc_init(&bulb);
        bulb.radius = LV_RADIUS_CIRCLE;
        bulb.bg_opa = LV_OPA_COVER;
        /* All three rings are baked as DEAD track: every one of the 216
         * bulbs is dim neutral in the background. The coloured accents are
         * drawn LIVE in draw_neon_live() - ring z's 24 accent bulbs light in
         * ring z's zone colour only when the reading has reached that zone
         * (see NEON_BULB_IS_ACCENT / the stage-ladder comment). Baking them
         * track means a zone flip repaints only the changed rings' 12
         * pair-boxes instead of the whole border, and the two-tone rest
         * state (only the innermost ring coloured) comes free. */
        for (int z = 0; z < NEON_BULB_RINGS; ++z) {
            for (int i = 0; i < NEON_BULB_N(z); ++i) {
                bulb.bg_color = c(theme->track);
                int bx, by;
                neon_bulb_pos(cx, cy, z, i, &bx, &by);
                lv_area_t a = { bx - NEON_BULB_HALF, by - NEON_BULB_HALF,
                                bx + NEON_BULB_HALF, by + NEON_BULB_HALF };
                lv_draw_rect(&layer, &bulb, &a);
            }
        }
        lv_draw_rect_dsc_t track;
        lv_draw_rect_dsc_init(&track);
        track.radius = 8; track.bg_opa = LV_OPA_COVER; track.bg_color = c(theme->track);
        lv_area_t bar = { cx - neon_mq(NEON_BAR_HALF), cy + neon_mq(NEON_BAR_Y),
                          cx + neon_mq(NEON_BAR_HALF),
                          cy + neon_mq(NEON_BAR_Y) + neon_mq(NEON_BAR_H) };
        lv_draw_rect(&layer, &track, &bar);
    } else if (s_neon_layout == BOOST_NEON_TUBE) {
        arc.start_angle = (float)ARC_START;
        arc.end_angle = (float)(ARC_START + ARC_RANGE);
        arc.color = c(theme->track);
        /* Unlit track spans the track + cap footprint (NEON_R down to the
         * halo's outer edge), a bit wider than the old body width. */
        arc.width = NEON_TUBE_TRACK_W;
        lv_draw_arc(&layer, &arc);
        const float zero = psi_to_sweep(0.0f, (float)ARC_START,
                                        (float)(ARC_START + ARC_RANGE));
        arc.start_angle = zero - NEON_TUBE_ZERO_DEG + NEON_TUBE_ZERO_CENTER;
        arc.end_angle = zero + NEON_TUBE_ZERO_DEG + NEON_TUBE_ZERO_CENTER;
        arc.color = lv_color_white();
        /* Full lit depth (NEON_TUBE_BAND_DEPTH): the zero marker is a bright
         * full-band landmark spanning the whole ring width, restored to its
         * original design (the 2026-08-12 "inside the track" request was
         * about the PEAK/max marker only - shortening the zero marker was a
         * mistake). Baked here so the marker still shows when nothing is lit
         * and the live pass never runs; the live pass in draw_neon_live()
         * redraws it on top of the run. */
        arc.width = NEON_TUBE_BAND_DEPTH;
        lv_draw_arc(&layer, &arc);
    } else for (int i = 0; i < NEON_NSEG; ++i) {
        arc.start_angle = neon_seg_start(i);
        arc.end_angle = arc.start_angle + NEON_SEG_LIT;
        /* The zero marker is drawn at the LIT width so it has the same
         * length and shape as a coloured segment and reads as a marker
         * rather than as a brighter piece of track. */
        arc.color = (i == zero_seg) ? lv_color_white() : c(theme->track);
        arc.width = (i == zero_seg) ? NEON_SEG_BAND_DEPTH : NEON_SEG_W;
        lv_draw_arc(&layer, &arc);
    }
    /* No scale numerals. Inside the ring the lit segments' inner band reaches
     * over them; outside, a 466 px round panel puts them under the bezel. The
     * SCALE ITSELF is unchanged - the lit run is still mapped through
     * psi_to_sweep(), so zeroAngle, psiMin and psiMax from the settings drive
     * it exactly as they drive every other face. Only the numerals are gone. */
    /* (Numerals were removed rather than left disabled: any future revival must
     * re-check the ring-bezel clearance measured when they were dropped.) */

    /* Hairline rule between the unit mark and the peak readout. It never
     * moves, so it is baked rather than drawn live. */
    lv_draw_rect_dsc_t rule;
    lv_draw_rect_dsc_init(&rule);
    rule.bg_color = c(theme->muted);
    rule.bg_opa = LV_OPA_COVER;
    /* Same offset from the unit mark and the peak on every layout; the lower
     * stack sits at one rhythm everywhere now. */
    const int rule_y = cy + neon_mq(NEON_RULE_Y);
    const int rule_half = neon_mq(62);
    lv_area_t ra = { cx - rule_half, rule_y, cx + rule_half, rule_y + 1 };
    lv_draw_rect(&layer, &rule, &ra);

    lv_canvas_finish_layer(canvas, &layer);
}

/* The ONE place the neon zone colour is decided. Drawing and flip detection
 * must agree: the colour is chosen here and flips are detected here. Using
 * color_for_psi() for the flip test while drawing from a different threshold
 * set left the readout recolouring at 0.05 psi with nothing invalidated. */
static uint32_t neon_zone_rgb(const boost_theme_t *t, float psi)
{
    return (psi >= s_psi_overboost) ? t->overboost
         : (psi > 0.05f) ? t->boost : t->vacuum;
}

/* The peak tell-tale's fixed ink, shared by the tube and segments layouts:
 * the DARKER of the vacuum state's inner-ring colours (scale_rgb(vacuum,
 * NEON_HALO_DIM) - the same dim factor the halo band uses). Deliberately NOT
 * the peak value's zone colour: a marker in the live zone colour stood out
 * jarringly against the track and re-tinted every time the peak crossed a
 * zone threshold. The constant vacuum tone makes the marker read as a
 * reference mark rather than as another lit band, and it never restyles, so
 * its invalidation has no colour-flip case. */
static uint32_t neon_peak_color(const boost_theme_t *t)
{
    return scale_rgb(t->vacuum, NEON_HALO_DIM);
}

/* Draw-cost counters. The audit measures FLUSHED pixels, which is the dirty
 * area and says nothing about how much rasterisation happened inside it. These
 * count the work instead: how many times the callback ran per render cycle (one
 * per dirty region) and how many arc primitives it submitted. Compiled out
 * unless BOOST_NEON_DRAW_STATS is defined. */
#if BOOST_NEON_DRAW_STATS
uint32_t g_neon_cb_calls;
uint32_t g_neon_arcs;
uint32_t g_neon_arcs_clipped;
uint32_t g_neon_labels;
uint32_t g_neon_sign_bars;
uint32_t g_neon_sprite_blits;
#define NEON_STAT_CB()      (g_neon_cb_calls++)
#define NEON_STAT_ARC()     (g_neon_arcs++)
#define NEON_STAT_SKIP()    (g_neon_arcs_clipped++)
#define NEON_STAT_LABEL()   (g_neon_labels++)
#define NEON_STAT_SIGN()    (g_neon_sign_bars++)
#define NEON_STAT_SPRITE()  (g_neon_sprite_blits++)
#else
#define NEON_STAT_CB()      ((void)0)
#define NEON_STAT_ARC()     ((void)0)
#define NEON_STAT_SKIP()    ((void)0)
#define NEON_STAT_LABEL()   ((void)0)
#define NEON_STAT_SIGN()    ((void)0)
#define NEON_STAT_SPRITE()  ((void)0)
#endif

#if BOOST_NEON_GLYPH_SPRITES
/* Mirrors the two-tier threshold in neon_zone_rgb() exactly. Kept as its own
 * one-line function, rather than refactoring neon_zone_rgb() to share it, so
 * the existing (already relied upon) function is not touched by this work.
 * Lives OUTSIDE the sprite guard: the marquee's live accent bulbs and the
 * zone-word sprite selector both call it, and the accent bulbs are drawn
 * with plain lv_draw_rect - they exist whether or not glyph sprites do. */
#endif
static inline int neon_zone_id(float psi)
{
    return (psi >= s_psi_overboost) ? 2 : (psi > 0.05f) ? 1 : 0;
}
#if BOOST_NEON_GLYPH_SPRITES

static void neon_free_glyph_sprites(void)
{
    if (s_neon_glyph_buf != NULL) {
        BG_FREE(s_neon_glyph_buf);
        s_neon_glyph_buf = NULL;
    }
    memset(s_neon_glyph_img, 0, sizeof(s_neon_glyph_img));
    if (s_neon_glyph_buf_s != NULL) {
        BG_FREE(s_neon_glyph_buf_s);
        s_neon_glyph_buf_s = NULL;
    }
    memset(s_neon_glyph_img_s, 0, sizeof(s_neon_glyph_img_s));
    memset(s_neon_glyph_bbox_s, 0, sizeof(s_neon_glyph_bbox_s));
    s_neon_glyph_sprites_ready = false;
    s_neon_sprite_layout = (boost_neon_layout_t)0xFF;
    /* Segment band tiles are layout-independent but kept only while the
     * segments layout is active; dropping them here (the same scene-build-only
     * path) keeps them from eating PSRAM on the tube/marquee faces. */
    if (s_neon_seg_buf != NULL) {
        BG_FREE(s_neon_seg_buf);
        s_neon_seg_buf = NULL;
    }
    memset(s_neon_seg_tile, 0, sizeof(s_neon_seg_tile));
    s_neon_seg_sprites_ready = false;
    /* Tube run tiles: a_zero-dependent, so kept only while the tube layout is
     * active; same lifecycle rule as the segment tiles. */
    if (s_neon_tube_buf != NULL) {
        BG_FREE(s_neon_tube_buf);
        s_neon_tube_buf = NULL;
    }
    memset(s_neon_tube_tile, 0, sizeof(s_neon_tube_tile));
    memset(s_neon_tube_box, 0, sizeof(s_neon_tube_box));
    s_neon_tube_sprites_ready = false;
    /* Tube radial-gradient arc image. Same lifecycle as the tube tiles:
     * scene-build-only, freed here so it does not eat PSRAM on the
     * segments/marquee faces. */
    for (int z = 0; z < NEON_GRAD_ZONES; ++z) {
        if (s_neon_grad_buf[z] != NULL) {
            BG_FREE(s_neon_grad_buf[z]);
            s_neon_grad_buf[z] = NULL;
        }
    }
    memset(s_neon_grad_img, 0, sizeof(s_neon_grad_img));
    memset(s_neon_grad_key, 0, sizeof(s_neon_grad_key));
}

/* Deliberately does NOT touch the zone-word tiles. They are baked once and
 * never freed or rebaked: nothing they depend on can change at runtime - three
 * fixed strings, one fixed font, one fixed box - and coverage carries no
 * colour, so not even a palette change reaches them. There is no free
 * counterpart for them on purpose; a function that is never called would just
 * be dead code claiming otherwise. */

/* Top-left corner of the box the zone word is laid out in. The word used to
 * be a 300x34 lv_label aligned to the face centre with a per-layout y offset;
 * keeping that exact box means the bake's recorded offsets place the sprite
 * on the same pixels the label occupied. */
static void neon_word_box_origin(int cx, int cy, int *x, int *y)
{
    *x = cx - NEON_WORD_BOX_W / 2;
    *y = cy + NEON_WORD_Y - NEON_WORD_BOX_H / 2;
}

/* Separable box blur over an 8-bit coverage plane, in place. w and h are
 * bounded well under 256 by every caller (largest tile in use is the
 * marquee's padded coverage window), so a fixed on-stack scratch row is
 * enough - no per-call heap traffic in a routine run 3 zones x 12 glyphs x
 * 2 passes per scene build.
 *
 * Sliding-window running sum, not a re-summed kernel per pixel: the naive
 * form re-adds all (2r+1) samples at every x, which is O(w*r) per row and
 * got directly more expensive when NEON_GLOW_BLUR_R grew from 3 to 5. This
 * carries one running sum/count across the row (or column), adding the pixel
 * entering the window and dropping the one leaving it, which is O(w)
 * regardless of r - the result is bit-for-bit the same box average, just
 * without recomputing it from scratch every step. */
/* Sized for the widest thing blurred, which is no longer a glyph tile (~132)
 * but the zone-word scratch (360). The cost is one stack buffer of this many
 * bytes, and every caller runs at scene-build time on the display task, not
 * in a draw path - so this is 128 bytes of build-time stack, not a per-frame
 * cost. A scratch wider than this is REJECTED rather than clipped, which is
 * how the first zone-word bake failed: "words stay unglowed", silently
 * correct and visibly flat. */
#define NEON_BLUR_MAXDIM 384
static void neon_box_blur(uint8_t *cov, int w, int h, int r)
{
    if (w <= 0 || h <= 0 || w > NEON_BLUR_MAXDIM || h > NEON_BLUR_MAXDIM) return;
    uint8_t line[NEON_BLUR_MAXDIM];
    /* Horizontal pass, row by row. */
    for (int y = 0; y < h; ++y) {
        uint8_t *row = cov + (size_t)y * (size_t)w;
        int sum = 0, count = 0;
        for (int k = 0; k <= r && k < w; ++k) { sum += row[k]; count++; }
        for (int x = 0; x < w; ++x) {
            line[x] = (uint8_t)(sum / count);
            const int add = x + r + 1;
            const int drop = x - r;
            if (add < w) { sum += row[add]; count++; }
            if (drop >= 0) { sum -= row[drop]; count--; }
        }
        memcpy(row, line, (size_t)w);
    }
    /* Vertical pass, column by column. */
    for (int x = 0; x < w; ++x) {
        int sum = 0, count = 0;
        for (int k = 0; k <= r && k < h; ++k) { sum += cov[(size_t)k * (size_t)w + (size_t)x]; count++; }
        for (int y = 0; y < h; ++y) {
            line[y] = (uint8_t)(sum / count);
            const int add = y + r + 1;
            const int drop = y - r;
            if (add < h) { sum += cov[(size_t)add * (size_t)w + (size_t)x]; count++; }
            if (drop >= 0) { sum -= cov[(size_t)drop * (size_t)w + (size_t)x]; count--; }
        }
        for (int y = 0; y < h; ++y) cov[(size_t)y * (size_t)w + (size_t)x] = line[y];
    }
}

/* Zero a window of the scratch canvas directly against its backing buffer,
 * bypassing lv_draw_rect()'s style/blend dispatch for a plain opaque black
 * fill. Run up to 48 times per scene build (12 geometry-pass glyphs over the
 * whole canvas, 36 colour-pass glyphs over just the crop window - see the
 * call sites), so the dispatch overhead and, for the colour pass, the area
 * cleared both mattered. x0/y0/w/h are assumed already clamped to
 * [0, canvas width) x [0, canvas height) by the caller. */
static void neon_scratch_clear(lv_obj_t *scratch, int x0, int y0, int w, int h)
{
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
    for (int y = 0; y < h; ++y) {
        uint8_t *row = db->data + (size_t)(y0 + y) * db->header.stride + (size_t)x0 * 2u;
        memset(row, 0, (size_t)w * 2u);
    }
}

static void neon_draw_sign_pass(lv_layer_t *layer,
                                const boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS],
                                uint32_t color);

/* Fold one glyph's core and blurred coverage into the single 8-bit plane that
 * gets blitted, and publish the windowed image descriptor for it.
 *
 * cov = max(core, glow*gain) is unchanged from the RGB565 version - what
 * changes is that the result is stored as coverage rather than immediately
 * multiplied into an ink colour, so the ink can be chosen per blit. Inside the
 * glyph's solid ink core saturates at 255 and the gain cannot touch it, so the
 * body still reads as exactly the accent colour and only the surround glows. */
static void neon_store_coverage(int slot, const uint8_t *core, const uint8_t *glow,
                                int cov_w, int pad, int spr_w, int spr_h,
                                uint32_t spr_stride, size_t glyph_bytes)
{
    uint8_t *dst_bytes = (uint8_t *)s_neon_glyph_buf + (size_t)slot * glyph_bytes;
    for (int y = 0; y < spr_h; ++y) {
        uint8_t *dst = dst_bytes + (size_t)y * spr_stride;
        for (int x = 0; x < spr_w; ++x) {
            const int cxi = x + pad, cyi = y + pad;
            const int core_v = core[(size_t)cyi * (size_t)cov_w + (size_t)cxi];
            int glow_v = (glow[(size_t)cyi * (size_t)cov_w + (size_t)cxi] * NEON_GLOW_GAIN_PCT) / 100;
            if (glow_v > 255) glow_v = 255;
            dst[x] = (uint8_t)(core_v > glow_v ? core_v : glow_v);
        }
    }

    /* Store the TIGHT-BBOX WINDOW, not the full tile. The tile is one uniform
     * size shared by every sprite so they can share a single anchor, but a
     * narrow glyph's tile is mostly empty, and blitting the empty part is
     * wasted fill even now that it no longer erases anything. Baking the crop
     * here means draw time never has to build a windowed descriptor on the
     * stack. */
    const int16_t *bb = s_neon_glyph_bbox[slot];
    const int bw = bb[2] - bb[0] + 1;
    const int bh = bb[3] - bb[1] + 1;
    lv_image_dsc_t *img = &s_neon_glyph_img[slot];
    memset(img, 0, sizeof(*img));
    img->header.magic = LV_IMAGE_HEADER_MAGIC;
    img->header.cf = LV_COLOR_FORMAT_A8;
    img->header.w = (uint32_t)bw;
    img->header.h = (uint32_t)bh;
    img->header.stride = spr_stride;
    img->data_size = (uint32_t)((size_t)spr_stride * (size_t)bh);
    img->data = (const uint8_t *)s_neon_glyph_buf + (size_t)slot * glyph_bytes +
                (size_t)bb[1] * spr_stride + (size_t)bb[0];
}

/* Blit one baked coverage tile as `ink`, anchored the way the live label draw
 * anchors its box. Returns false if the slot was never baked, which is the
 * caller's cue to fall back to drawing the thing live. */
static bool neon_blit_sprite_scaled(lv_layer_t *layer, int slot,
                                    int anchor_x, int anchor_y, uint32_t ink,
                                    float scale);
/* Scaled variant for the marquee: draws the SAME baked tile at scale S, so
 * the one shared sprite set serves every layout. The anchor is the cell
 * centre already scaled about the face centre (the caller scales cell.x and
 * the bake-time spr offsets by S); the bbox offsets are sprite-local and
 * must scale with them. coords is the FULL-size rect (LVGL derives the
 * transformed area from coords + scale + pivot), positioned so that the
 * transform pivot - the image centre - lands on the scaled ink centre:
 * anchor + S*bb + S*(bw/2, bh/2), i.e. gx = anchor + S*bb + (S-1)*bw/2. The
 * clip test against the full-size rect is conservative (the drawn area is
 * smaller), which is correct. */
static bool neon_blit_sprite_scaled(lv_layer_t *layer, int slot,
                                    int anchor_x, int anchor_y, uint32_t ink,
                                    float scale)
{
    /* Pre-scaled marquee set: a plain blit of the baked 0.87-size tile at
     * anchor + bbox_s. The pixels are identical to what the per-frame
     * transform used to paint (same lv_draw_image call, once at bake), so
     * this is a pure cost win with no visual change. */
    if (scale != 1.0f && s_neon_glyph_buf_s != NULL) {
        const lv_image_dsc_t *img = &s_neon_glyph_img_s[slot];
        if (img->data == NULL) return false;
        const int16_t *bb = s_neon_glyph_bbox_s[slot];
        const int bw = (int)img->header.w;
        const int bh = (int)img->header.h;
        lv_area_t a = { anchor_x + bb[0], anchor_y + bb[1],
                        anchor_x + bb[0] + bw - 1, anchor_y + bb[1] + bh - 1 };
        if (!neon_area_overlaps(&a, &layer->_clip_area)) return true;
        NEON_STAT_SPRITE();
        lv_draw_image_dsc_t idsc; lv_draw_image_dsc_init(&idsc);
        idsc.src = img;
        idsc.opa = LV_OPA_COVER;
        /* A8 source: LVGL blends `recolor` through the plane as the ink. */
        idsc.recolor = c(ink);
        idsc.recolor_opa = LV_OPA_COVER;
        lv_draw_image(layer, &idsc, &a);
        return true;
    }
    const lv_image_dsc_t *img = &s_neon_glyph_img[slot];
    if (img->data == NULL) return false;
    const int16_t *bb = s_neon_glyph_bbox[slot];
    const int bw = (int)img->header.w;
    const int bh = (int)img->header.h;
    const int gx = (int)lroundf((float)anchor_x + scale * (float)bb[0] +
                                (scale - 1.0f) * (float)bw / 2.0f);
    const int gy = (int)lroundf((float)anchor_y + scale * (float)bb[1] +
                                (scale - 1.0f) * (float)bh / 2.0f);
    lv_area_t a = { gx, gy, gx + bw - 1, gy + bh - 1 };
    if (!neon_area_overlaps(&a, &layer->_clip_area)) return true;
    NEON_STAT_SPRITE();
    lv_draw_image_dsc_t idsc; lv_draw_image_dsc_init(&idsc);
    idsc.src = img;
    idsc.opa = LV_OPA_COVER;
    /* For an A8 source LVGL blends `recolor` through the plane regardless of
     * recolor_opa, so this is the ink, not a tint applied on top of one. */
    idsc.recolor = c(ink);
    idsc.recolor_opa = LV_OPA_COVER;
    if (scale != 1.0f) {
        idsc.scale_x = idsc.scale_y = (uint16_t)lroundf(256.0f * scale);
        idsc.pivot.x = bw / 2;
        idsc.pivot.y = bh / 2;
    }
    lv_draw_image(layer, &idsc, &a);
    return true;
}

/*
 * Bake the minus mark into the same coverage pipeline as the digits.
 *
 * The mark cannot come from the font: this typeface's '-' has no contour, so
 * it has always been drawn as two sheared parallelogram bars from
 * boost_neon_sign_bars(). Its glow used to be faked by running that same
 * geometry a second time with every edge pushed out NEON_SIGN_GLOW px in a dim
 * colour. That does not read as glow at this scale. The bars are only ~6 px
 * tall on an ~8 px pitch, so a 5 px inflation overlapped the two bars by 8 px
 * and flooded the 2 px gap between them - the mark came out as one solid slab
 * with a slot in it, nothing like the digits beside it.
 *
 * Baking it instead means the mark gets the identical two-pass box blur the
 * digits get, so the two match by construction rather than by tuning. It is
 * drawn into the scratch canvas at the same anchor the live code uses (box
 * centre horizontally, top + line_h/2 vertically), which is what lets the blit
 * reuse the glyphs' own dx/dy mapping.
 */
static void neon_bake_sign_sprite(lv_obj_t *scratch, int safe_w, int safe_h,
                                  int cx0, int cy0, int font_px, int sign_w,
                                  uint8_t *core, uint8_t *glow,
                                  int cov_w, int cov_h, int pad,
                                  int spr_w, int spr_h,
                                  uint32_t spr_stride, size_t glyph_bytes)
{
    const lv_font_t *font = NEON_BIG;
    const int line_h = (int)lv_font_get_line_height(font);

    boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS];
    boost_neon_sign_bars(safe_w / 2, line_h / 2, font_px, sign_w, bars);

    lv_layer_t layer;
    lv_canvas_init_layer(scratch, &layer);
    neon_scratch_clear(scratch, 0, 0, safe_w, safe_h);
    neon_draw_sign_pass(&layer, bars, 0xFFFFFFu);
    lv_canvas_finish_layer(scratch, &layer);

    const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
    memset(core, 0, (size_t)cov_w * (size_t)cov_h);
    int ix0 = safe_w, iy0 = safe_h, ix1 = -1, iy1 = -1;
    for (int y = 0; y < cov_h; ++y) {
        const int sy = cy0 - pad + y;
        if (sy < 0 || sy >= safe_h) continue;
        const lv_color16_t *row =
            (const lv_color16_t *)(db->data + (size_t)sy * db->header.stride);
        for (int x = 0; x < cov_w; ++x) {
            const int sx = cx0 - pad + x;
            if (sx < 0 || sx >= safe_w) continue;
            const uint8_t v = (uint8_t)(row[sx].red * 255 / 31);
            core[(size_t)y * (size_t)cov_w + (size_t)x] = v;
            if (v) {
                if (sx < ix0) ix0 = sx;
                if (sx > ix1) ix1 = sx;
                if (sy < iy0) iy0 = sy;
                if (sy > iy1) iy1 = sy;
            }
        }
    }
    if (ix1 < ix0 || iy1 < iy0) {
        /* No ink found - leave the slot unpublished so the draw path falls
         * back to the live triangle mark rather than blitting a blank tile. */
        s_neon_glyph_img[NEON_SIGN_SLOT].data = NULL;
        return;
    }

    memcpy(glow, core, (size_t)cov_w * (size_t)cov_h);
    neon_box_blur(glow, cov_w, cov_h, NEON_GLOW_BLUR_R);
    neon_box_blur(glow, cov_w, cov_h, NEON_GLOW_BLUR_R);

    int bx0 = ix0 - NEON_SPR_GLOW_MARGIN - cx0;
    int by0 = iy0 - NEON_SPR_GLOW_MARGIN - cy0;
    int bx1 = ix1 + NEON_SPR_GLOW_MARGIN - cx0;
    int by1 = iy1 + NEON_SPR_GLOW_MARGIN - cy0;
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > spr_w - 1) bx1 = spr_w - 1;
    if (by1 > spr_h - 1) by1 = spr_h - 1;
    bx0 &= ~(LV_DRAW_BUF_ALIGN - 1);
    by0 &= ~1;
    s_neon_glyph_bbox[NEON_SIGN_SLOT][0] = (int16_t)bx0;
    s_neon_glyph_bbox[NEON_SIGN_SLOT][1] = (int16_t)by0;
    s_neon_glyph_bbox[NEON_SIGN_SLOT][2] = (int16_t)bx1;
    s_neon_glyph_bbox[NEON_SIGN_SLOT][3] = (int16_t)by1;

    neon_store_coverage(NEON_SIGN_SLOT, core, glow, cov_w, pad,
                        spr_w, spr_h, spr_stride, glyph_bytes);
}

/* Bake the three zone words as blurred coverage tiles.
 *
 * Mirrors neon_bake_glyph_sprites() but keeps each word on its OWN crop
 * rather than a shared anchor: there are only three, they differ hugely in
 * width, and a shared tile would be sized for OVERBOOST and mostly empty for
 * BOOST. The label box the live code positions against is
 * NEON_WORD_BOX_W x NEON_WORD_BOX_H, so the recorded offsets are measured
 * from that box's top-left corner and the draw side needs no knowledge of
 * where the ink landed inside it.
 */
static void neon_bake_zone_words(lv_obj_t *scr)
{
    if (s_neon_words_ready) return;   /* layout-independent; bake once, ever */
    const int safe_w = NEON_WORD_W + 40;
    const int safe_h = NEON_WORD_H + 40;
    const int box_x = (safe_w - NEON_WORD_BOX_W) / 2;
    const int box_y = 20;

    const uint32_t scratch_bytes =
        LV_CANVAS_BUF_SIZE(safe_w, safe_h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    void *scratch_buf = BG_ALLOC(scratch_bytes);
    if (scratch_buf == NULL) return;
    lv_obj_t *scratch = lv_canvas_create(scr);
    lv_canvas_set_buffer(scratch, scratch_buf, safe_w, safe_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);

    uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)NEON_WORD_W, LV_COLOR_FORMAT_A8);
    if (stride % LV_DRAW_BUF_ALIGN != 0) {
        stride += LV_DRAW_BUF_ALIGN - (stride % LV_DRAW_BUF_ALIGN);
    }
    const size_t word_bytes = (size_t)stride * (size_t)NEON_WORD_H;
    s_neon_word_buf = BG_ALLOC(word_bytes * NEON_WORD_COUNT);
    uint8_t *core = (uint8_t *)BG_ALLOC((size_t)safe_w * (size_t)safe_h);
    uint8_t *glow = (uint8_t *)BG_ALLOC((size_t)safe_w * (size_t)safe_h);
    if (s_neon_word_buf == NULL || core == NULL || glow == NULL ||
        safe_w > NEON_BLUR_MAXDIM || safe_h > NEON_BLUR_MAXDIM) {
        ESP_LOGW(TAG, "neon zone word bake alloc failed; words stay unglowed");
        BG_FREE(core); BG_FREE(glow);
        BG_FREE(s_neon_word_buf); s_neon_word_buf = NULL;
        lv_obj_delete(scratch);
        BG_FREE(scratch_buf);
        return;
    }
    memset(s_neon_word_buf, 0, word_bytes * NEON_WORD_COUNT);

    for (int wi = 0; wi < NEON_WORD_COUNT; ++wi) {
        neon_scratch_clear(scratch, 0, 0, safe_w, safe_h);
        lv_layer_t layer;
        lv_canvas_init_layer(scratch, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.font = NEON_LABEL;
        dsc.align = LV_TEXT_ALIGN_CENTER;
        dsc.color = lv_color_white();
        /* Must match the label styling this replaces, or the words shift. */
        dsc.letter_space = 2;
        dsc.text = k_neon_zone_words[wi];
        dsc.text_local = 0;
        lv_area_t box = { box_x, box_y,
                          box_x + NEON_WORD_BOX_W - 1, box_y + NEON_WORD_BOX_H - 1 };
        lv_draw_label(&layer, &dsc, &box);
        lv_canvas_finish_layer(scratch, &layer);

        const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
        memset(core, 0, (size_t)safe_w * (size_t)safe_h);
        int ix0 = safe_w, iy0 = safe_h, ix1 = -1, iy1 = -1;
        for (int y = 0; y < safe_h; ++y) {
            const lv_color16_t *row =
                (const lv_color16_t *)(db->data + (size_t)y * db->header.stride);
            for (int x = 0; x < safe_w; ++x) {
                const uint8_t v = (uint8_t)(row[x].red * 255 / 31);
                core[(size_t)y * (size_t)safe_w + (size_t)x] = v;
                if (v) {
                    if (x < ix0) ix0 = x;
                    if (x > ix1) ix1 = x;
                    if (y < iy0) iy0 = y;
                    if (y > iy1) iy1 = y;
                }
            }
        }
        if (ix1 < ix0) continue;

        memcpy(glow, core, (size_t)safe_w * (size_t)safe_h);
        neon_box_blur(glow, safe_w, safe_h, NEON_GLOW_BLUR_R);
        neon_box_blur(glow, safe_w, safe_h, NEON_GLOW_BLUR_R);

        int cx0 = ix0 - NEON_SPR_GLOW_MARGIN, cy0 = iy0 - NEON_SPR_GLOW_MARGIN;
        int cx1 = ix1 + NEON_SPR_GLOW_MARGIN, cy1 = iy1 + NEON_SPR_GLOW_MARGIN;
        if (cx0 < 0) cx0 = 0;
        if (cy0 < 0) cy0 = 0;
        if (cx1 > safe_w - 1) cx1 = safe_w - 1;
        if (cy1 > safe_h - 1) cy1 = safe_h - 1;
        cx0 &= ~(LV_DRAW_BUF_ALIGN - 1);
        int tw = cx1 - cx0 + 1, th = cy1 - cy0 + 1;
        if (tw > NEON_WORD_W) tw = NEON_WORD_W;
        if (th > NEON_WORD_H) th = NEON_WORD_H;

        uint8_t *dst_bytes = (uint8_t *)s_neon_word_buf + (size_t)wi * word_bytes;
        for (int y = 0; y < th; ++y) {
            uint8_t *dst = dst_bytes + (size_t)y * stride;
            for (int x = 0; x < tw; ++x) {
                const size_t si = (size_t)(cy0 + y) * (size_t)safe_w + (size_t)(cx0 + x);
                int g = (glow[si] * NEON_GLOW_GAIN_PCT) / 100;
                if (g > 255) g = 255;
                dst[x] = (uint8_t)(core[si] > g ? core[si] : g);
            }
        }
        lv_image_dsc_t *img = &s_neon_word_img[wi];
        memset(img, 0, sizeof(*img));
        img->header.magic = LV_IMAGE_HEADER_MAGIC;
        img->header.cf = LV_COLOR_FORMAT_A8;
        img->header.w = (uint32_t)tw;
        img->header.h = (uint32_t)th;
        img->header.stride = stride;
        img->data_size = (uint32_t)((size_t)stride * (size_t)th);
        img->data = (const uint8_t *)dst_bytes;
        s_neon_word_dx[wi] = (int16_t)(cx0 - box_x);
        s_neon_word_dy[wi] = (int16_t)(cy0 - box_y);
        s_neon_words_ready = true;
    }

    BG_FREE(core);
    BG_FREE(glow);
    lv_obj_delete(scratch);
    BG_FREE(scratch_buf);
}

/*
 * Bake all 12 readout glyphs plus the minus mark into one PSRAM block at the
 * active layout's font size, as 8-bit coverage planes. Called once per scene
 * build from build_neon(). Coverage is colour-independent, so a zone flip -
 * or any interpolated shade between zones - is a recolor on the blit, not a
 * rebake and not a second copy of the tiles.
 *
 * Geometry pass: render every glyph in a big scratch canvas exactly the way
 * draw_neon_live() draws it today (lv_draw_label(), LV_TEXT_ALIGN_CENTER,
 * same box height), and scan for the union ink bounding box. That union plus
 * NEON_SPR_GLOW_MARGIN on every side fixes ONE crop window, reused for every
 * glyph, so every tile shares one anchor and the tile size stays uniform
 * regardless of which character is narrower (e.g. '.', '1').
 *
 * Coverage pass: render every glyph once in white, read the anti-aliased
 * result straight off as an 8-bit coverage plane (a white-on-black render
 * already IS coverage), box-blur a copy of it twice for the glow, and store
 * max(core, glow * gain) as the tile. No colour enters the bake at all - it is
 * applied per blit as the A8 recolor.
 */

/* Marquee readout performance: bake the shared 118 px tiles at the marquee's
 * 0.87 scale ONCE per scene build so draw time never pays LVGL's per-frame
 * transform (host A/B: ~35-40% of every readout repaint). The scaled pixels
 * are produced by the SAME lv_draw_image call the old per-frame blit made
 * (A8 source, scale NEON_MARQUEE_CENTER_SCALE, pivot at the image centre,
 * recolor white) into a scratch RGB565 canvas, then the exact transformed
 * area - computed with the same integer corner math
 * lv_image_buf_get_transformed_area() uses - is copied out as a compact A8
 * tile. The destination offset uses the same expression the old blit used for
 * its coords origin, plus LVGL's own x1/y1 of the transformed area, so the
 * tile lands on exactly the pixels the per-frame transform used to paint.
 * Slot's whose crop is empty (no source ink) get no tile. */
static void neon_bake_scaled_sprites(lv_obj_t *scr, float scale)
{
    if (s_neon_glyph_buf_s != NULL) {
        BG_FREE(s_neon_glyph_buf_s);
        s_neon_glyph_buf_s = NULL;
    }
    memset(s_neon_glyph_img_s, 0, sizeof(s_neon_glyph_img_s));
    memset(s_neon_glyph_bbox_s, 0, sizeof(s_neon_glyph_bbox_s));

    const int scale_i = (int)lroundf(256.0f * scale);
    if (scale_i <= 0) return;

    /* Pass 1: per-slot tile dims (the transformed area of that slot's crop)
     * and packed buffer size. x1/y1 are LVGL's own integer corner mapping of
     * the crop corners through the pivot, exactly as
     * lv_image_buf_get_transformed_area() computes them. */
    int sw[NEON_SPRITE_COUNT], sh[NEON_SPRITE_COUNT];
    uint32_t stride[NEON_SPRITE_COUNT];
    size_t total = 0;
    int max_bw = 0, max_bh = 0;
    for (int slot = 0; slot < NEON_SPRITE_COUNT; ++slot) {
        const lv_image_dsc_t *src = &s_neon_glyph_img[slot];
        if (src->data == NULL) { sw[slot] = 0; sh[slot] = 0; stride[slot] = 0; continue; }
        const int bw = (int)src->header.w, bh = (int)src->header.h;
        const int x1 = (bw / 2) + (((0 - (bw / 2)) * scale_i) >> 8);
        const int x2 = (bw / 2) + (((bw - (bw / 2)) * scale_i) >> 8) - 1;
        const int y1 = (bh / 2) + (((0 - (bh / 2)) * scale_i) >> 8);
        const int y2 = (bh / 2) + (((bh - (bh / 2)) * scale_i) >> 8) - 1;
        sw[slot] = x2 - x1 + 1;
        sh[slot] = y2 - y1 + 1;
        stride[slot] = lv_draw_buf_width_to_stride((uint32_t)sw[slot], LV_COLOR_FORMAT_A8);
        if (sw[slot] > 0 && sh[slot] > 0) {
            total += (size_t)stride[slot] * (size_t)sh[slot];
            if (bw > max_bw) max_bw = bw;
            if (bh > max_bh) max_bh = bh;
        }
    }
    if (total == 0 || max_bw == 0 || max_bh == 0) return;
    s_neon_glyph_buf_s = BG_ALLOC(total);
    if (s_neon_glyph_buf_s == NULL) {
        ESP_LOGW(TAG, "neon scaled sprite alloc failed (%u B); keeping per-frame transform",
                 (unsigned)total);
        return;
    }

    /* Pass 2: transform each crop into one reused scratch canvas and copy the
     * exact transformed area into its packed tile. */
    const size_t canvas_bytes =
        LV_CANVAS_BUF_SIZE((uint32_t)max_bw, (uint32_t)max_bh, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    void *canvas_buf = BG_ALLOC(canvas_bytes);
    if (canvas_buf == NULL) {
        ESP_LOGW(TAG, "neon scaled sprite canvas alloc failed (%u B); keeping per-frame transform",
                 (unsigned)canvas_bytes);
        BG_FREE(s_neon_glyph_buf_s);
        s_neon_glyph_buf_s = NULL;
        return;
    }

    uint8_t *out = (uint8_t *)s_neon_glyph_buf_s;
    for (int slot = 0; slot < NEON_SPRITE_COUNT; ++slot) {
        const lv_image_dsc_t *src = &s_neon_glyph_img[slot];
        if (src->data == NULL || sw[slot] <= 0) continue;
        const int bw = (int)src->header.w, bh = (int)src->header.h;
        const int x1 = (bw / 2) + (((0 - (bw / 2)) * scale_i) >> 8);
        const int x2 = (bw / 2) + (((bw - (bw / 2)) * scale_i) >> 8) - 1;
        const int y1 = (bh / 2) + (((0 - (bh / 2)) * scale_i) >> 8);
        const int y2 = (bh / 2) + (((bh - (bh / 2)) * scale_i) >> 8) - 1;

        lv_obj_t *cv = lv_canvas_create(scr);
        lv_canvas_set_buffer(cv, canvas_buf, bw, bh, LV_COLOR_FORMAT_RGB565);
        lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
        lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
        for (int y = 0; y < bh; ++y)
            memset(db->data + (size_t)y * db->header.stride, 0, (size_t)bw * 2u);
        lv_layer_t layer;
        lv_canvas_init_layer(cv, &layer);
        lv_area_t full = { 0, 0, bw - 1, bh - 1 };
        lv_draw_image_dsc_t idsc; lv_draw_image_dsc_init(&idsc);
        idsc.src = src;
        idsc.opa = LV_OPA_COVER;
        idsc.recolor = lv_color_white();
        idsc.recolor_opa = LV_OPA_COVER;
        idsc.scale_x = idsc.scale_y = (uint16_t)scale_i;
        idsc.pivot.x = bw / 2;
        idsc.pivot.y = bh / 2;
        lv_draw_image(&layer, &idsc, &full);
        lv_canvas_finish_layer(cv, &layer);

        const lv_draw_buf_t *db2 = lv_canvas_get_draw_buf(cv);
        for (int y = y1; y <= y2; ++y) {
            const lv_color16_t *row =
                (const lv_color16_t *)(db2->data + (size_t)y * db2->header.stride);
            uint8_t *dst = out + (size_t)(y - y1) * stride[slot];
            for (int x = x1; x <= x2; ++x)
                dst[x - x1] = (uint8_t)(row[x].red * 255 / 31);
        }
        lv_obj_delete(cv);

        /* Destination offset from the blit anchor: the old per-frame coords
         * origin expression plus LVGL's transformed-area x1/y1, so the tile's
         * pixel 0 lands where the transformed crop used to start. */
        const int16_t *bb = s_neon_glyph_bbox[slot];
        s_neon_glyph_bbox_s[slot][0] =
            (int16_t)(lroundf(scale * (float)bb[0] + (scale - 1.0f) * (float)bw / 2.0f) + x1);
        s_neon_glyph_bbox_s[slot][1] =
            (int16_t)(lroundf(scale * (float)bb[1] + (scale - 1.0f) * (float)bh / 2.0f) + y1);

        lv_image_dsc_t *img = &s_neon_glyph_img_s[slot];
        memset(img, 0, sizeof(*img));
        img->header.magic = LV_IMAGE_HEADER_MAGIC;
        img->header.cf = LV_COLOR_FORMAT_A8;
        img->header.w = (uint32_t)sw[slot];
        img->header.h = (uint32_t)sh[slot];
        img->header.stride = stride[slot];
        img->data_size = (uint32_t)((size_t)stride[slot] * (size_t)sh[slot]);
        img->data = out;
        out += (size_t)stride[slot] * (size_t)sh[slot];
    }
    BG_FREE(canvas_buf);
}
static void neon_bake_glyph_sprites(lv_obj_t *scr)
{
    /* Already baked for this layout - reuse. See s_neon_sprite_layout for why
     * the layout alone is a sufficient key. */
    if (s_neon_glyph_sprites_ready && s_neon_sprite_layout == s_neon_layout) {
        s_neon_sprites_reused = true;
        return;
    }
    s_neon_sprites_reused = false;
    neon_free_glyph_sprites();

    const bool marquee = (s_neon_layout == BOOST_NEON_MARQUEE);
    (void)marquee; /* border art differs; the readout is shared now */
    const lv_font_t *font = NEON_BIG;
    const int font_px = NEON_FONT_PX;
    const int spr_w = NEON_SPR_BIG_W;
    const int spr_h = NEON_SPR_BIG_H;
    s_neon_spr_w = spr_w;
    s_neon_spr_h = spr_h;

    const int line_h = (int)lv_font_get_line_height(font);
    /* Wide enough that no glyph's ink can clip against the scratch canvas
     * edges regardless of its advance/bearing. Tall enough for the SAME
     * top-anchored draw (row 0 = the live draw's 'top', unaffected by making
     * the box taller) to leave room for the crop below the ink, with margin
     * on both sides: line_h + 8 matches the live draw's own box height, but
     * this font's line_h (its ACTUAL ascent+descent) turned out to be
     * shorter than the glyph-ink-plus-glow sprite height on the marquee
     * font, which pushed the crop's top clamp negative and painted glow
     * above the invalidated box - the exact bug this margin exists to rule
     * out, not just patch over for the font measured this session. */
    const int safe_w = font_px * 2;
    const int safe_h = line_h + 8 + 2 * spr_h;

    const uint32_t scratch_bytes =
        LV_CANVAS_BUF_SIZE(safe_w, safe_h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    void *scratch_buf = BG_ALLOC(scratch_bytes);
    if (scratch_buf == NULL) {
        ESP_LOGW(TAG, "neon glyph scratch alloc failed (%u B); using live labels",
                (unsigned)scratch_bytes);
        return;
    }
    /* A throwaway child of the scene root: never shown, deleted before this
     * function returns. */
    lv_obj_t *scratch = lv_canvas_create(scr);
    lv_canvas_set_buffer(scratch, scratch_buf, safe_w, safe_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);

    /* --- geometry pass: each glyph's own ink bbox, plus their union --- */
    int own_x0[NEON_GLYPH_COUNT], own_y0[NEON_GLYPH_COUNT];
    int own_x1[NEON_GLYPH_COUNT], own_y1[NEON_GLYPH_COUNT];
    int ux0 = safe_w, uy0 = safe_h, ux1 = -1, uy1 = -1;
    for (int gi = 0; gi < NEON_GLYPH_COUNT; ++gi) {
        own_x0[gi] = safe_w; own_y0[gi] = safe_h; own_x1[gi] = -1; own_y1[gi] = -1;
        const char ch = k_neon_glyph_chars[gi];
        if (ch == '-') continue; /* zero-contour in this typeface: nothing to bound */
        /* The geometry pass does not yet know where the ink will land - that
         * is what this scan is for - so it has to clear the WHOLE canvas,
         * unlike the colour pass below. Still a plain memset rather than
         * lv_draw_rect(), which avoids that call's style/blend dispatch for
         * what is just a black fill. */
        neon_scratch_clear(scratch, 0, 0, safe_w, safe_h);
        lv_layer_t layer;
        lv_canvas_init_layer(scratch, &layer);
        lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
        dsc.font = font; dsc.align = LV_TEXT_ALIGN_CENTER; dsc.color = lv_color_white();
        char text[2] = { ch, 0 };
        dsc.text = text; dsc.text_local = 1;
        lv_area_t full = { 0, 0, safe_w - 1, safe_h - 1 };
        lv_draw_label(&layer, &dsc, &full);
        lv_canvas_finish_layer(scratch, &layer);

        const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
        for (int y = 0; y < safe_h; ++y) {
            const lv_color16_t *row =
                (const lv_color16_t *)(db->data + (size_t)y * db->header.stride);
            for (int x = 0; x < safe_w; ++x) {
                if (row[x].red || row[x].green || row[x].blue) {
                    if (x < ux0) ux0 = x;
                    if (x > ux1) ux1 = x;
                    if (y < uy0) uy0 = y;
                    if (y > uy1) uy1 = y;
                    if (x < own_x0[gi]) own_x0[gi] = x;
                    if (x > own_x1[gi]) own_x1[gi] = x;
                    if (y < own_y0[gi]) own_y0[gi] = y;
                    if (y > own_y1[gi]) own_y1[gi] = y;
                }
            }
        }
    }
    if (ux1 < ux0 || uy1 < uy0) {
        ESP_LOGW(TAG, "neon glyph geometry scan found no ink; using live labels");
        lv_obj_delete(scratch);
        BG_FREE(scratch_buf);
        return;
    }

    int cx0 = (ux0 + ux1) / 2 - spr_w / 2;
    int cy0 = uy0 - NEON_SPR_GLOW_MARGIN;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx0 + spr_w > safe_w) cx0 = safe_w - spr_w;
    if (cy0 + spr_h > safe_h) cy0 = safe_h - spr_h;
    if ((ux1 - ux0 + 1) + 2 * NEON_SPR_GLOW_MARGIN > spr_w ||
        (uy1 - uy0 + 1) + 2 * NEON_SPR_GLOW_MARGIN > spr_h) {
        ESP_LOGW(TAG, "neon glyph ink bbox %dx%d exceeds the %dx%d sprite budget",
                ux1 - ux0 + 1, uy1 - uy0 + 1, spr_w, spr_h);
    }
    /* draw_neon_live() draws the label with LV_TEXT_ALIGN_CENTER into a box
     * whose CENTRE sits at (cx + cell.x); this scratch box's centre is at
     * safe_w/2, and its top row is the live draw's 'top'. So a sprite blit at
     * (cx + cell.x + dx, top + dy) reproduces the same pixels the crop came
     * from, without needing the box width/height to match. */
    s_neon_spr_dx = cx0 - safe_w / 2;
    s_neon_spr_dy = cy0;

    /* Per-glyph tight bbox, tile-local, own ink + glow margin - see the
     * static array's comment for why this (not the full tile) is what gets
     * blitted: cell pitch can be narrower than the tile. */
    for (int gi = 0; gi < NEON_GLYPH_COUNT; ++gi) {
        int bx0, by0, bx1, by1;
        if (own_x1[gi] < own_x0[gi]) {
            /* No ink (the '-' entry): an arbitrary 1x1 tile-local pixel.
             * Never blitted at draw time - neon_glyph_index() is only asked
             * about characters that actually appear in a cell. */
            bx0 = by0 = 0; bx1 = by1 = 0;
        } else {
            const int margin = neon_glyph_glow_margin(gi);
            bx0 = own_x0[gi] - margin - cx0;
            by0 = own_y0[gi] - margin - cy0;
            bx1 = own_x1[gi] + margin - cx0;
            by1 = own_y1[gi] + margin - cy0;
            if (bx0 < 0) bx0 = 0;
            if (by0 < 0) by0 = 0;
            if (bx1 > spr_w - 1) bx1 = spr_w - 1;
            if (by1 > spr_h - 1) by1 = spr_h - 1;
            /* Round the top-left corner down to an aligned pixel. The window's
             * data pointer is tile_base + by0*stride + bx0 bytes (A8 is one
             * byte per pixel), so rounding bx0 down to a multiple of
             * LV_DRAW_BUF_ALIGN keeps that offset alignment-clean against the
             * likewise-rounded stride below, and LVGL's mask path never falls
             * back to its unaligned case. Costs at most a few columns/rows of
             * harmless zero-coverage padding, which now blends to nothing
             * instead of painting black. */
            bx0 &= ~(LV_DRAW_BUF_ALIGN - 1);
            by0 &= ~1;
        }
        s_neon_glyph_bbox[gi][0] = (int16_t)bx0;
        s_neon_glyph_bbox[gi][1] = (int16_t)by0;
        s_neon_glyph_bbox[gi][2] = (int16_t)bx1;
        s_neon_glyph_bbox[gi][3] = (int16_t)by1;
    }

    /* Rounded up to LV_DRAW_BUF_ALIGN so that, combined with the aligned
     * bx0/by0 above, every windowed sub-image's data pointer lands aligned -
     * see the comment on the bbox rounding for why this matters for staying on
     * LVGL's fast mask path. */
    uint32_t spr_stride = lv_draw_buf_width_to_stride((uint32_t)spr_w, LV_COLOR_FORMAT_A8);
    if (spr_stride % LV_DRAW_BUF_ALIGN != 0) {
        spr_stride += LV_DRAW_BUF_ALIGN - (spr_stride % LV_DRAW_BUF_ALIGN);
    }
    s_neon_spr_stride = spr_stride;
    const size_t glyph_bytes = (size_t)spr_stride * (size_t)spr_h;
    /* No zone dimension: coverage is colour-independent, so one plane per
     * sprite serves all three zones and any interpolated shade between them. */
    const size_t total_bytes = glyph_bytes * (size_t)NEON_SPRITE_COUNT;
    s_neon_glyph_buf = BG_ALLOC(total_bytes);
    if (s_neon_glyph_buf == NULL) {
        ESP_LOGW(TAG, "neon glyph sprite alloc failed (%u B); using live labels",
                (unsigned)total_bytes);
        lv_obj_delete(scratch);
        BG_FREE(scratch_buf);
        return;
    }
    memset(s_neon_glyph_buf, 0, total_bytes);

    const int pad = 6; /* blur halo around the tile; kept > NEON_GLOW_BLUR_R so
                         * the box blur's edge samples come from real source
                         * pixels instead of the clamp, and still well inside
                         * the 14 px NEON_SPR_GLOW_MARGIN over 2 passes. */
    const int cov_w = spr_w + 2 * pad;
    const int cov_h = spr_h + 2 * pad;
    uint8_t *core = (uint8_t *)BG_ALLOC((size_t)cov_w * (size_t)cov_h);
    uint8_t *glow = (uint8_t *)BG_ALLOC((size_t)cov_w * (size_t)cov_h);
    if (core == NULL || glow == NULL || cov_w > NEON_BLUR_MAXDIM || cov_h > NEON_BLUR_MAXDIM) {
        ESP_LOGW(TAG, "neon glyph coverage scratch alloc failed; using live labels");
        BG_FREE(core); BG_FREE(glow);
        BG_FREE(s_neon_glyph_buf); s_neon_glyph_buf = NULL;
        lv_obj_delete(scratch);
        BG_FREE(scratch_buf);
        return;
    }
    ESP_LOGI(TAG, "neon glyph sprites: %dx%d A8 tile x %d sprites = %u B",
             spr_w, spr_h, NEON_SPRITE_COUNT, (unsigned)total_bytes);

    /* Coverage-pass clear window: every glyph's own ink is a SUBSET of the
     * union bbox the geometry pass found (own_x0/y0/x1/y1 above are each
     * bounded by ux0/uy0/ux1/uy1 by construction), and the union in turn is
     * what sized and centred this crop tile - so this window, expanded by
     * `pad` for the blur and clamped to the canvas, is guaranteed to contain
     * every glyph's rendered ink for every zone. Computed once, outside the
     * z/gi loop below, since cx0/cy0/pad/cov_w/cov_h never change per glyph. */
    int cwx0 = cx0 - pad, cwy0 = cy0 - pad;
    int cwx1 = cx0 - pad + cov_w, cwy1 = cy0 - pad + cov_h;
    if (cwx0 < 0) cwx0 = 0;
    if (cwy0 < 0) cwy0 = 0;
    if (cwx1 > safe_w) cwx1 = safe_w;
    if (cwy1 > safe_h) cwy1 = safe_h;

    for (int gi = 0; gi < NEON_GLYPH_COUNT; ++gi) {
            lv_layer_t layer;
            lv_canvas_init_layer(scratch, &layer);
            neon_scratch_clear(scratch, cwx0, cwy0, cwx1 - cwx0, cwy1 - cwy0);
            lv_area_t full = { 0, 0, safe_w - 1, safe_h - 1 };
            lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
            dsc.font = font; dsc.align = LV_TEXT_ALIGN_CENTER; dsc.color = lv_color_white();
            char text[2] = { k_neon_glyph_chars[gi], 0 };
            dsc.text = text; dsc.text_local = 1;
            lv_draw_label(&layer, &dsc, &full);
            lv_canvas_finish_layer(scratch, &layer);

            const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
            memset(core, 0, (size_t)cov_w * (size_t)cov_h);
            for (int y = 0; y < cov_h; ++y) {
                const int sy = cy0 - pad + y;
                if (sy < 0 || sy >= safe_h) continue;
                const lv_color16_t *row =
                    (const lv_color16_t *)(db->data + (size_t)sy * db->header.stride);
                for (int x = 0; x < cov_w; ++x) {
                    const int sx = cx0 - pad + x;
                    if (sx < 0 || sx >= safe_w) continue;
                    core[(size_t)y * (size_t)cov_w + (size_t)x] =
                        (uint8_t)(row[sx].red * 255 / 31);
                }
            }
            memcpy(glow, core, (size_t)cov_w * (size_t)cov_h);
            neon_box_blur(glow, cov_w, cov_h, NEON_GLOW_BLUR_R);
            neon_box_blur(glow, cov_w, cov_h, NEON_GLOW_BLUR_R);

            neon_store_coverage(gi, core, glow, cov_w, pad, spr_w, spr_h,
                                spr_stride, glyph_bytes);
    }

    neon_bake_sign_sprite(scratch, safe_w, safe_h, cx0, cy0, font_px,
                          NEON_SIGN_W,
                          core, glow, cov_w, cov_h, pad,
                          spr_w, spr_h, spr_stride, glyph_bytes);

    if (marquee) {
        neon_bake_scaled_sprites(scr, NEON_MARQUEE_CENTER_SCALE);
    }

    BG_FREE(core);
    BG_FREE(glow);
    lv_obj_delete(scratch);
    BG_FREE(scratch_buf);
    s_neon_glyph_sprites_ready = true;
    s_neon_sprite_layout = s_neon_layout;
}

/* Bake the segments layout's three lit-band coverage tiles for every segment.
 *
 * Each band is rendered with the EXACT arc the live fallback path draws
 * (same radius, width and angular extent - the same lv_draw_arc call, white
 * instead of the accent-derived colour) into a scratch canvas whose (0,0) is
 * the band's bounding box, so the coverage is whatever the rasteriser actually
 * painted and the blit lands on those same pixels. The ink bbox crop means
 * the tile is exactly the painted extent (including the AA fringe) and the
 * stored offset (face centre -> tile top-left) reproduces the position at any
 * burn-in shift.
 *
 * Allocation failure clears the whole set: the draw path checks
 * s_neon_seg_sprites_ready before blitting and otherwise keeps the live arcs
 * unchanged, so a failed bake degrades gracefully to the current renderer. */
static void neon_bake_seg_sprites(lv_obj_t *scr)
{
    if (s_neon_seg_sprites_ready) return;
    if (s_neon_seg_buf != NULL) {
        BG_FREE(s_neon_seg_buf);
        s_neon_seg_buf = NULL;
    }
    memset(s_neon_seg_tile, 0, sizeof(s_neon_seg_tile));

    const int nom_cx = DISP_SIZE / 2, nom_cy = DISP_SIZE / 2;

    /* Pass 1: windowed dims per band tile and the packed total. The window is
     * the band's bbox inflated by NEON_SEG_SPR_MARGIN so no rasterised pixel
     * can be clipped by the scratch edge; the crop below is the actual ink. */
    size_t total = 0;
    int max_w = 0, max_h = 0;
    for (int i = 0; i < NEON_NSEG; ++i) {
        const float s = neon_seg_start(i);
        for (int b = 0; b < 3; ++b) {
            lv_area_t bb;
            lv_draw_arc_get_area(nom_cx, nom_cy,
                                 (uint16_t)k_neon_seg_band_geom[b].radius,
                                 s, s + NEON_SEG_LIT,
                                 (uint16_t)k_neon_seg_band_geom[b].width,
                                 false, &bb);
            const int w = bb.x2 - bb.x1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
            const int h = bb.y2 - bb.y1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
            uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_A8);
            if (stride % LV_DRAW_BUF_ALIGN != 0)
                stride += LV_DRAW_BUF_ALIGN - (stride % LV_DRAW_BUF_ALIGN);
            total += (size_t)stride * (size_t)h;
            if (w > max_w) max_w = w;
            if (h > max_h) max_h = h;
        }
    }
    if (max_w <= 0 || max_h <= 0) return;

    s_neon_seg_buf = BG_ALLOC(total);
    if (s_neon_seg_buf == NULL) {
        ESP_LOGW(TAG, "neon segment band sprite alloc failed (%u B); keeping live arcs",
                 (unsigned)total);
        return;
    }
    memset(s_neon_seg_buf, 0, total);

    const uint32_t scratch_bytes =
        LV_CANVAS_BUF_SIZE((uint32_t)max_w, (uint32_t)max_h, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    void *scratch_buf = BG_ALLOC(scratch_bytes);
    if (scratch_buf == NULL) {
        ESP_LOGW(TAG, "neon segment band scratch alloc failed (%u B); keeping live arcs",
                 (unsigned)scratch_bytes);
        BG_FREE(s_neon_seg_buf);
        s_neon_seg_buf = NULL;
        return;
    }
    lv_obj_t *scratch = lv_canvas_create(scr);
    lv_canvas_set_buffer(scratch, scratch_buf, max_w, max_h, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);

    uint8_t *out = (uint8_t *)s_neon_seg_buf;
    for (int i = 0; i < NEON_NSEG; ++i) {
        const float s = neon_seg_start(i);
        for (int b = 0; b < 3; ++b) {
            lv_area_t bb;
            lv_draw_arc_get_area(nom_cx, nom_cy,
                                 (uint16_t)k_neon_seg_band_geom[b].radius,
                                 s, s + NEON_SEG_LIT,
                                 (uint16_t)k_neon_seg_band_geom[b].width,
                                 false, &bb);
            const int wx0 = bb.x1 - NEON_SEG_SPR_MARGIN;
            const int wy0 = bb.y1 - NEON_SEG_SPR_MARGIN;
            const int win_w = bb.x2 - bb.x1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
            const int win_h = bb.y2 - bb.y1 + 1 + 2 * NEON_SEG_SPR_MARGIN;

            /* White arc at the translated centre: scratch (0,0) is screen
             * (wx0, wy0), so the coverage pixels land exactly where the live
             * arc paints them. */
            neon_scratch_clear(scratch, 0, 0, max_w, max_h);
            lv_layer_t layer;
            lv_canvas_init_layer(scratch, &layer);
            lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
            arc.center.x = nom_cx - wx0;
            arc.center.y = nom_cy - wy0;
            arc.radius = (uint16_t)k_neon_seg_band_geom[b].radius;
            arc.width = (uint16_t)k_neon_seg_band_geom[b].width;
            arc.start_angle = s;
            arc.end_angle = s + NEON_SEG_LIT;
            arc.color = lv_color_white();
            arc.opa = LV_OPA_COVER;
            lv_draw_arc(&layer, &arc);
            lv_canvas_finish_layer(scratch, &layer);

            const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);
            int ix0 = max_w, iy0 = max_h, ix1 = -1, iy1 = -1;
            for (int y = 0; y < win_h; ++y) {
                const lv_color16_t *row =
                    (const lv_color16_t *)(db->data + (size_t)y * db->header.stride);
                for (int x = 0; x < win_w; ++x) {
                    if (row[x].red) {
                        if (x < ix0) ix0 = x;
                        if (x > ix1) ix1 = x;
                        if (y < iy0) iy0 = y;
                        if (y > iy1) iy1 = y;
                    }
                }
            }
            if (ix1 < ix0 || iy1 < iy0) continue;   /* no ink: tile stays NULL */

            const int bw = ix1 - ix0 + 1, bh = iy1 - iy0 + 1;
            uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)win_w, LV_COLOR_FORMAT_A8);
            if (stride % LV_DRAW_BUF_ALIGN != 0)
                stride += LV_DRAW_BUF_ALIGN - (stride % LV_DRAW_BUF_ALIGN);
            uint8_t *tile = out;
            for (int y = 0; y < bh; ++y) {
                const lv_color16_t *row =
                    (const lv_color16_t *)(db->data + (size_t)(iy0 + y) * db->header.stride);
                uint8_t *dst = tile + (size_t)y * stride;
                for (int x = 0; x < bw; ++x)
                    dst[x] = (uint8_t)(row[ix0 + x].red * 255 / 31);
            }
            out += (size_t)stride * (size_t)bh;

            neon_seg_band_tile_t *t = &s_neon_seg_tile[i][b];
            t->img.header.magic = LV_IMAGE_HEADER_MAGIC;
            t->img.header.cf = LV_COLOR_FORMAT_A8;
            t->img.header.w = (uint32_t)bw;
            t->img.header.h = (uint32_t)bh;
            t->img.header.stride = stride;
            t->img.data_size = stride * (uint32_t)bh;
            t->img.data = tile;
            t->off_x = (int16_t)(wx0 + ix0 - nom_cx);
            t->off_y = (int16_t)(wy0 + iy0 - nom_cy);
        }
    }
    lv_obj_delete(scratch);
    BG_FREE(scratch_buf);
    s_neon_seg_sprites_ready = true;
    ESP_LOGI(TAG, "neon segment band sprites: %d tiles, %u B",
             NEON_NSEG * 3, (unsigned)total);
}

/* Blit one baked band tile as `ink` at its baked position. A NULL tile or a
 * dirty region that misses the tile is a no-op, so a partial bake or a region
 * far from the ring degrades exactly like the arc path did. */
static void neon_blit_seg_band(lv_layer_t *layer, int cx, int cy,
                               int seg, int band, uint32_t ink)
{
    const neon_seg_band_tile_t *tile = &s_neon_seg_tile[seg][band];
    if (tile->img.data == NULL) return;
    const int x = cx + tile->off_x;
    const int y = cy + tile->off_y;
    const int bw = (int)tile->img.header.w, bh = (int)tile->img.header.h;
    lv_area_t a = { x, y, x + bw - 1, y + bh - 1 };
    if (!neon_area_overlaps(&a, &layer->_clip_area)) return;
    NEON_STAT_SPRITE();
    lv_draw_image_dsc_t idsc; lv_draw_image_dsc_init(&idsc);
    idsc.src = &tile->img;
    idsc.opa = LV_OPA_COVER;
    /* A8 source: LVGL blends `recolor` through the plane as the ink. */
    idsc.recolor = c(ink);
    idsc.recolor_opa = LV_OPA_COVER;
    lv_draw_image(layer, &idsc, &a);
}

/* Clip-skip boxes for the tube run tiles, anchored at the CURRENT face
 * centre, so they must be rebuilt on scene build and on pixel shift exactly
 * like the segment boxes. Full band depth, plus the tile overlap. */
static void neon_build_tube_boxes(void)
{
    if (!s_neon_tube_sprites_ready) return;
    const float tstep = NEON_TUBE_TILE_STEP;
    const float ov = NEON_TUBE_TILE_OVERLAP;
    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START,
                                      (float)(ARC_START + ARC_RANGE));
    for (int dir = 0; dir < 2; ++dir) {
        const float d = dir ? -1.0f : 1.0f;
        for (int k = 0; k < NEON_TUBE_TILES; ++k) {
            const float s0 = a_zero + d * ((float)k * tstep - ov);
            const float s1 = a_zero + d * (((float)k + 1.0f) * tstep + ov);
            lv_draw_arc_get_area(px_icx(), px_icy(), (uint16_t)NEON_R,
                                 fminf(s0, s1), fmaxf(s0, s1),
                                 (uint16_t)NEON_TUBE_BAND_DEPTH,
                                 false, &s_neon_tube_box[dir][k]);
            /* Same fringe inflation as the segment boxes: a region reaching
             * only the tile's AA fringe must still overlap the box or the
             * tile is skipped and the canvas repaints the fringe black. */
            s_neon_tube_box[dir][k].x1 -= NEON_BAND_SPR_MARGIN;
            s_neon_tube_box[dir][k].y1 -= NEON_BAND_SPR_MARGIN;
            s_neon_tube_box[dir][k].x2 += NEON_BAND_SPR_MARGIN;
            s_neon_tube_box[dir][k].y2 += NEON_BAND_SPR_MARGIN;
        }
    }
}

/* Tube layout only: bake the run's A8 coverage as a_zero-aligned wedge tiles,
 * direction 0 counterclockwise from the notch (boost side), 1 clockwise
 * (vacuum side). Each tile is NEON_TUBE_TILE_STEP wide and carries the band's
 * PURE RADIAL AA with NO angular AA: each band is baked ONCE as a full
 * 360-degree ring (lv_draw_arc start=0 end=360 draws it as a border with only
 * radial edges), and every tile of that band is copied from that one ring,
 * keeping only pixels whose CENTER lies inside the tile's half-open wedge.
 * Because the tiles carry no angular AA they tile EXACTLY - every pixel in
 * the run is painted by exactly one tile at the full radial coverage, so
 * there is no src-over dip at tile boundaries and the composite is
 * independent of which tiles the run currently blits (a dirty region
 * bisecting a boundary repaints one side correctly). The run's true angular
 * edges stay live: the moving tip is a live arc in draw_neon_live() and the
 * zero notch is covered by the baked white marker. Geometry is
 * k_neon_tube_band_geom - the exact arcs the tip draws. Lifecycle and
 * failure mode mirror neon_bake_seg_sprites: a failed bake keeps the live
 * arcs. */
static void neon_bake_tube_sprites(lv_obj_t *scr)
{
    if (s_neon_tube_sprites_ready) return;
    if (s_neon_tube_buf != NULL) {
        BG_FREE(s_neon_tube_buf);
        s_neon_tube_buf = NULL;
    }
    memset(s_neon_tube_tile, 0, sizeof(s_neon_tube_tile));

    const float tstep = NEON_TUBE_TILE_STEP;
    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START,
                                      (float)(ARC_START + ARC_RANGE));
    const int nom_cx = DISP_SIZE / 2, nom_cy = DISP_SIZE / 2;

    /* Pass 1: windowed dims per band tile and the packed total, exactly as the
     * segment bake does - the window is the tile's nominal wedge bbox inflated
     * by NEON_SEG_SPR_MARGIN and the crop below is the actual ink. */
    size_t total = 0;
    int max_w = 0, max_h = 0;
    for (int dir = 0; dir < 2; ++dir) {
        const float d = dir ? -1.0f : 1.0f;
        for (int k = 0; k < NEON_TUBE_TILES; ++k) {
            const float lo = a_zero + d * ((float)k * tstep);
            const float hi = a_zero + d * (((float)k + 1.0f) * tstep);
            for (int b = 0; b < 4; ++b) {
                lv_area_t bb;
                lv_draw_arc_get_area(nom_cx, nom_cy,
                                     (uint16_t)k_neon_tube_band_geom[b].radius,
                                     fminf(lo, hi), fmaxf(lo, hi),
                                     (uint16_t)k_neon_tube_band_geom[b].width,
                                     false, &bb);
                const int w = bb.x2 - bb.x1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
                const int h = bb.y2 - bb.y1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
                uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_A8);
                if (stride % LV_DRAW_BUF_ALIGN != 0)
                    stride += LV_DRAW_BUF_ALIGN - (stride % LV_DRAW_BUF_ALIGN);
                total += (size_t)stride * (size_t)h;
                if (w > max_w) max_w = w;
                if (h > max_h) max_h = h;
            }
        }
    }
    if (max_w <= 0 || max_h <= 0) return;
    if (max_w > 128 || max_h > 128) return;   /* window bound for the stack */

    s_neon_tube_buf = BG_ALLOC(total);
    if (s_neon_tube_buf == NULL) {
        ESP_LOGW(TAG, "neon tube band sprite alloc failed (%u B); keeping live arcs",
                 (unsigned)total);
        return;
    }
    memset(s_neon_tube_buf, 0, total);

    /* The ring bake needs the whole panel (the ring spans 233+-228 px). This
     * is temporary, freed below, like the per-window scratch the segment bake
     * uses. */
    const uint32_t scratch_bytes =
        LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    void *scratch_buf = BG_ALLOC(scratch_bytes);
    if (scratch_buf == NULL) {
        ESP_LOGW(TAG, "neon tube band scratch alloc failed (%u B); keeping live arcs",
                 (unsigned)scratch_bytes);
        BG_FREE(s_neon_tube_buf);
        s_neon_tube_buf = NULL;
        return;
    }
    lv_obj_t *scratch = lv_canvas_create(scr);
    lv_canvas_set_buffer(scratch, scratch_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(scratch, LV_OBJ_FLAG_HIDDEN);

    uint8_t *out = (uint8_t *)s_neon_tube_buf;
    uint8_t covwin[128 * 128];
    for (int b = 0; b < 4; ++b) {
        /* Bake the band once as a full ring - only radial AA, no angular
         * edges - then extract every tile of that band from the one ring. */
        neon_scratch_clear(scratch, 0, 0, DISP_SIZE, DISP_SIZE);
        lv_layer_t layer;
        lv_canvas_init_layer(scratch, &layer);
        lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
        arc.center.x = nom_cx;
        arc.center.y = nom_cy;
        arc.radius = (uint16_t)k_neon_tube_band_geom[b].radius;
        arc.width = (uint16_t)k_neon_tube_band_geom[b].width;
        arc.start_angle = 0;
        arc.end_angle = 360;
        arc.color = lv_color_white();
        arc.opa = LV_OPA_COVER;
        lv_draw_arc(&layer, &arc);
        lv_canvas_finish_layer(scratch, &layer);
        const lv_draw_buf_t *db = lv_canvas_get_draw_buf(scratch);

        for (int dir = 0; dir < 2; ++dir) {
            const float d = dir ? -1.0f : 1.0f;
            for (int k = 0; k < NEON_TUBE_TILES; ++k) {
                const float lo = a_zero + d * ((float)k * tstep);
                const float hi = a_zero + d * (((float)k + 1.0f) * tstep);
                const float c_lo = cosf(lo), s_lo = sinf(lo);
                const float c_hi = cosf(hi), s_hi = sinf(hi);
                lv_area_t bb;
                lv_draw_arc_get_area(nom_cx, nom_cy,
                                     (uint16_t)k_neon_tube_band_geom[b].radius,
                                     fminf(lo, hi), fmaxf(lo, hi),
                                     (uint16_t)k_neon_tube_band_geom[b].width,
                                     false, &bb);
                const int wx0 = bb.x1 - NEON_SEG_SPR_MARGIN;
                const int wy0 = bb.y1 - NEON_SEG_SPR_MARGIN;
                const int win_w = bb.x2 - bb.x1 + 1 + 2 * NEON_SEG_SPR_MARGIN;
                const int win_h = bb.y2 - bb.y1 + 1 + 2 * NEON_SEG_SPR_MARGIN;

                /* Fill the window from the ring, keeping only pixels whose
                 * CENTER is inside the tile's half-open wedge (the CCW arc
                 * from lo to hi), so no pixel belongs to two tiles.
                 * cross(a, p) = c*dy - s*dx is >= 0 when the pixel is CCW of
                 * the line; the pixel is inside iff it is CCW of the lo line
                 * AND CW (before) the hi line. */
                int ix0 = win_w, iy0 = win_h, ix1 = -1, iy1 = -1;
                for (int y = 0; y < win_h; ++y) {
                    const int py = wy0 + y;
                    const float dy = (float)(py - nom_cy);
                    const lv_color16_t *rrow =
                        (const lv_color16_t *)(db->data + (size_t)py * db->header.stride);
                    uint8_t *cv = &covwin[(size_t)y * (size_t)win_w];
                    for (int x = 0; x < win_w; ++x) {
                        const int px = wx0 + x;
                        const float dx = (float)(px - nom_cx);
                        const float cr_lo = c_lo * dy - s_lo * dx;
                        const float cr_hi = dx * s_hi - dy * c_hi;
                        const uint8_t cov = (cr_lo < 0.0f || cr_hi < 0.0f)
                            ? 0 : (uint8_t)(rrow[px].red * 255 / 31);
                        cv[x] = cov;
                        if (cov) {
                            if (x < ix0) ix0 = x;
                            if (x > ix1) ix1 = x;
                            if (y < iy0) iy0 = y;
                            if (y > iy1) iy1 = y;
                        }
                    }
                }
                if (ix1 < ix0 || iy1 < iy0) continue;   /* no ink: tile stays NULL */

                const int bw = ix1 - ix0 + 1, bh = iy1 - iy0 + 1;
                uint32_t stride = lv_draw_buf_width_to_stride((uint32_t)win_w, LV_COLOR_FORMAT_A8);
                if (stride % LV_DRAW_BUF_ALIGN != 0)
                    stride += LV_DRAW_BUF_ALIGN - (stride % LV_DRAW_BUF_ALIGN);
                uint8_t *tile = out;
                for (int y = 0; y < bh; ++y) {
                    const uint8_t *src = &covwin[(size_t)(iy0 + y) * (size_t)win_w + (size_t)ix0];
                    memcpy(tile + (size_t)y * stride, src, (size_t)bw);
                }
                out += (size_t)stride * (size_t)bh;

                neon_seg_band_tile_t *t = &s_neon_tube_tile[dir][k][b];
                t->img.header.magic = LV_IMAGE_HEADER_MAGIC;
                t->img.header.cf = LV_COLOR_FORMAT_A8;
                t->img.header.w = (uint32_t)bw;
                t->img.header.h = (uint32_t)bh;
                t->img.header.stride = stride;
                t->img.data_size = stride * (uint32_t)bh;
                t->img.data = tile;
                t->off_x = (int16_t)(wx0 + ix0 - nom_cx);
                t->off_y = (int16_t)(wy0 + iy0 - nom_cy);
            }
        }
    }
    lv_obj_delete(scratch);
    BG_FREE(scratch_buf);
    s_neon_tube_sprites_ready = true;
    neon_build_tube_boxes();
    ESP_LOGI(TAG, "neon tube band sprites: %d tiles, %u B",
             2 * NEON_TUBE_TILES * 3, (unsigned)total);
}

/* Blit one baked tube wedge tile as `ink` at its baked position. A NULL tile
 * or a dirty region that misses the tile is a no-op, exactly like the segment
 * band blit. */
static void neon_blit_tube_tile(lv_layer_t *layer, int cx, int cy,
                                int dir, int k, int band, uint32_t ink)
{
    const neon_seg_band_tile_t *tile = &s_neon_tube_tile[dir][k][band];
    if (tile->img.data == NULL) return;
    const int x = cx + tile->off_x;
    const int y = cy + tile->off_y;
    const int bw = (int)tile->img.header.w, bh = (int)tile->img.header.h;
    lv_area_t a = { x, y, x + bw - 1, y + bh - 1 };
    if (!neon_area_overlaps(&a, &layer->_clip_area)) return;
    NEON_STAT_SPRITE();
    lv_draw_image_dsc_t idsc; lv_draw_image_dsc_init(&idsc);
    idsc.src = &tile->img;
    idsc.opa = LV_OPA_COVER;
    /* A8 source: LVGL blends `recolor` through the plane as the ink. */
    idsc.recolor = c(ink);
    idsc.recolor_opa = LV_OPA_COVER;
    lv_draw_image(layer, &idsc, &a);
}

/* Tube layout only: bake one 2*NEON_R (456x456) RGB565 radial-gradient image
 * for the lit run, drawn by draw_neon_live() as a single lv_draw_arc whose
 * img_src is this image instead of the three stacked live band arcs. The
 * three abutting bands (dim / body / white cap) are drawn as full rings with
 * the EXACT geometry k_neon_tube_band_geom spells out - the same geometry the
 * live arcs use - so the image's pixels along the radius are identical to what
 * the live arcs paint, and the arc's own angular + radial masks in the live
 * draw supply the run's edges. Drawing the run then costs ONE arc mask pass
 * instead of three, on every tube frame (zone flips recolour the whole run, so
 * those were the ~3x-rasterisation stutters this is attacking).
 *
 * Each image depends on its zone's accent colour only, so ALL THREE are baked
 * once at scene build (guarded by s_neon_grad_key[zone]) and a zone flip just
 * SWITCHES which image the run repaint uses - a flip never rebuilds, or it
 * would re-pay the three full-ring rasterisations this feature exists to
 * avoid. The bake runs from scene build only - NEVER from inside a draw
 * callback (canvas finish_layer inside a draw is unsafe). Allocation failure
 * keeps the live arcs as the fallback. The web mirror intentionally stays on
 * the live three-band arcs: this is a firmware rasterisation optimisation with
 * identical band colours, not a visual change. */
static void neon_bake_grad(lv_obj_t *scr, int zone_id, uint32_t accent_rgb)
{
    if (zone_id < 0 || zone_id >= NEON_GRAD_ZONES) return;
    if (s_neon_grad_buf[zone_id] != NULL && s_neon_grad_key[zone_id] == accent_rgb) return;
    /* The image is NEON_R + NEON_GRAD_MARGIN either side of the centre so the
     * baked ring's own outer/inner AA (which spans r=NEON_R and its 1 px
     * fringe) fits fully INSIDE the square. A bare 2*NEON_R image is one
     * pixel short at the axes (its last pixel sits at r=NEON_R-1) so the cap
     * ring's outer AA was cut off at the four axis directions while the
     * diagonals had room - the outer edge read ragged/pixelated in spots. */
    const int size = 2 * (NEON_R + NEON_GRAD_MARGIN);
    const uint32_t bytes = LV_CANVAS_BUF_SIZE((uint32_t)size, (uint32_t)size, 16,
                                              LV_DRAW_BUF_STRIDE_ALIGN);
    if (s_neon_grad_buf[zone_id] == NULL) {
        s_neon_grad_buf[zone_id] = BG_ALLOC(bytes);
        if (s_neon_grad_buf[zone_id] == NULL) {
            ESP_LOGW(TAG, "neon tube gradient image alloc failed (%u B); keeping live arcs",
                     (unsigned)bytes);
            return;
        }
    }
    /* Deterministic bake: clear everything so the area outside the band (which
     * the arc's radial AA fringes still sample) is black on every rebuild. */
    memset(s_neon_grad_buf[zone_id], 0, bytes);

    lv_obj_t *cv = lv_canvas_create(scr);
    lv_canvas_set_buffer(cv, s_neon_grad_buf[zone_id], size, size, LV_COLOR_FORMAT_RGB565);
    lv_obj_add_flag(cv, LV_OBJ_FLAG_HIDDEN);
    lv_layer_t layer;
    lv_canvas_init_layer(cv, &layer);
    lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
    arc.center.x = size / 2; arc.center.y = size / 2;
    arc.start_angle = 0; arc.end_angle = 360;
    arc.opa = LV_OPA_COVER;
    for (int b = 0; b < 4; ++b) {
        arc.radius = (uint16_t)k_neon_tube_band_geom[b].radius;
        arc.width = (uint16_t)k_neon_tube_band_geom[b].width;
        arc.color = c(neon_tube_band_color(accent_rgb, b));
        lv_draw_arc(&layer, &arc);
    }
    lv_canvas_finish_layer(cv, &layer);

    const lv_draw_buf_t *db = lv_canvas_get_draw_buf(cv);
    memset(&s_neon_grad_img[zone_id], 0, sizeof(s_neon_grad_img[zone_id]));
    s_neon_grad_img[zone_id].header.magic = LV_IMAGE_HEADER_MAGIC;
    s_neon_grad_img[zone_id].header.cf = LV_COLOR_FORMAT_RGB565;
    s_neon_grad_img[zone_id].header.w = (uint32_t)size;
    s_neon_grad_img[zone_id].header.h = (uint32_t)size;
    s_neon_grad_img[zone_id].header.stride = (uint32_t)db->header.stride;
    s_neon_grad_img[zone_id].data_size = bytes;
    s_neon_grad_img[zone_id].data = s_neon_grad_buf[zone_id];
    lv_obj_delete(cv);
    s_neon_grad_key[zone_id] = accent_rgb;
    ESP_LOGI(TAG, "neon tube gradient image %d: %dx%d, %u B, accent 0x%06X",
             zone_id, size, size, (unsigned)bytes, (unsigned)accent_rgb);
}
#endif /* BOOST_NEON_GLYPH_SPRITES */

/* One pass of the sign mark's antialiased-triangle + solid-row fill (see the
 * caller's comment for why it takes two primitives). Factored out so the
 * halo pass and the core pass share the exact same geometry-to-pixels code -
 * the halo is just this same routine called on inflated bars with a dimmer
 * colour, drawn first so the core paints over it. */
static void neon_draw_sign_pass(lv_layer_t *layer,
                                const boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS],
                                uint32_t color)
{
    lv_draw_triangle_dsc_t tri; lv_draw_triangle_dsc_init(&tri);
    tri.color = c(color); tri.opa = LV_OPA_COVER;
    lv_draw_rect_dsc_t bar; lv_draw_rect_dsc_init(&bar);
    bar.bg_color = c(color); bar.bg_opa = LV_OPA_COVER;
    for (int i = 0; i < BOOST_NEON_SIGN_BARS; ++i) {
        lv_area_t sign_box = {
            LV_MIN(bars[i].xt0, bars[i].xb0), bars[i].y0,
            LV_MAX(bars[i].xt1, bars[i].xb1), bars[i].y1,
        };
        if (!neon_area_overlaps(&sign_box, &layer->_clip_area)) continue;
        NEON_STAT_SIGN();
        tri.p[0].x = bars[i].xt0; tri.p[0].y = bars[i].y0;
        tri.p[1].x = bars[i].xt1; tri.p[1].y = bars[i].y0;
        tri.p[2].x = bars[i].xb1; tri.p[2].y = bars[i].y1;
        lv_draw_triangle(layer, &tri);
        tri.p[0].x = bars[i].xt0; tri.p[0].y = bars[i].y0;
        tri.p[1].x = bars[i].xb1; tri.p[1].y = bars[i].y1;
        tri.p[2].x = bars[i].xb0; tri.p[2].y = bars[i].y1;
        lv_draw_triangle(layer, &tri);

        const int h = bars[i].y1 - bars[i].y0;
        if (h <= 0) continue;
        for (int row = 0; row < h; ++row) {
            /* Both edges interpolated top to bottom so the shear carries row
             * by row, then inset to whichever columns the triangles cover
             * COMPLETELY, leaving the fringe columns to their AA - see the
             * caller's original comment for the full rationale. */
            const float t = (float)row / (float)h;
            const float xlf = (float)bars[i].xt0 +
                ((float)bars[i].xb0 - (float)bars[i].xt0) * t;
            const float xrf = (float)bars[i].xt1 +
                ((float)bars[i].xb1 - (float)bars[i].xt1) * t;
            const int xl = (int)ceilf(xlf);
            const int xr = (int)floorf(xrf) - 1;
            if (xr < xl) continue;
            lv_area_t ra = { xl, bars[i].y0 + row, xr, bars[i].y0 + row };
            lv_draw_rect(layer, &bar, &ra);
        }
    }
}

static void draw_neon_live(lv_event_t *e)
{
    NEON_STAT_CB();
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const int cx = px_icx(), cy = px_icy();
    const float psi = s_neon_psi;
    const uint32_t accent_rgb = neon_zone_rgb(theme, psi);
    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START, (float)(ARC_START + ARC_RANGE));
    const float a_val = psi_to_sweep(psi, (float)ARC_START, (float)(ARC_START + ARC_RANGE));
    /* Marquee has no ring, so its bar is drawn here rather than inside the ring
     * guard below. It had been written as a branch INSIDE that guard, whose
     * condition excludes marquee - so the branch was unreachable and the bar
     * never filled at all. */
    if (s_neon_layout == BOOST_NEON_MARQUEE) {
        /* The bulb rings carry live accent bulbs now (see the marquee branch
         * in draw_neon_live()); the bar and its zero mark are also live. */
        const int x_zero = neon_bar_x(cx, 0.0f);
        const int x_val = neon_bar_x(cx, psi);
        const int x_lo = (x_zero < x_val) ? x_zero : x_val;
        const int x_hi = (x_zero < x_val) ? x_val : x_zero;
        if (x_hi - x_lo > 2) {
            lv_draw_rect_dsc_t fill; lv_draw_rect_dsc_init(&fill);
            fill.radius = 8; fill.bg_opa = LV_OPA_COVER;
            fill.bg_color = c(neon_lit(accent_rgb));
            lv_area_t a = { x_lo, cy + neon_mq(NEON_BAR_Y),
                            x_hi, cy + neon_mq(NEON_BAR_Y) + neon_mq(NEON_BAR_H) };
            lv_draw_rect(layer, &fill, &a);
        }
        /* Zero mark, after the fill so the fill sweeping past cannot bury it.
         * Same job as the white zero segment on the ring layouts. */
        lv_draw_rect_dsc_t tick; lv_draw_rect_dsc_init(&tick);
        tick.bg_opa = LV_OPA_COVER; tick.bg_color = lv_color_white();
        /* Capsule ends, matching the bar it marks. */
        tick.radius = LV_RADIUS_CIRCLE;
        const int tick_cy = cy + neon_mq(NEON_BAR_Y) + neon_mq(NEON_BAR_H) / 2;
        lv_area_t tk = { x_zero - neon_mq(NEON_BAR_TICK_W) / 2,
                         tick_cy - neon_mq(NEON_BAR_TICK_H) / 2,
                         x_zero + neon_mq(NEON_BAR_TICK_W) / 2,
                         tick_cy + neon_mq(NEON_BAR_TICK_H) / 2 };
        lv_draw_rect(layer, &tick, &tk);
    }

    /* Live accent bulbs: ring z's accent dots (18/22/24 by ring) light in ring
     * z's zone colour once the reading has REACHED that zone (zone id >= z).
     * The dead track bulbs are baked, so only these need repainting - and only
     * when the zone flips (see update_neon's pair-box invalidation). Drawing
     * here (over the baked canvas) is what makes them live. */
    if (s_neon_layout == BOOST_NEON_MARQUEE) {
        const int zone = neon_zone_id(psi);
        /* The nearest pixel any accent bulb can reach is the innermost ring's
         * inner edge (176 - 4). A dirty region that stays inside that radius
         * (the usual value-tick cell/bar boxes, whose far corner is r~156)
         * cannot contain an accent pixel, so skip the whole 192-bulb scan
         * instead of running its per-bulb clip test on every callback
         * invocation. Spin and zone-flip boxes reach the rings, so they still
         * scan. Byte-identical: nothing is drawn where no bulb overlaps. */
        if (zone >= 0 && clip_reaches_radius(layer, (float)cx, (float)cy,
                (float)(NEON_BULB_RING_R(0) - NEON_BULB_HALF))) {
            const uint32_t ring_rgb[NEON_BULB_RINGS] = {
                theme->vacuum, theme->boost, theme->overboost,
            };
            lv_draw_rect_dsc_t bulb; lv_draw_rect_dsc_init(&bulb);
            bulb.radius = LV_RADIUS_CIRCLE;
            bulb.bg_opa = LV_OPA_COVER;
            for (int z = 0; z <= zone && z < NEON_BULB_RINGS; ++z) {
                const lv_color_t lit_c = c(neon_bulb_accent(ring_rgb[z]));
                for (int i = 0; i < NEON_BULB_N(z); ++i) {
                    if (!NEON_BULB_IS_ACCENT(i, z)) continue;
                    int bx, by;
                    neon_bulb_pos(cx, cy, z, i, &bx, &by);
                    lv_area_t a = { bx - NEON_BULB_HALF, by - NEON_BULB_HALF,
                                    bx + NEON_BULB_HALF, by + NEON_BULB_HALF };
                    if (!neon_area_overlaps(&a, &layer->_clip_area)) continue;
                    bulb.bg_color = lit_c;
                    lv_draw_rect(layer, &bulb, &a);
                }
            }
        }
    }

    /* Guard on the GLOW's inner edge, not the body's. The glow reaches furthest
     * in, so a dirty region touching only that band would otherwise skip the
     * ring entirely and strand its pixels. Per layout: the inner edge is
     * NEON_R minus the full lit depth (NEON_TUBE_BAND_DEPTH on the tube,
     * NEON_SEG_BAND_DEPTH on the segments). */
    if (s_neon_layout != BOOST_NEON_MARQUEE && clip_reaches_radius(layer, (float)cx, (float)cy,
                            (float)(NEON_R - ((s_neon_layout == BOOST_NEON_TUBE)
                                ? NEON_TUBE_BAND_DEPTH
                                : NEON_SEG_BAND_DEPTH) - 3))) {
        lv_draw_arc_dsc_t arc; lv_draw_arc_dsc_init(&arc);
        arc.center.x = cx; arc.center.y = cy; arc.opa = LV_OPA_COVER;
        int first = 0, last = 0;
        const int lit_n = boost_neon_lit_span(a_zero, a_val, NEON_SEG_START,
                                              NEON_SEG_PITCH * (float)NEON_NSEG, NEON_NSEG,
                                              &first, &last);
        /* The peak's segment index (segments layout), used by the in-run
         * suppression test below. The marker is drawn after the run (see the
         * marker block at the end of this guard), so the suppression is what
         * stops it from painting over the run's own lit segment. */
        const int peak_i = (s_neon_peak_value > 0.2f)
            ? neon_seg_index(psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                          (float)(ARC_START + ARC_RANGE)))
            : -1;
        /* Segments only: the whole-segment marker is suppressed while the run
         * itself fills the peak's segment - the lit segment IS the indicator
         * then, and the old body-width overlay additionally erased the run's
         * white cap. The tube's marker is continuous and ALWAYS drawn (it is a
         * permanent landmark in a fixed dim colour, distinct from the run's
         * zone tones), so suppressing it here would recreate the pop-in/out
         * flicker at the peak as the value hovered around the segment
         * boundary. */
        const bool peak_in_run = (s_neon_layout == BOOST_NEON_SEGMENTS &&
                                  lit_n > 0 && peak_i >= first && peak_i <= last);
        /* TUBE peak tell-tale, drawn BEFORE the run so the lit arc sweeps
         * OVER it - the old after-the-run order painted the marker on top,
         * so the run appeared to stop at the marker's near edge instead of
         * passing it. Continuous arc centred EXACTLY on the peak's sweep
         * angle (no segment snapping), always drawn while a peak exists (no
         * in-run suppression -> no pop-in/out flicker), drawn at the unlit
         * track's own width (NEON_TUBE_TRACK_W, r 192-228) so it stays
         * FULLY CONTAINED within the track - it must never bleed into the
         * dark halo the way the white zero marker does - and clamped to the
         * dial so it can never overshoot psiMax. Its ANGULAR span is
         * NEON_TUBE_PEAK_DEG = NEON_TUBE_ZERO_DEG (2.25), so the two
         * landmarks have the same total width in degrees - the user's
         * "width" is the left/right angular extent, not the radial depth.
         * Skipped only when it would overlap the white zero marker - a peak
         * on the notch is indistinguishable from the notch itself. Ink is
         * the dimmed VACUUM inner-ring tone (neon_peak_color): a constant
         * reference mark that never re-tints at zone thresholds. */
        if (s_neon_layout == BOOST_NEON_TUBE && s_neon_peak_value > 0.2f) {
            const float ap = psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                          (float)(ARC_START + ARC_RANGE));
            if (fabsf(ap - a_zero) > NEON_TUBE_PEAK_DEG + NEON_TUBE_ZERO_DEG) {
                const float lo = fmaxf(ap - NEON_TUBE_PEAK_DEG, (float)ARC_START);
                const float hi = fminf(ap + NEON_TUBE_PEAK_DEG,
                                       (float)(ARC_START + ARC_RANGE));
                if (hi - lo > 0.1f) {
                    arc.start_angle = lo;
                    arc.end_angle = hi;
                    arc.color = c(neon_peak_color(theme));
                    arc.width = NEON_TUBE_TRACK_W;
                    arc.radius = NEON_R;
                    lv_draw_arc(layer, &arc);
                }
            }
        }
        if (s_neon_layout == BOOST_NEON_TUBE && lit_n > 0) {
            float lo = a_zero;
            float hi = a_val;
            if (lo > hi) { const float t = lo; lo = hi; hi = t; }
            /* The run is a continuous arc from the zero notch to the value,
             * drawn as A8 wedge tiles (NEON_TUBE_TILE_STEP each, aligned to
             * a_zero, blitted with recolor) plus a live arc for the partial
             * tip. The tip stays live because the run end is continuous - the
             * tiles only ever cover whole wedges - and it is the part that
             * moves every tick anyway. A zone flip that recolours the whole
             * run then repaints as blits instead of rasterising the full run
             * three arcs deep; that recolor is what used to stutter at the
             * boost/overboost crossings. Tiles and tip both use
             * k_neon_tube_band_geom's arcs, so the two paint identical
             * pixels. */
            int k = 0;
#if BOOST_NEON_GLYPH_SPRITES
            if (s_neon_tube_sprites_ready) {
                const float tstep = NEON_TUBE_TILE_STEP;
                const int dir = (a_val >= a_zero) ? 0 : 1;
                while (k < NEON_TUBE_TILES &&
                       (float)k * tstep <= (hi - lo) - tstep + 0.001f) {
                    if (!neon_area_overlaps(&s_neon_tube_box[dir][k],
                                            &layer->_clip_area)) {
                        ++k;
                        continue;
                    }
                    /* A missing tile (alloc failure mid-bake) falls through
                     * to the live arcs for the remainder of the run. */
                    if (s_neon_tube_tile[dir][k][0].img.data == NULL ||
                        s_neon_tube_tile[dir][k][1].img.data == NULL ||
                        s_neon_tube_tile[dir][k][2].img.data == NULL ||
                        s_neon_tube_tile[dir][k][3].img.data == NULL) break;
                    for (int b = 0; b < 4; ++b) {
                        neon_blit_tube_tile(layer, cx, cy, dir, k, b,
                                            neon_tube_band_color(accent_rgb, b));
                    }
                    ++k;
                }
            }
#endif
            /* Live tip: the partial wedge beyond the last full tile. It starts
             * NEON_TUBE_TILE_OVERLAP before the tile boundary so the last
             * tile's AA fringe lands under this arc's full-coverage interior
             * (and the tile's own baked overhang covers this arc's start
             * fringe). With no tiles blitted the run starts exactly at the
             * notch, as before, and the marker repaint below covers the
             * zero-adjacent pixels either way. */
            const float ov = (k > 0) ? NEON_TUBE_TILE_OVERLAP : 0.0f;
            const float tip_a = (a_val >= a_zero)
                ? a_zero + (float)k * NEON_TUBE_TILE_STEP
                : a_zero - (float)k * NEON_TUBE_TILE_STEP;
            if (k < NEON_TUBE_TILES) {
                lo = fminf(a_val, tip_a - ov);
                hi = fmaxf(a_val, tip_a - ov);
            } else {
                lo = a_zero; hi = a_zero;   /* whole sweep lit; no tip */
            }
            if (hi - lo > 0.01f) {
                arc.start_angle = lo;
                arc.end_angle = hi;
#if BOOST_NEON_GLYPH_SPRITES
                /* One arc carrying the baked radial-gradient image (the
                 * dim/body/cap bands baked into a 2*NEON_R RGB565 square by
                 * neon_bake_grad) replaces the three stacked live band arcs
                 * whenever the bake is present and the zone accent matches.
                 * lv_draw_arc centres an img_src square 1:1 on the arc centre,
                 * so the image spans the full radius range with no transform;
                 * the arc's own angular and radial masks supply the run's
                 * edges exactly as the three live arcs did, so the painted
                 * pixels are identical and the per-frame arc mask work drops
                 * from three passes to one. On a key mismatch (gradient not
                 * baked for this zone yet) it falls through to the three live
                 * arcs below. */
                const int grad_zone = neon_zone_id(psi);
                if (s_neon_grad_buf[grad_zone] != NULL &&
                    s_neon_grad_key[grad_zone] == accent_rgb) {
                    arc.img_src = &s_neon_grad_img[grad_zone];
                    /* Mask clips NEON_GRAD_MARGIN outside the band so the
                     * image's own baked edges supply the ring's AA; a mask
                     * exactly on the band would double-AA the edges. */
                    arc.radius = NEON_R + NEON_GRAD_MARGIN;
                    arc.width = NEON_TUBE_BAND_DEPTH + 2 * NEON_GRAD_MARGIN;
                    lv_draw_arc(layer, &arc);
                    arc.img_src = NULL;
                }                 else
#endif
                {
                    /* Abutting bands, same reasoning as the segments loop below.
                     * Four bands now: dim halo, track inner (bloomed), track
                     * outer (lighter), white cap - see neon_tube_band_color. */
                    for (int b = 0; b < 4; ++b) {
                        arc.radius = (uint16_t)k_neon_tube_band_geom[b].radius;
                        arc.width = (uint16_t)k_neon_tube_band_geom[b].width;
                        arc.color = c(neon_tube_band_color(accent_rgb, b));
                        lv_draw_arc(layer, &arc);
                    }
                }
            }
            /* Zero marker last, so the run that just swept over it cannot bury
             * it - the same ordering the marquee bar's tick already uses. The
             * segments layout gets this for free by skipping the zero segment
             * in its loop; the tube run is one continuous arc, so it has no
             * such seam to leave alone and the marker has to be repainted.
             * Only reached when something is lit; with nothing lit the baked
             * copy underneath is already correct and untouched. */
            arc.start_angle = a_zero - NEON_TUBE_ZERO_DEG + NEON_TUBE_ZERO_CENTER;
            arc.end_angle = a_zero + NEON_TUBE_ZERO_DEG + NEON_TUBE_ZERO_CENTER;
            /* Explicit radius: after the gradient path above, `arc.radius`
             * still holds NEON_R + NEON_GRAD_MARGIN from the mask, which would
             * shift this marker 4 px outboard and leave the ring's inner band
             * (the baked marker's innermost depth) uncovered on its inner
             * edge. Match the baked copy (NEON_R / NEON_TUBE_BAND_DEPTH) so
             * live and baked markers are the same geometry whether or not the
             * gradient bake is active. */
            arc.radius = NEON_R;
            arc.width = NEON_TUBE_BAND_DEPTH;
            arc.color = lv_color_white();
            lv_draw_arc(layer, &arc);
        } else if (lit_n > 0) {
            const int zseg = neon_seg_index(a_zero);
            for (int i = first; i <= last; ++i) {
                /* Skip segments this dirty region does not touch. Without it the
                 * whole run was re-submitted for every region. */
                if (s_neon_seg_box_ready &&
                    !neon_area_overlaps(&s_neon_seg_box[i], &layer->_clip_area)) {
                    NEON_STAT_SKIP();
                    continue;
                }
                if (i == zseg) {
                    /* Nothing to draw. The zero marker is already BAKED white
                     * at this exact radius, width and angle, and the baked
                     * canvas sits under this layer and is repainted with every
                     * dirty region - so redrawing it here produced an identical
                     * pixel for an extra arc primitive. The zero segment is
                     * always inside the lit run, since the run starts at zero,
                     * so that was one wasted arc on every cycle the ring moved.
                     * It is also what makes the marker survive with nothing
                     * lit, where this loop never runs at all. */
                    continue;
                }
#if BOOST_NEON_GLYPH_SPRITES
                if (s_neon_seg_sprites_ready &&
                    s_neon_seg_tile[i][0].img.data != NULL &&
                    s_neon_seg_tile[i][1].img.data != NULL &&
                    s_neon_seg_tile[i][2].img.data != NULL) {
                    /* Lit-run fast path: the three bands are baked A8 coverage
                     * tiles (neon_bake_seg_sprites) made by the SAME lv_draw_arc
                     * geometry, so a changed segment repaints as three
                     * recolored blits on the exact pixels the arcs rasterized -
                     * same bands, same colours, same draw order. A missing tile
                     * (bake alloc failure) falls through to the live arcs. */
                    neon_blit_seg_band(layer, cx, cy, i, 0,
                                       scale_rgb(accent_rgb, NEON_HALO_DIM));
                    neon_blit_seg_band(layer, cx, cy, i, 1,
                                       neon_lit(accent_rgb));
                    neon_blit_seg_band(layer, cx, cy, i, 2, 0xFFFFFFu);
                    continue;
                }
#endif
                const float s = neon_seg_start(i);
                arc.start_angle = s; arc.end_angle = s + NEON_SEG_LIT;
                NEON_STAT_ARC();
                /* An additive gaussian bloom does two things a dim halo does
                 * not: it spreads light outward AND it lifts the lit body
                 * itself toward white. Sampling the mockup, a #8B3DFF segment
                 * reads (255,113,255) once bloomed, not (139,61,255). So the
                 * body is scaled up and the falloff is quantised into enough
                 * steps to read as a gradient rather than as banding. */
                /* Exactly three bands, read from the outside in: white cap,
                 * bloomed body, then the unbloomed palette colour on the inner
                 * edge. All share the outer radius, so drawing widest first and
                 * narrowing stacks them without any blending. A multi-step
                 * falloff was tried and rejected - it reads as banding, not as
                 * depth. */
                /* Stacked so the three bands ABUT rather than overlap. They
                 * used to share arc.radius and differ only in width, so each
                 * band painted the full depth of the one outside it and was
                 * then covered by it - 54+30+6 = 90 px of radial
                 * rasterisation for 54 px of visible band, on every lit
                 * segment, three arcs deep. Giving each band its own radius
                 * paints each pixel once for an identical result. The 1 px
                 * of deliberate overlap (the +1s) keeps the antialiased
                 * edges meeting instead of abutting exactly, where neither
                 * arc fully covers and a faint seam can show. */
                arc.radius = NEON_R - NEON_SEG_W + 1;
                arc.width = NEON_SEG_HALO_W;
                arc.color = c(scale_rgb(accent_rgb, NEON_HALO_DIM)); /* inner  */
                lv_draw_arc(layer, &arc);
                arc.radius = NEON_R - NEON_CAP_W + 1;
                arc.width = NEON_SEG_W - NEON_CAP_W + 1;
                arc.color = c(neon_lit(accent_rgb));       /* middle */
                lv_draw_arc(layer, &arc);
                arc.radius = NEON_R;
                arc.width = NEON_CAP_W;
                arc.color = lv_color_white();              /* outer  */
                lv_draw_arc(layer, &arc);
            }
        }
        /* SEGMENTS peak tell-tale, drawn after the run and suppressed while
         * the run fills its segment (peak_in_run) - the lit segment IS the
         * indicator then, and the overlay would paint over the run's white
         * cap. Marks the segment whose slot contains the peak (same floor()
         * the draw's boost_neon_lit_span() uses, so it cannot disagree with
         * the lit run), at the track's OWN width NEON_SEG_W - the full-depth
         * (NEON_SEG_BAND_DEPTH) extension was reverted at the user's review:
         * the marker should be the width of the track, no more. The
         * full-depth arc is deliberately avoided - it is the geometry that
         * produces the corner AA seam in the stale-pixel audit. Ink is the
         * same dimmed vacuum tone as the tube. Skipped when the peak segment
         * is the zero segment (pi == zseg), so the dim tone never smudges the
         * baked white zero marker. */
        if (s_neon_layout == BOOST_NEON_SEGMENTS &&
            s_neon_peak_value > 0.2f && !peak_in_run) {
            const float ap = psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                          (float)(ARC_START + ARC_RANGE));
            const int pi = (int)floorf((ap - NEON_SEG_START) / NEON_SEG_PITCH);
            const int zseg = neon_seg_index(a_zero);
            if (pi >= 0 && pi < NEON_NSEG && pi != zseg) {
                const float s = neon_seg_start(pi);
                arc.start_angle = s; arc.end_angle = s + NEON_SEG_LIT;
                arc.color = c(neon_peak_color(theme));
                arc.width = NEON_SEG_W;
                arc.radius = NEON_R;
                lv_draw_arc(layer, &arc);
            }
        }
    }
    const bool marquee = s_neon_layout == BOOST_NEON_MARQUEE;
    /* The marquee centre draws the SHARED sprites at NEON_MARQUEE_CENTER_SCALE
     * so the readout/bar/stack fit inside the spread rings; every metric and
     * anchor below goes through neon_mq() on marquee so the geometry, the
     * sprite blits and the invalidation all agree about the smaller size. */
    const float mq = marquee ? NEON_MARQUEE_CENTER_SCALE : 1.0f;
    const int slot_w = neon_mq(NEON_SLOT_W);
    const int dot_w = neon_mq(NEON_DOT_W);
    const int sign_w = neon_mq(NEON_SIGN_W);
    const int sign_gap = neon_mq(NEON_SIGN_GAP);
    const int font_px = neon_mq(NEON_FONT_PX);
    boost_neon_readout_t r;
    boost_neon_layout_readout(psi, slot_w, dot_w, sign_w, sign_gap, font_px, &r);
    const uint32_t ink = neon_lit(accent_rgb);
    lv_draw_label_dsc_t dsc; lv_draw_label_dsc_init(&dsc);
    dsc.font = NEON_BIG; dsc.align = LV_TEXT_ALIGN_CENTER;
    char ch[2] = { 0, 0 };
    /* The glow used to be unavailable here - LVGL cannot blur a glyph at draw
     * time, and stamping the same glyph at small offsets reads as fringing
     * rather than as glow. The sprite path below bakes a real box-blurred
     * glow into each tile at scene build instead; this live label draw stays
     * the flat-ink fallback for when sprites are unavailable. */
    dsc.color = c(ink);
    /* lv_draw_label() draws from the TOP of its area, so this is what places
     * the digits vertically - and therefore what the sign has to be aligned
     * against. */
    const int top = cy + neon_readout_top_off();
    const int line_h = (int)lv_font_get_line_height(dsc.font);
    const int bottom = top + line_h + 8;
    for (uint8_t i = 0; i < r.count; ++i) {
#if BOOST_NEON_GLYPH_SPRITES
        if (s_neon_glyph_sprites_ready) {
            const int gi = neon_glyph_index(r.cells[i].ch);
            if (gi >= 0) {
                /* A8 coverage blit: LVGL takes an untransformed A8 image as a
                 * MASK for idsc.recolor, so zero-coverage pixels contribute
                 * nothing and neighbouring cells' glows composite instead of
                 * overwriting each other. The ink comes from the live accent,
                 * not from a zone index, so the readout follows exactly the
                 * colour the ring is using. The stored image is already
                 * cropped to this glyph's own tight bbox - position it by the
                 * same bbox offset baked alongside it. On the marquee the
                 * sprite is drawn at NEON_MARQUEE_CENTER_SCALE, so the
                 * bake-time spr offsets scale with the cells. */
                neon_blit_sprite_scaled(layer, gi,
                    cx + r.cells[i].x + (int)lroundf(mq * (float)s_neon_spr_dx),
                    top + (int)lroundf(mq * (float)s_neon_spr_dy), ink, mq);
                continue;
            }
        }
#endif
        ch[0] = r.cells[i].ch; dsc.text = ch; dsc.text_local = 1;
        /* The draw area must be wider than the cell pitch, because the glyph
         * is wider than its cell and leans right at the top - a box the size
         * of the cell clips the ink, and then the clip and the invalidation
         * disagree at the top-right corner. NEON_CELL_BLEED covers both. */
        lv_area_t a = { cx + r.cells[i].x - slot_w / 2 - NEON_CELL_BLEED, top,
                        cx + r.cells[i].x + slot_w / 2 + NEON_CELL_BLEED, bottom };
        if (!neon_area_overlaps(&a, &layer->_clip_area)) continue;
        NEON_STAT_LABEL();
        lv_draw_label(layer, &dsc, &a);
    }
    if (r.sign) {
        /* Centre the mark on the digits' line box rather than on the face.
         * The two coincide on the ring layouts by accident of their box top,
         * but marquee's readout sits far higher, and centring on the face put
         * the sign level with the BOTTOM of its digits. */
#if BOOST_NEON_GLYPH_SPRITES
        /* The baked mark carries the same two-pass blur the digits carry, so
         * its halo matches theirs by construction. What it replaced was a
         * second pass of the bar geometry with every edge pushed out
         * NEON_SIGN_GLOW px: at a 6 px bar height on an 8 px pitch that
         * inflation overlapped the two bars and filled the gap between them,
         * so the mark read as a solid slab rather than as a glowing minus. */
        if (s_neon_glyph_sprites_ready &&
            neon_blit_sprite_scaled(layer, NEON_SIGN_SLOT,
                cx + r.sign_x + (int)lroundf(mq * (float)s_neon_spr_dx),
                top + (int)lroundf(mq * (float)s_neon_spr_dy), ink, mq)) {
            /* done */
        } else
#endif
        {
            boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS];
            boost_neon_sign_bars(cx + r.sign_x, top + line_h / 2, font_px, sign_w, bars);
            neon_draw_sign_pass(layer, bars, ink);
        }
    }

    /* Zone word, in the same bloomed ink the readout uses. Drawn here rather
     * than as a child label so its glow composites against the face; the
     * label it replaced had none, which made it the one flat lit element on
     * an otherwise blooming face. */
    if (s_neon_words_ready) {
        const int wi = neon_zone_id(psi);
        const lv_image_dsc_t *wimg = &s_neon_word_img[wi];
        if (wimg->data != NULL) {
            int bx, by;
            neon_word_box_origin(cx, cy, &bx, &by);
            const int wx = bx + s_neon_word_dx[wi];
            const int wy = by + s_neon_word_dy[wi];
            lv_area_t wa = { wx, wy, wx + (int)wimg->header.w - 1,
                             wy + (int)wimg->header.h - 1 };
            if (neon_area_overlaps(&wa, &layer->_clip_area)) {
                lv_draw_image_dsc_t widsc; lv_draw_image_dsc_init(&widsc);
                widsc.src = wimg;
                widsc.opa = LV_OPA_COVER;
                widsc.recolor = c(ink);
                widsc.recolor_opa = LV_OPA_COVER;
                lv_draw_image(layer, &widsc, &wa);
            }
        }
    }
}

static void build_neon(lv_obj_t *scr)
{
    const double build_start_ms = neon_now_ms();
    const boost_theme_t *theme = active_theme();
    s_neon_layout = boost_theme_neon_layout();
    /* Restart the marquee chase from a clean state on every scene build: a
     * theme switch or config change rebuilds the scene, so the phase must
     * not carry across (the bake and the live accents agree only when the
     * phase restarts at 0). */
    memset(s_neon_spin_phase, 0, sizeof(s_neon_spin_phase));
    s_neon_spin_tick = 0;
    s_neon_spin_last_ms = 0;
    /* Force a bar repaint on the first update after a rebuild: the sentinels
     * can never equal a real neon_bar_x() extent. */
    s_neon_bar_lo = INT_MIN;
    s_neon_bar_hi = INT_MAX;
    s_neon_bg_reused = false;
    s_neon_sprites_reused = false;
    neon_build_seg_boxes();
    /* Memoised exactly like s_vault_bg_buf: keep the painted face across theme
     * switches and repaint only when one of its inputs changes. Everything the
     * paint reads is in the key - the layout (which of the three faces), the
     * two colours it draws with, and the psi mapping that places the white
     * zero marker. */
    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    if (s_neon_bg_buf == NULL) s_neon_bg_buf = BG_ALLOC(bg_bytes);
    if (s_neon_bg_buf != NULL) {
        s_neon_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_neon_bg, s_neon_bg_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_neon_bg);
        lv_obj_clear_flag(s_neon_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        const neon_bg_key_t key = {
            .layout = (uint8_t)s_neon_layout,
            .track = theme->track,
            .zero = theme->zero,
            .zero_sweep = psi_to_sweep(0.0f, (float)ARC_START,
                                       (float)(ARC_START + ARC_RANGE)),
        };
        if (!s_neon_bg_key_valid || memcmp(&key, &s_neon_bg_key, sizeof(key)) != 0) {
            paint_neon_background(s_neon_bg, theme);
            s_neon_bg_key = key;
            s_neon_bg_key_valid = true;
        } else {
            s_neon_bg_reused = true;
        }
    } else {
        ESP_LOGW(TAG, "neon background cache alloc failed (%u B)", (unsigned)bg_bytes);
    }
    const double bg_done_ms = neon_now_ms();
#if BOOST_NEON_GLYPH_SPRITES
    /* Bake up front, never on a zone flip, or the flip would stutter.
     * Allocation failure degrades to the live-label path; the face still
     * works either way. */
    neon_bake_glyph_sprites(scr);
    neon_bake_zone_words(scr);
    if (s_neon_layout == BOOST_NEON_SEGMENTS) {
        neon_bake_seg_sprites(scr);
    } else if (0 && s_neon_layout == BOOST_NEON_TUBE) {
        /* Tube run tiles are PARKED. Three designs failed the sim stale-pixel
         * audit: overlap 0.5 deg, overlap 2 deg, and the full-ring radial
         * bake with half-open angular membership plus a tile-boundary
         * invalidation. The tiles tile exactly (no seams) but the tile blits
         * are opaque src-over paints: a pixel's composite depends on which
         * tiles AND the live tip arc happened to paint it across frames, and
         * the delta-only invalidation lets the retained composite drift from
         * the truth. The tube keeps the live arcs (pixel-clean); the zone
         * flip cost is attacked by the radial-gradient arc instead. */
        neon_bake_tube_sprites(scr);
    }
    /* The tube run's radial-gradient arc images, baked with the other bakes
     * (never from inside a draw callback). ALL THREE zone images are baked
     * here so a zone flip only SWITCHES which image the run repaint uses -
     * rebuilding on the flip frame would re-pay the three full-ring
     * rasterisations this feature exists to avoid. Each is keyed on its
     * zone's accent colour, so a theme change (which rebuilds the scene)
     * rebakes all three. */
    if (s_neon_layout == BOOST_NEON_TUBE) {
        for (int z = 0; z < NEON_GRAD_ZONES; ++z) {
            const float zp = (z == 2) ? s_psi_overboost
                                      : (z == 1) ? (s_psi_overboost * 0.5f)
                                                 : -1.0f;
            neon_bake_grad(scr, z, neon_zone_rgb(theme, zp));
        }
    }
#endif
    const double bake_done_ms = neon_now_ms();
    s_neon_face = lv_obj_create(scr);
    lv_obj_remove_style_all(s_neon_face); lv_obj_set_size(s_neon_face, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_neon_face);
    lv_obj_clear_flag(s_neon_face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_neon_face, draw_neon_live, LV_EVENT_DRAW_MAIN, NULL);
    /* The lower block sits at one rhythm on every layout. */
    const int stack_dy = 0;
    /* The zone word is drawn from draw_neon_live() as a baked sprite so it can
     * carry a glow, and only falls back to a label if that bake failed. The
     * label's geometry and styling are kept identical either way, because the
     * bake records its offsets against exactly this box. */
    if (!s_neon_words_ready) {
        s_neon_zone = lv_label_create(scr);
        lv_label_set_text(s_neon_zone, "VACUUM");
        lv_obj_set_style_text_font(s_neon_zone, NEON_LABEL, 0);
        lv_obj_set_style_text_letter_space(s_neon_zone, 2, 0);
        lv_obj_set_style_text_color(s_neon_zone, c(theme->vacuum), 0);
        lv_obj_set_style_text_align(s_neon_zone, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(s_neon_zone, NEON_WORD_BOX_W, NEON_WORD_BOX_H);
        lv_obj_align(s_neon_zone, LV_ALIGN_CENTER, 0, NEON_WORD_Y);
    }
    s_neon_unit = lv_label_create(scr);
    lv_label_set_text(s_neon_unit, "P S I");
    lv_obj_set_style_text_font(s_neon_unit, NEON_LABEL, 0);
    lv_obj_set_style_text_letter_space(s_neon_unit, 3, 0);
    lv_obj_set_style_text_color(s_neon_unit, c(theme->muted), 0);
    lv_obj_set_style_text_align(s_neon_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_neon_unit, 200, 30);
    lv_obj_align(s_neon_unit, LV_ALIGN_CENTER, 0, neon_mq(NEON_UNIT_Y) + stack_dy);
    s_neon_peak = lv_label_create(scr);
    lv_label_set_text(s_neon_peak, "PEAK 0.0");
    lv_obj_set_style_text_font(s_neon_peak, F_MONO16, 0);
    lv_obj_set_style_text_color(s_neon_peak, c(theme->muted), 0);
    lv_obj_set_style_text_align(s_neon_peak, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_neon_peak, 260, 24);
    lv_obj_align(s_neon_peak, LV_ALIGN_CENTER, 0, neon_mq(NEON_PEAK_Y) + stack_dy);
    s_neon_psi = isfinite(s_display_psi) ? s_display_psi : 0.0f;
    s_neon_peak_value = fmaxf(s_peak_psi, 0.0f);
    s_neon_color_psi = s_neon_psi;
    s_neon_cell_n = 0;   /* force a full readout paint on the first update */
    s_neon_peak_idx = (s_neon_peak_value > 0.2f)
        ? neon_seg_index(psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                      (float)(ARC_START + ARC_RANGE)))
        : -1;
    /* Tube marker state: the scene build repaints the whole face, so the
     * marker is already on screen; these just need to agree with what was
     * drawn so the first update does not invalidate a phantom old span. */
    s_neon_tube_peak_vis = s_neon_peak_value > 0.2f;
    s_neon_tube_peak_angle = s_neon_tube_peak_vis
        ? psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                       (float)(ARC_START + ARC_RANGE))
        : NAN;
    /* Always logged, not gated behind BOOST_NEON_DRAW_STATS: this is one line
     * per scene build, and the split between background and bake is what says
     * whether a slow theme switch is worth chasing. It is also the only
     * timing the DEVICE emits - the stats build is a host-sim thing, so
     * gating it was why the panel's own load cost had never been measured. */
    ESP_LOGI(TAG, "neon scene build: layout=%d bg %.0f ms%s + bake %.0f ms%s = %.0f ms",
             (int)s_neon_layout,
             bg_done_ms - build_start_ms, s_neon_bg_reused ? " (cached)" : "",
             bake_done_ms - bg_done_ms, s_neon_sprites_reused ? " (cached)" : "",
             neon_now_ms() - build_start_ms);
}

/* Invalidate one angular run of the ring. Split at the 90 degree boundaries
 * exactly as invalidate_value_arc() does: lv_draw_arc_get_area returns an
 * axis-aligned bounding box, so a span crossing an axis bloats into most of the
 * ring. Sized by the halo, the widest stroke. */
static void neon_inv_span(float a0, float a1)
{
    /* Marquee has no ring. Invalidating one anyway cost it both pixels and
     * invalidation SLOTS: LVGL keeps LV_INV_BUF_SIZE (32) areas per refresh and
     * falls back to redrawing the whole 466x466 screen once that overflows, so
     * the wasted slots turned a zone flip into a full-screen repaint. */
    if (s_neon_layout == BOOST_NEON_MARQUEE) return;
    if (a1 < a0) { const float t = a0; a0 = a1; a1 = t; }
    if ((a1 - a0) < 0.01f) return;
    /* Segments light whole: boost_neon_lit_span() indexes by floor(), so a
     * value landing mid-segment lights that segment all the way to its far
     * edge. Snapping the run outward to segment boundaries is what makes the
     * invalidated area cover what is actually painted â€” without it the far
     * half of the boundary segment is left stale on every frame. The tube
     * paints one continuous arc at the exact span, so it keeps its precise
     * angular extent; only the common sweep clamp below applies to both. */
    if (s_neon_layout == BOOST_NEON_SEGMENTS) {
        a0 = NEON_SEG_START + floorf((a0 - NEON_SEG_START) / NEON_SEG_PITCH) * NEON_SEG_PITCH;
        a1 = NEON_SEG_START + ceilf((a1 - NEON_SEG_START) / NEON_SEG_PITCH) * NEON_SEG_PITCH;
    }
    if (a0 < (float)ARC_START) a0 = (float)ARC_START;
    if (a1 > (float)(ARC_START + ARC_RANGE)) a1 = (float)(ARC_START + ARC_RANGE);
    /* The arc is invalidated in angular chunks so each box hugs the ring band
     * instead of the arc's full bounding box, which would otherwise include the
     * empty dial interior on a long run (the boost/overboost zone flip recolors
     * the whole run and is the widest dirty region on the face). Both layouts
     * split at 30 degrees - measured on the host audit, 30 is the knee (30 and
     * 15 both floor at the same flushed-pixel total; 90 lands ~12% higher on
     * the flip's worst cycle). Byte-identical pixels; the box count stays
     * far under LVGL's 32-area invalidation buffer on the longest run. */
    for (float seg = a0; seg < a1;) {
        const float step = 30.0f;
        const float boundary = (floorf(seg / step) + 1.0f) * step;
        const float seg_end = fminf(a1, boundary);
        lv_area_t area;
        /* Span the live painted band: NEON_GLOW_OUT outside the ring through
         * to the inner band's inner edge, so widening the glow cannot outrun
         * the bounds. The radial width is per-layout - the tube's four-band
         * depth is NEON_TUBE_BAND_DEPTH (55), the segments' NEON_SEG_BAND_DEPTH
         * (49). */
        const uint16_t band_w = (uint16_t)(NEON_GLOW_OUT +
            ((s_neon_layout == BOOST_NEON_TUBE)
                ? NEON_TUBE_BAND_DEPTH
                : NEON_SEG_BAND_DEPTH));
        lv_draw_arc_get_area(px_icx(), px_icy(), (uint16_t)(NEON_R + NEON_GLOW_OUT),
                             seg, seg_end,
                             band_w,
                             false, &area);
        /* Pad the exact sector bbox by the painted extent it does not cover:
         * the tube's gradient mask runs 4 px inside the geometric inner edge
         * (NEON_GRAD_MARGIN, the image is NEON_R+4 with width BAND_DEPTH+8),
         * plus ~2 px of arc-mask AA fringe, plus 2 px margin. lv_draw_arc_get_area
         * already includes the end-corner rays, so 14 px (as shipped) was pure
         * slack beyond that. */
        area.x1 -= NEON_INV_PAD; area.y1 -= NEON_INV_PAD;
        area.x2 += NEON_INV_PAD; area.y2 += NEON_INV_PAD;
        lv_obj_invalidate_area(s_neon_face, &area);
        seg = seg_end;
    }
}

/* Segment index carrying a given angle, or -1 when it falls outside the sweep. */
static int neon_seg_index(float angle)
{
    const int i = (int)floorf((angle - NEON_SEG_START) / NEON_SEG_PITCH);
    return (i >= 0 && i < NEON_NSEG) ? i : -1;
}

/* Invalidate one contiguous run of segments, first through last inclusive.
 * One angular span (split at the 90 degree boundaries inside neon_inv_span),
 * not one call per segment, so a run crossing an axis stays a single box and
 * a run's neighbouring gaps stay covered by the same span the segments paint
 * into. */
static void neon_inv_seg_range(int first, int last)
{
    if (first < 0 || last < first || last >= NEON_NSEG) return;
    neon_inv_span(NEON_SEG_START + (float)first * NEON_SEG_PITCH,
                  NEON_SEG_START + (float)(last + 1) * NEON_SEG_PITCH);
}

static void neon_inv_seg(int index)
{
    if (index < 0) return;
    neon_inv_seg_range(index, index);
}

/* Horizontal invalidation span for one readout cell: the union of the
 * existing label-box span (slot_w + NEON_CELL_BLEED either side, the same
 * box lv_draw_label() clips to) and, when sprites are baked, the sprite's own
 * footprint at this cell. Needed because the sprite tile is one uniform
 * width shared by every glyph including '.', which is narrower than a digit
 * slot in the ORIGINAL layout - so the label-box span alone is not
 * guaranteed to contain it. Union rather than replace, so the fallback path
 * (sprites unavailable) keeps exactly its old, already-correct bound. */
/* Horizontal extent the minus mark actually paints, around a given sign_x.
 *
 * Bounding it by "sign width plus a fixed glow pad" was fine while the glow
 * was a 5 px inflation, but the baked mark's blur reaches NEON_SPR_GLOW_MARGIN,
 * and simply raising the pad to match pushed the readout's dirty rectangle out
 * far enough to cut through the ring's inner halo band - which cost flushed
 * pixels on every readout update and left an antialiasing seam where the arc
 * got clipped at the new boundary. The baked sprite knows its own footprint
 * exactly, and that footprint is NARROWER than either pad, so asking it is
 * both tighter and correct. The pad path stays for the live-label fallback,
 * where there is no sprite to ask. */
static void neon_sign_x_span(int cx, int sign_x, int pad, int *lo, int *hi)
{
    *lo = cx + sign_x - pad;
    *hi = cx + sign_x + pad;
#if BOOST_NEON_GLYPH_SPRITES
    if (s_neon_glyph_sprites_ready &&
        s_neon_glyph_img[NEON_SIGN_SLOT].data != NULL) {
        /* Marquee scaled set active: the sign is blitted as the pre-scaled
         * tile at anchor + bbox_s (see neon_blit_sprite_scaled), whose
         * footprint is narrower than the pad box - ask the tile, with a 1 px
         * AA margin, exactly like the full-size path below. */
        if (s_neon_layout == BOOST_NEON_MARQUEE && s_neon_glyph_buf_s != NULL &&
            s_neon_glyph_img_s[NEON_SIGN_SLOT].data != NULL) {
            const int ax = cx + sign_x +
                (int)lroundf(NEON_MARQUEE_CENTER_SCALE * (float)s_neon_spr_dx);
            const int16_t *bs = s_neon_glyph_bbox_s[NEON_SIGN_SLOT];
            *lo = ax + bs[0] - 1;
            *hi = ax + bs[0] + (int)s_neon_glyph_img_s[NEON_SIGN_SLOT].header.w;
        } else {
            const int16_t *bb = s_neon_glyph_bbox[NEON_SIGN_SLOT];
            *lo = cx + sign_x + s_neon_spr_dx + bb[0];
            *hi = *lo + (int)s_neon_glyph_img[NEON_SIGN_SLOT].header.w - 1;
        }
    }
#endif
}

static void neon_cell_x_span(int cx, int cell_x, int slot_w, int cell_pad,
                             int glyph, int *lo, int *hi)
{
    *lo = cx + cell_x - slot_w / 2 - cell_pad;
    *hi = cx + cell_x + slot_w / 2 + cell_pad;
#if BOOST_NEON_GLYPH_SPRITES
    if (s_neon_glyph_sprites_ready) {
        /* Marquee scaled set active: the cell is blitted as the pre-scaled
         * tile at anchor + bbox_s (see neon_blit_sprite_scaled), so the painted
         * pixels are exactly that footprint - narrower than the label box's
         * slot/bleed span, and REPLACING it with the tile's own extent (plus
         * 1 px AA margin) shrinks the flushed box without dropping coverage.
         * The fallback paths (per-frame transform or no sprite for this
         * glyph) keep the label-box span unioned with the full-size tile. */
        if (s_neon_layout == BOOST_NEON_MARQUEE && s_neon_glyph_buf_s != NULL &&
            glyph >= 0 && s_neon_glyph_img_s[glyph].data != NULL) {
            const int ax = cx + cell_x +
                (int)lroundf(NEON_MARQUEE_CENTER_SCALE * (float)s_neon_spr_dx);
            const int16_t *bs = s_neon_glyph_bbox_s[glyph];
            *lo = ax + bs[0] - 1;
            *hi = ax + bs[0] + (int)s_neon_glyph_img_s[glyph].header.w;
        } else if (s_neon_layout != BOOST_NEON_MARQUEE &&
                   glyph >= 0 && s_neon_glyph_img[glyph].data != NULL) {
            /* Full-size set (tube/segments, mq=1.0): the blit is the tile at
             * anchor + bbox (see neon_blit_sprite_scaled), so the per-glyph
             * bbox footprint is exact - narrower than the label box, and
             * REPLACING it (plus 1 px AA margin) shrinks the flushed box.
             * Marquee never reaches here: its transform path shifts the rect
             * by (scale-1)*bw/2, which the conservative label box covers. */
            const int16_t *bb = s_neon_glyph_bbox[glyph];
            const int sx0 = cx + cell_x + s_neon_spr_dx + bb[0] - 1;
            *lo = sx0;
            *hi = sx0 + (int)s_neon_glyph_img[glyph].header.w + 1;
        } else {
            const int sx0 = cx + cell_x + s_neon_spr_dx;
            const int sx1 = sx0 + s_neon_spr_w - 1;
            if (sx0 < *lo) *lo = sx0;
            if (sx1 > *hi) *hi = sx1;
        }
    }
#endif
}

/* Tight vertical extent of one baked readout sprite's ACTUAL painted pixels at
 * the readout anchor `top`, for the full-size sprite set (tube/segments; the
 * marquee's pre-scaled set has its own geometry and stays on the conservative
 * box). Returns false when no immutable full-size tile exists for this glyph -
 * live-label fallback or marquee - leaving the y outputs untouched so the caller
 * keeps its conservative box. The +/-1 AA margin is CLAMPED into the caller's
 * conservative box (lo_box..hi_box), which already covers the painted pixels
 * by construction, so tightening can never expand a flush box. */
#if BOOST_NEON_GLYPH_SPRITES
static bool neon_glyph_y_span(int top, int glyph, int lo_box, int hi_box,
                              int *y0, int *y1)
{
    if (s_neon_layout == BOOST_NEON_MARQUEE || !s_neon_glyph_sprites_ready)
        return false;
    if (glyph < 0) return false;
    const lv_image_dsc_t *img = &s_neon_glyph_img[glyph];
    if (img->data == NULL) return false;
    const int16_t *bb = s_neon_glyph_bbox[glyph];
    const int sy0 = top + s_neon_spr_dy + bb[1] - 1;
    int t0 = sy0, t1 = sy0 + (int)img->header.h + 1;
    if (t0 < lo_box) t0 = lo_box;
    if (t1 > hi_box) t1 = hi_box;
    *y0 = t0;
    *y1 = t1;
    return true;
}

/* Fold one element's tight vertical extent into a running union. An element
 * without an immutable sprite can be painted anywhere inside the conservative
 * label-box/tile union, so its presence marks the whole union as needing that
 * box. */
static void neon_y_fold(int top, int glyph, int lo_box, int hi_box,
                        int *y0, int *y1, bool *all_tight)
{
    int t0, t1;
    if (neon_glyph_y_span(top, glyph, lo_box, hi_box, &t0, &t1)) {
        if (t0 < *y0) *y0 = t0;
        if (t1 > *y1) *y1 = t1;
    } else {
        *all_tight = false;
    }
}
#endif

static void update_neon(const boost_sample_t *sample, const boost_theme_t *theme)
{
    /* A colour flip from the previous sample deferred its full-run recolor to
     * NOW (word-first, arc-next-frame): this frame repaints the run in the new
     * zone colour. Runs before the live logic so the pending span and this
     * sample's own delta union correctly. */
    if (g_neon_flip_pending) {
        neon_inv_span(s_neon_flip_lo, s_neon_flip_hi);
        g_neon_flip_pending = false;
    }
    const float psi = isfinite(sample->psi) ? sample->psi : 0.0f;
    const float old_psi = s_neon_psi;
    s_neon_psi = psi;
    s_neon_peak_value = fmaxf(s_peak_psi, 0.0f);
    const char *zone = (psi >= s_psi_overboost) ? "OVERBOOST" : (psi > 0.05f) ? "BOOST" : "VACUUM";
    const lv_color_t zc = c(neon_lit(neon_zone_rgb(theme, psi)));
    if (s_neon_zone != NULL) {
        if (strcmp(lv_label_get_text(s_neon_zone), zone) != 0) lv_label_set_text(s_neon_zone, zone);
        if (!lv_color_eq(lv_obj_get_style_text_color(s_neon_zone, 0), zc)) lv_obj_set_style_text_color(s_neon_zone, zc, 0);
    } else {
        /* Sprite path: no child object is tracking this, so the word's own
         * dirty region is this code's responsibility. Both the word AND its
         * colour change only at a zone threshold, so one invalidation on the
         * flip covers both - and it has to span the union of the outgoing and
         * incoming words, since OVERBOOST is far wider than BOOST and a box
         * sized to the new word alone would strand the old one's tail. The
         * box is the full layout box, which contains every word by
         * construction, plus the glow margin. */
        (void)zone;
        const int wi = neon_zone_id(psi);
        if (wi != s_neon_word_drawn) {
            int bx, by;
            neon_word_box_origin(px_icx(), px_icy(), &bx, &by);
            /* Invalidate only the painted word pixels. Each word's sprite is
             * cropped to its ink + glow at bake time (s_neon_word_dx/dy + the
             * image's own w/h), so the union of the outgoing and incoming
             * sprites is the exact dirty area. The layout box (300x34 + glow,
             * ~328x62) was 2-3x wider than the widest word and reached past
             * the ring's inner edge (r~187 vs the band's r=173), pulling the
             * ring into the word's repaint. BOOST (117x45) <-> OVERBOOST
             * (190x45) unions to ~190x45 with corners at r~158 - inside the
             * ring. */
            lv_area_t wa = { INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
            const int idx[2] = { s_neon_word_drawn, wi };
            for (int i = 0; i < 2; ++i) {
                const int w = idx[i];
                if (w < 0 || w >= NEON_WORD_COUNT) continue;
                const lv_image_dsc_t *wimg = &s_neon_word_img[w];
                if (wimg->data == NULL) continue;
                const int wx = bx + s_neon_word_dx[w];
                const int wy = by + s_neon_word_dy[w];
                const int wx2 = wx + (int)wimg->header.w - 1;
                const int wy2 = wy + (int)wimg->header.h - 1;
                if (wx < wa.x1) wa.x1 = wx;
                if (wy < wa.y1) wa.y1 = wy;
                if (wx2 > wa.x2) wa.x2 = wx2;
                if (wy2 > wa.y2) wa.y2 = wy2;
            }
            if (wa.x1 <= wa.x2 && wa.y1 <= wa.y2) {
                lv_obj_invalidate_area(s_neon_face, &wa);
            }
            s_neon_word_drawn = wi;
        }
    }
    char buf[32]; snprintf(buf, sizeof(buf), "PEAK %.1f", (double)s_neon_peak_value);
    if (strcmp(lv_label_get_text(s_neon_peak), buf) != 0) lv_label_set_text(s_neon_peak, buf);
    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START, (float)(ARC_START + ARC_RANGE));
    const float a_old = psi_to_sweep(old_psi, (float)ARC_START, (float)(ARC_START + ARC_RANGE));
    const float a_new = psi_to_sweep(psi, (float)ARC_START, (float)(ARC_START + ARC_RANGE));
    /* Three cases, mirroring set_value_arc(). A delta-only wedge is wrong for
     * the first two and leaves the rest of the run stale. */
    const bool color_flip = neon_zone_rgb(theme, s_neon_color_psi) != neon_zone_rgb(theme, psi);
    const bool side_flip = (old_psi < 0.0f) != (psi < 0.0f);
    if (side_flip) {
        /* Runs on opposite sides of the notch are disjoint. Invalidate each
         * side only if that side's run is actually painted - the draw gates on
         * boost_neon_lit_span()'s half-tile threshold, so a side below that
         * threshold shows nothing and needs no repaint (and repainting it can
         * only widen the dirty region). Same helper and arguments the draw
         * uses, so threshold behaviour cannot disagree with what was painted. */
        int f, l;
        if (boost_neon_lit_span(a_zero, a_old, NEON_SEG_START,
                                NEON_SEG_PITCH * (float)NEON_NSEG, NEON_NSEG,
                                &f, &l) > 0)
            neon_inv_span(a_zero, a_old);
        if (boost_neon_lit_span(a_zero, a_new, NEON_SEG_START,
                                NEON_SEG_PITCH * (float)NEON_NSEG, NEON_NSEG,
                                &f, &l) > 0)
            neon_inv_span(a_zero, a_new);
    } else if (color_flip) {
        /* Every lit segment restyles, so the whole run must repaint - but NOT
         * this frame. Deferred to the next sample (word-first, arc-next-frame)
         * so the flip frame carries only the cheap word/readout/peak repaints,
         * and this frame's dirty region is smaller. The pending span covers
         * the union of both full runs; the draw repaints it next frame in the
         * new zone colour. One frame of old-colour ring after the word flips
         * is the accepted visual lag. */
        g_neon_flip_pending = true;
        s_neon_flip_lo = fminf(a_zero, fminf(a_old, a_new));
        s_neon_flip_hi = fmaxf(a_zero, fmaxf(a_old, a_new));
    } else if (s_neon_layout == BOOST_NEON_SEGMENTS) {
        /* Segments light whole and in the same colour, so only the segments
         * whose painted state flipped need a repaint - the symmetric
         * difference of the old and new lit runs, minus the baked zero
         * marker. Repainting the angular delta instead also reflushed the
         * segment that merely contained the old endpoint on every move. The
         * helper derives both sets from boost_neon_lit_span() with the same
         * arguments the draw passes, so threshold and boundary behaviour
         * cannot disagree with what draw_neon_live() actually painted. */
        boost_neon_seg_diff_t d;
        const int dn = boost_neon_seg_diff(a_zero, a_old, a_new,
                                           NEON_SEG_START,
                                           NEON_SEG_PITCH * (float)NEON_NSEG,
                                           NEON_NSEG, &d);
        for (int i = 0; i < dn; ++i)
            neon_inv_seg_range(d.first[i], d.last[i]);
    } else if (s_neon_layout == BOOST_NEON_TUBE) {
        /* The tube is all-or-nothing: draw_neon_live() paints the whole
         * run from a_zero whenever boost_neon_lit_span() finds at least
         * half a segment, and nothing at all below that. When old and
         * new straddle that lit threshold, the painted area changes by
         * the whole run, not by the delta between the endpoints - so a
         * delta-only wedge strands the zero-to-near-endpoint band (the
         * ring-stale audit caught this at r~228.8). Detect the flip
         * with the same helper the draw uses and repaint the full span
         * (a_zero through the farther endpoint) so the whole run flips
         * at once. Steady motion with both ends lit (or both unlit)
         * keeps the precise delta wedge. */
        float lo, hi;
        if (boost_neon_tube_dirty_span(a_zero, a_old, a_new,
                                       NEON_SEG_START,
                                       NEON_SEG_PITCH * (float)NEON_NSEG,
                                       NEON_NSEG, &lo, &hi)) {
            neon_inv_span(lo, hi);
        } else {
            neon_inv_span(fminf(a_old, a_new), fmaxf(a_old, a_new));
        }
    } else {
        /* Only the moving end moved. */
        neon_inv_span(a_old, a_new);
    }
    s_neon_color_psi = psi;

    /* The peak tell-tale. Its ink is FIXED (neon_peak_color - the dimmed
     * vacuum inner-ring tone), so it never restyles at a zone threshold; the
     * only changes are its position (a new peak or a tap reset) and, on the
     * segments layout, the in-run suppression state.
     *
     * Segments: index-tracked, suppressed while the run fills its segment
     * (peak_in_run, computed with the same helper and arguments the draw
     * uses, so the invalidation and the draw cannot disagree about whether
     * the overlay is showing - the old blanket `peak_idx >= 0` fired every
     * frame once any peak existed, queueing two extra invalidations per
     * cycle).
     *
     * Tube: a continuous arc at the exact peak angle, ALWAYS drawn, so its
     * only change is the angle itself - when the marker moves (or appears /
     * disappears across the 0.2 psi gate), invalidate exactly the old and
     * new spans. */
    if (s_neon_layout == BOOST_NEON_TUBE) {
        const bool vis = s_neon_peak_value > 0.2f;
        const float ap = vis
            ? psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                           (float)(ARC_START + ARC_RANGE))
            : NAN;
        const bool moved = vis && fabsf(ap - s_neon_tube_peak_angle) > 0.01f;
        if (vis != s_neon_tube_peak_vis || moved) {
            if (s_neon_tube_peak_vis) {
                neon_inv_span(fmaxf(s_neon_tube_peak_angle - NEON_TUBE_PEAK_DEG,
                                    (float)ARC_START),
                              fminf(s_neon_tube_peak_angle + NEON_TUBE_PEAK_DEG,
                                    (float)(ARC_START + ARC_RANGE)));
            }
            if (vis) {
                /* Same dial clamps the draw uses, so a peak at psiMax cannot
                 * invalidate a span past the dial end. */
                neon_inv_span(fmaxf(ap - NEON_TUBE_PEAK_DEG, (float)ARC_START),
                              fminf(ap + NEON_TUBE_PEAK_DEG,
                                    (float)(ARC_START + ARC_RANGE)));
            }
            s_neon_tube_peak_vis = vis;
            s_neon_tube_peak_angle = ap;
        }
    } else {
        const int peak_idx = (s_neon_peak_value > 0.2f)
            ? neon_seg_index(psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                          (float)(ARC_START + ARC_RANGE)))
            : -1;
        int pf = 0, pl = 0;
        const int peak_lit_n = boost_neon_lit_span(a_zero, a_new, NEON_SEG_START,
                                                   NEON_SEG_PITCH * (float)NEON_NSEG, NEON_NSEG,
                                                   &pf, &pl);
        const bool peak_in_run = (peak_lit_n > 0 && peak_idx >= pf && peak_idx <= pl);
        if (peak_idx != s_neon_peak_idx || peak_in_run != s_neon_peak_in_run) {
            neon_inv_seg(s_neon_peak_idx);
            neon_inv_seg(peak_idx);
            s_neon_peak_idx = peak_idx;
        }
        s_neon_peak_in_run = peak_in_run;
    }
    /* Repaint only the digit cells whose glyph actually changed. Cell x
     * positions shift when the digit count changes, so any change of count
     * falls back to the union of the old and new compositions. */
    boost_neon_readout_t r;
    /* MUST use the same metrics draw_neon_live uses, per layout. Computing the
     * invalidation from the segments constants while marquee draws at 156 px
     * left the sign and the outer digits uncovered. */
    /* Same scaled metrics draw_neon_live() uses on the marquee, so the dirty
     * boxes and the painted pixels agree. */
    const int inv_slot = neon_mq(NEON_SLOT_W);
    /* The true sign width, fed to boost_neon_layout_readout() below so the
     * computed sign_x/half_w geometry matches what draw_neon_live() actually
     * draws. The invalidation margin used further down is this PLUS the
     * sign's glow reach - kept as a separate variable so widening the dirty
     * region can never leak into the geometry math. */
    const int inv_sign = neon_mq(NEON_SIGN_W);
    /* The sign is a baked sprite now, so its halo reaches exactly as far as
     * every other sprite's: NEON_SPR_GLOW_MARGIN past its own ink. Every
     * invalidation box below that bounds the sign must grow by the same
     * amount, or the halo paints outside its own dirty region and strands a
     * stale trail - which is what happened the first time a sign glow was
     * attempted, before any widening existed. This was NEON_SIGN_GLOW (5)
     * while the halo was an inflated solid pass; the baked blur reaches
     * further, so the pad has to follow it up rather than stay behind. */
    const int inv_sign_pad = inv_sign + NEON_SPR_GLOW_MARGIN;
    boost_neon_layout_readout(psi, inv_slot,
                              neon_mq(NEON_DOT_W),
                              inv_sign,
                              neon_mq(NEON_SIGN_GAP),
                              neon_mq(NEON_FONT_PX), &r);
    /* The glyph is WIDER than the cell it sits in: a '0' at 104 px measures
     * about 83 px against a 62 px cell, so it overhangs ~10 px each side. The
     * cell pitch is spacing, not ink extent, and invalidating only the cell
     * strands the overhang. */
    /* The glyph is wider than its cell - a '0' at 104 px measures about 83 px
     * against a 62 px pitch - so the cell pitch is spacing, not ink extent. */
    /* Marquee's glyph is 156 px and leans ~23 px at the top, so it overhangs
     * its cell far further than the 108 px readout does. */
    /* The glyph is clipped to its draw box, which is the cell pitch plus
     * NEON_CELL_BLEED either side, so covering that box plus a small margin is
     * sufficient on EVERY layout - no per-layout pad. Marquee had been using 70,
     * 40 px more than its own draw box, and its cells are the largest on the
     * face, so the waste showed up directly in the flush cost. */
    const int cell_pad = NEON_CELL_BLEED;
    int cell_top = px_icy() + neon_readout_top_off();
    int cell_bot = cell_top +
        (int)lv_font_get_line_height(NEON_BIG) + 8;
#if BOOST_NEON_GLYPH_SPRITES
    /* The sprite's own vertical footprint is not guaranteed to sit inside
     * the label-box height above - a font whose measured line height is
     * shorter than the glyph-ink-plus-glow tile pushes the bake-time crop's
     * top past this box's top edge. Union it in explicitly rather than
     * trust the two to agree. */
    if (s_neon_glyph_sprites_ready) {
        const int sy0 = cell_top + s_neon_spr_dy;
        const int sy1 = sy0 + s_neon_spr_h - 1;
        if (sy0 < cell_top) cell_top = sy0;
        if (sy1 > cell_bot) cell_bot = sy1;
    }
#endif
    if (s_neon_layout == BOOST_NEON_MARQUEE) {
        /* Only the span the bar's filled end swept, plus a pad. The bar grows
         * from the zero mark, so the union runs zero..old..new. Mapped through
         * neon_bar_x() so the invalidation and the fill share one rule. */
        const int x0 = neon_bar_x(px_icx(), fminf(fminf(old_psi, psi), 0.0f));
        const int x1 = neon_bar_x(px_icx(), fmaxf(fmaxf(old_psi, psi), 0.0f));
        /* Gate on the DRAWN extent, not on the sample: the fill is always the
         * current extent zero..new, so the box has to cover the vacated tail
         * whenever the value moved toward zero. When both ends are unchanged
         * and the zone colour did not flip, the painted bar is pixel-identical
         * and repainting would only flush the same box every 16 ms - this is
         * what lets the marquee go idle at a static reading like every other
         * face. */
        const int lo = neon_bar_x(px_icx(), fminf(psi, 0.0f));
        const int hi = neon_bar_x(px_icx(), fmaxf(psi, 0.0f));
        if (lo != s_neon_bar_lo || hi != s_neon_bar_hi || color_flip) {
            /* Tall enough for the zero mark, which overhangs the bar top and
             * bottom and is redrawn with it. The +2 is AA margin beyond the
             * tick's integer capsule box; verified 0-stale by the host audit
             * (was +6). */
            const int over = (neon_mq(NEON_BAR_TICK_H) - neon_mq(NEON_BAR_H)) / 2 + 2;
            lv_area_t bar = { x0 - 6, px_icy() + neon_mq(NEON_BAR_Y) - over,
                              x1 + 6, px_icy() + neon_mq(NEON_BAR_Y)
                                          + neon_mq(NEON_BAR_H) + over };
            lv_obj_invalidate_area(s_neon_face, &bar);
        }
        s_neon_bar_lo = lo;
        s_neon_bar_hi = hi;
        /* Live accent bulbs: on a zone flip, the rings whose lit-state
         * changed must repaint - each ring's accent bulbs in adjacent
         * PAIR-boxes (pairs of bulbs i, i+1 at the accent residues). With the
         * cumulative ladder a single-step flip touches ONE ring (9/11/12
         * boxes by ring); a two-zone jump touches two - both inside LVGL's
         * 32-slot invalidation buffer. The dead track bulbs are baked and
         * never move. */
        const int z_old = neon_zone_id(old_psi);
        const int z_new = neon_zone_id(psi);
        /* Marquee chase (neonMarqueeSpin): one ring advances per spin tick,
         * round-robin, so a step costs the SAME number of pair-boxes as a zone
         * flip (the advancing ring's 9/11/12) and stays inside LVGL's 32-slot
         * buffer. Advanced phases are skipped on a zone-flip frame so the two
         * never stack; the flip's own invalidation reads the CURRENT phase via
         * neon_accent_base(), so a ring that lights mid-chase draws at its
         * phase. Unlit rings advance silently (nothing is drawn, nothing to
         * repaint) so that when the reading reaches them they are already
         * mid-chase. */
        if (boost_theme_neon_marquee_spin() && z_old == z_new) {
            const uint32_t now = lv_tick_get();
            if (now - s_neon_spin_last_ms >= NEON_MARQUEE_SPIN_MS) {
                s_neon_spin_last_ms = now;
                const int z = s_neon_spin_tick % NEON_BULB_RINGS;
                s_neon_spin_tick++;
                /* The accent pair slides by dir, so the union of the OLD and
                 * NEW accent residues is three consecutive bulbs whose first
                 * residue is base_old for dir=-1 (pair slides to higher i)
                 * but base_new for dir=+1 (pair slides to lower i). Using
                 * the post-advance base alone would offset the boxes by one
                 * residue on the counterclockwise ring and leave the
                 * newly-darkened bulb stale. */
                const int base_old = neon_accent_base(z);
                s_neon_spin_phase[z] = (s_neon_spin_phase[z]
                                        + s_neon_spin_dir[z] + 6) % 6;
                const int base_new = neon_accent_base(z);
                if (z <= z_new) {
                    /* The accent pair slides one bulb, so the union of old
                     * and new accents spans 3 consecutive residues (first,
                     * first+1, first+2) per group - 12 boxes, like a zone
                     * flip. */
                    const int first = (s_neon_spin_dir[z] == -1) ? base_old : base_new;
                    for (int k = 0; k < NEON_BULB_N(z) / 6; ++k) {
                        int bx[3], by[3];
                        for (int j = 0; j < 3; ++j) {
                            neon_bulb_pos(px_icx(), px_icy(), z,
                                          k * 6 + (first + j) % 6,
                                          &bx[j], &by[j]);
                        }
                        const int pad = NEON_BULB_HALF + 1;
                        lv_area_t pb = {
                            (bx[0] < bx[1] ? bx[0] : bx[1]) < bx[2]
                                ? (bx[0] < bx[1] ? bx[0] : bx[1]) : bx[2],
                            (by[0] < by[1] ? by[0] : by[1]) < by[2]
                                ? (by[0] < by[1] ? by[0] : by[1]) : by[2],
                            (bx[0] > bx[1] ? bx[0] : bx[1]) > bx[2]
                                ? (bx[0] > bx[1] ? bx[0] : bx[1]) : bx[2],
                            (by[0] > by[1] ? by[0] : by[1]) > by[2]
                                ? (by[0] > by[1] ? by[0] : by[1]) : by[2],
                        };
                        pb.x1 -= pad; pb.y1 -= pad; pb.x2 += pad; pb.y2 += pad;
                        lv_obj_invalidate_area(s_neon_face, &pb);
                    }
                }
            }
        }
        if (z_old != z_new) {
            const int z_lo = z_old < z_new ? z_old : z_new;
            const int z_hi = z_old < z_new ? z_new : z_old;
            for (int z = z_lo + 1; z <= z_hi; ++z) {
                /* 12 pairs per ring; the pair starts at the ring's accent
                 * base at its CURRENT phase (neon_accent_base), so a flip
                 * mid-chase invalidates exactly the bulbs that are lit now. */
                const int base = neon_accent_base(z);
                for (int k = 0; k < NEON_BULB_N(z) / 6; ++k) {
                    int bx1, by1, bx2, by2;
                    neon_bulb_pos(px_icx(), px_icy(), z, k * 6 + base, &bx1, &by1);
                    neon_bulb_pos(px_icx(), px_icy(), z, k * 6 + base + 1, &bx2, &by2);
                    const int pad = NEON_BULB_HALF + 1;
                    lv_area_t pb = {
                        (bx1 < bx2 ? bx1 : bx2) - pad,
                        (by1 < by2 ? by1 : by2) - pad,
                        (bx1 > bx2 ? bx1 : bx2) + pad,
                        (by1 > by2 ? by1 : by2) + pad,
                    };
                    lv_obj_invalidate_area(s_neon_face, &pb);
                }
            }
        }
    }
    if (r.count != s_neon_cell_n) {
        /* Same overhang allowance as the per-cell path, applied to the union
         * of the old and new compositions - plus, per cell, the sprite's own
         * footprint (see neon_cell_x_span()). Vertically the whole box can be
         * tightened to the union of the baked sprites' own extents when every
         * element has an immutable full-size tile (tube/segments); anything
         * without one - live-label fallback, the marquee scaled set - keeps
         * the conservative cell_top..cell_bot, which is exactly what
         * cell_top/cell_bot were already computed to be. */
#if BOOST_NEON_GLYPH_SPRITES
        int ty0 = cell_top, ty1 = cell_bot;
        bool all_tight = true;
#endif
        lv_area_t whole = { px_icx() - r.half_w - cell_pad, cell_top,
                            px_icx() + r.half_w + cell_pad, cell_bot };
        for (uint8_t i = 0; i < r.count; ++i) {
            int lo, hi;
            neon_cell_x_span(px_icx(), r.cells[i].x, inv_slot, cell_pad,
                             neon_glyph_index(r.cells[i].ch), &lo, &hi);
            if (lo < whole.x1) whole.x1 = lo;
            if (hi > whole.x2) whole.x2 = hi;
#if BOOST_NEON_GLYPH_SPRITES
            neon_y_fold(cell_top, neon_glyph_index(r.cells[i].ch), cell_top, cell_bot, &ty0, &ty1, &all_tight);
#endif
        }
        for (uint8_t i = 0; i < s_neon_cell_n; ++i) {
            int lo, hi;
            neon_cell_x_span(px_icx(), s_neon_cell_x[i], inv_slot, cell_pad,
                             neon_glyph_index(s_neon_cell_ch[i]), &lo, &hi);
            if (lo < whole.x1) whole.x1 = lo;
            if (hi > whole.x2) whole.x2 = hi;
#if BOOST_NEON_GLYPH_SPRITES
            neon_y_fold(cell_top, neon_glyph_index(s_neon_cell_ch[i]), cell_top, cell_bot, &ty0, &ty1, &all_tight);
#endif
        }
        /* Both the old and the new sign position: crossing +-10.0 changes the
         * cell count AND slides the sign, and covering only the new one leaves
         * the old mark stranded. */
        if (r.sign) {
            int sl, sh;
            neon_sign_x_span(px_icx(), r.sign_x, inv_sign_pad + cell_pad, &sl, &sh);
            if (sl < whole.x1) whole.x1 = sl;
#if BOOST_NEON_GLYPH_SPRITES
            neon_y_fold(cell_top, NEON_SIGN_SLOT, cell_top, cell_bot, &ty0, &ty1, &all_tight);
#endif
        }
        if (s_neon_sign_drawn) {
            int sl, sh;
            neon_sign_x_span(px_icx(), s_neon_sign_x, inv_sign_pad + cell_pad, &sl, &sh);
            if (sl < whole.x1) whole.x1 = sl;
#if BOOST_NEON_GLYPH_SPRITES
            neon_y_fold(cell_top, NEON_SIGN_SLOT, cell_top, cell_bot, &ty0, &ty1, &all_tight);
#endif
        }
#if BOOST_NEON_GLYPH_SPRITES
        if (all_tight) {
            whole.y1 = ty0;
            whole.y2 = ty1;
        }
#endif
        lv_obj_invalidate_area(s_neon_face, &whole);
    } else {
        for (uint8_t i = 0; i < r.count; ++i) {
            if (r.cells[i].ch == s_neon_cell_ch[i] &&
                r.cells[i].x == s_neon_cell_x[i] && !color_flip) continue;
            int lo, hi;
            /* The tight scaled span is per-glyph, so a changed cell must cover
             * the OLD glyph's footprint too - the previous occupant may have
             * been wider or sat differently, and the old code's uniform label
             * box hid that by construction. */
            neon_cell_x_span(px_icx(), r.cells[i].x, inv_slot, cell_pad,
                             neon_glyph_index(r.cells[i].ch), &lo, &hi);
            int lo2, hi2;
            neon_cell_x_span(px_icx(), s_neon_cell_x[i], inv_slot, cell_pad,
                             neon_glyph_index(s_neon_cell_ch[i]), &lo2, &hi2);
            if (lo2 < lo) lo = lo2;
            if (hi2 > hi) hi = hi2;
#if BOOST_NEON_GLYPH_SPRITES
            /* Same per-glyph tightening as the horizontal spans: the changed
             * cell covers the union of the OLD and NEW glyphs' own vertical
             * extents (plus 1 px AA), falling back to the conservative box
             * when either lacks an immutable full-size tile. */
            int t0 = cell_top, t1 = cell_bot;
            bool tight = true;
            neon_y_fold(cell_top, neon_glyph_index(r.cells[i].ch), cell_top, cell_bot, &t0, &t1, &tight);
            neon_y_fold(cell_top, neon_glyph_index(s_neon_cell_ch[i]), cell_top, cell_bot, &t0, &t1, &tight);
            lv_area_t cell = { lo, tight ? t0 : cell_top, hi, tight ? t1 : cell_bot };
#else
            lv_area_t cell = { lo, cell_top, hi, cell_bot };
#endif
            lv_obj_invalidate_area(s_neon_face, &cell);
        }
        if (r.sign != s_neon_sign_drawn || r.sign_x != s_neon_sign_x ||
            (r.sign && color_flip)) {
            const int lo = (r.sign_x < s_neon_sign_x) ? r.sign_x : s_neon_sign_x;
            const int hi = (r.sign_x > s_neon_sign_x) ? r.sign_x : s_neon_sign_x;
            int l0, l1, h0, h1;
            neon_sign_x_span(px_icx(), lo, inv_sign_pad + cell_pad, &l0, &l1);
            neon_sign_x_span(px_icx(), hi, inv_sign_pad + cell_pad, &h0, &h1);
#if BOOST_NEON_GLYPH_SPRITES
            /* Tighten to the baked sign sprite's own vertical extent; the
             * live-bar fallback keeps the conservative box. */
            int s0 = cell_top, s1 = cell_bot;
            bool tight = true;
            neon_y_fold(cell_top, NEON_SIGN_SLOT, cell_top, cell_bot, &s0, &s1, &tight);
            lv_area_t sg = { l0, tight ? s0 : cell_top, h1, tight ? s1 : cell_bot };
#else
            lv_area_t sg = { l0, cell_top, h1, cell_bot };
#endif
            lv_obj_invalidate_area(s_neon_face, &sg);
        }
    }
    for (uint8_t i = 0; i < r.count; ++i) {
        s_neon_cell_ch[i] = r.cells[i].ch;
        s_neon_cell_x[i] = r.cells[i].x;
    }
    s_neon_cell_n = r.count;
    s_neon_sign_drawn = r.sign;
    s_neon_sign_x = r.sign_x;
}

#if LV_USE_GIF
static lv_obj_t *s_media_gif;
static lv_image_dsc_t s_media_dsc;
#endif

static float s_display_psi;
static float s_peak_psi;

/* Shared visual-only arc animation. Raw samples remain authoritative for
 * readouts, peaks, zones, and model/API state; this state drives only the
 * Dyno Cell and Night City moving arcs. */
#define ARC_ANIM_GAP_RESET_MS 1000u
#define ARC_ANIM_DURATION_MS   40u
static uint32_t s_arc_anim_start_ms;
static bool s_arc_anim_valid;
static float s_arc_anim_from_psi;
static float s_arc_anim_target_psi;
static float s_arc_anim_display_psi;
static float s_arc_drawn_psi;
static float s_arc_color_psi;
/* The "PEAK  x.x" label is only regenerated when the peak value actually
 * moves. Formatting `%.1f` through a double runs soft-float conversion on
 * the S3 (no double FPU) and is pure waste on the ticks where the peak is
 * constant - which is nearly all of them once the sweep's maximum is set.
 * The cached text is a pure function of s_peak_psi (updated once per
 * boost_gauge_update() and on peak reset), so it can never diverge from
 * what the label should show; the strcmp in update_arc() still guards the
 * actual lv_label_set_text(), including the "PEAK  0.0" build text after a
 * scene rebuild. NAN != anything, so the first tick after boot always
 * formats. */
static float s_arc_peak_text_psi = NAN;
static char s_arc_peak_text[32] = "PEAK  0.0";

static bool s_ui_ready;
static char s_theme_id[BOOST_THEME_ID_MAX];
static float s_psi_min = DEFAULT_PSI_MIN;
static float s_psi_max = DEFAULT_PSI_MAX;
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

static void reset_visual_arc_animation(float psi)
{
    const float seed = isfinite(psi) ? psi : 0.0f;
    s_arc_anim_start_ms = lv_tick_get();
    s_arc_anim_valid = false;
    s_arc_anim_from_psi = seed;
    s_arc_anim_target_psi = seed;
    s_arc_anim_display_psi = seed;
}

/* Linear, target-directed interpolation for moving arc geometry only. Raw
 * pressure remains the newest target and all numeric/color state; the visual
 * endpoint eases from its currently displayed value toward that target. This
 * removes pacing steps without averaging away small measured changes. */
static float update_visual_arc_animation(float target)
{
    const uint32_t now = lv_tick_get();
    if (!isfinite(target)) {
        reset_visual_arc_animation(0.0f);
        return 0.0f;
    }
    if (!s_arc_anim_valid || lv_tick_elaps(s_arc_anim_start_ms) > ARC_ANIM_GAP_RESET_MS) {
        s_arc_anim_valid = true;
        s_arc_anim_start_ms = now;
        s_arc_anim_from_psi = target;
        s_arc_anim_target_psi = target;
        s_arc_anim_display_psi = target;
        return target;
    }

    const float progress = clampf((float)lv_tick_elaps(s_arc_anim_start_ms) /
                                  (float)ARC_ANIM_DURATION_MS, 0.0f, 1.0f);
    s_arc_anim_display_psi = s_arc_anim_from_psi +
        (s_arc_anim_target_psi - s_arc_anim_from_psi) * progress;
    if (target != s_arc_anim_target_psi) {
        s_arc_anim_from_psi = s_arc_anim_display_psi;
        s_arc_anim_target_psi = target;
        s_arc_anim_start_ms = now;
    }
    return s_arc_anim_display_psi;
}

/*
 * Face centre in SCREEN coordinates, including the burn-in offset.
 *
 * Draw callbacks get a layer in absolute screen space, so a callback that
 * hardcodes DISP_SIZE/2 keeps painting at the unshifted centre while the object
 * it belongs to has moved â€” the moving parts detach from the static art. Every
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

/* One fixed vacuum sentinel plus 24 buckets devoted exclusively to positive
 * pressure. The old full-range quantizer spent most slots on identical vacuum
 * colors, so its nominal step count overstated the visible ramp. */
#define BIG_POSITIVE_STEPS 24
static int big_step_for(float psi);
static uint32_t big_color_for_step(const boost_theme_t *theme, int step);
static lv_color_t gradient_lut_color(const boost_theme_t *theme, int step);
static lv_color_t gradient_color_for_psi(const boost_theme_t *theme, float psi);

/* The vacuum->boost->overboost ramp Big Digit sweeps, quantised so a fill only
 * recolours at a positive-pressure bucket boundary rather than every frame.
 * Shared by the arc and hud gradient-fill modes. */
static lv_color_t gradient_color_for_psi(const boost_theme_t *theme, float psi)
{
    return gradient_lut_color(theme, big_step_for(psi));
}

static lv_color_t color_for_psi(const boost_theme_t *theme, float psi)
{
    if (boost_theme_arc_gradient()) return gradient_color_for_psi(theme, psi);
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

static lv_color_t zone_color_for_psi(const boost_theme_t *theme, float psi)
{
    if (psi >= s_psi_overboost) return c(theme->overboost);
    if (psi >= 0.35f) return c(theme->boost);
    if (psi > -0.35f) return c(theme->text);
    return c(theme->vacuum);
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

static int hud_readout_glyph_index(uint32_t letter)
{
    if (letter >= '0' && letter <= '9') return (int)(letter - '0');
    if (letter == '.') return 10;
    if (letter == '-') return 11;
    return -1;
}

static bool hud_readout_get_glyph_dsc(const lv_font_t *font, lv_font_glyph_dsc_t *dsc,
                                      uint32_t letter, uint32_t letter_next)
{
    (void)letter_next;
    if (font == NULL || dsc == NULL || font->dsc == NULL) return false;
    const hud_readout_font_t *cache = font->dsc;
    const int index = hud_readout_glyph_index(letter);
    if (index < 0 || (uint32_t)index >= HUD_READOUT_GLYPH_COUNT) return false;
    *dsc = cache->glyph[index];
    return true;
}

static const void *hud_readout_get_glyph_bitmap(lv_font_glyph_dsc_t *dsc,
                                                lv_draw_buf_t *draw_buf)
{
    (void)draw_buf;
    if (dsc == NULL || dsc->resolved_font == NULL || dsc->resolved_font->dsc == NULL) return NULL;
    const hud_readout_font_t *cache = dsc->resolved_font->dsc;
    if (dsc->resolved_font != &cache->font || dsc->gid.index >= HUD_READOUT_GLYPH_COUNT ||
        cache->pixels == NULL || dsc->format != LV_FONT_GLYPH_FORMAT_A8 ||
        dsc->box_w != cache->glyph[dsc->gid.index].box_w ||
        dsc->box_h != cache->glyph[dsc->gid.index].box_h ||
        dsc->stride == 0 || dsc->stride != cache->glyph[dsc->gid.index].stride) return NULL;
    const size_t offset = cache->offset[dsc->gid.index];
    const size_t bytes = (size_t)dsc->stride * (size_t)dsc->box_h;
    if (offset > cache->pixels_size || bytes > cache->pixels_size - offset) return NULL;
    return cache->pixels + offset;
}

static bool hud_readout_size_mul(size_t a, size_t b, size_t *out)
{
    if (out == NULL || (b != 0 && a > SIZE_MAX / b)) return false;
    *out = a * b;
    return true;
}

static bool hud_readout_round_up(size_t value, size_t alignment, size_t *out)
{
    if (out == NULL || alignment == 0) return false;
    const size_t remainder = value % alignment;
    const size_t add = remainder == 0 ? 0 : alignment - remainder;
    if (value > SIZE_MAX - add) return false;
    *out = value + add;
    return true;
}

static bool hud_readout_glyph_consistent(const lv_font_glyph_dsc_t *a,
                                         const lv_font_glyph_dsc_t *b)
{
    if (a == NULL || b == NULL) return false;
    return a->adv_w == b->adv_w && a->box_w == b->box_w && a->box_h == b->box_h &&
           a->ofs_x == b->ofs_x && a->ofs_y == b->ofs_y && a->stride == b->stride &&
           a->format == b->format && a->is_placeholder == b->is_placeholder &&
           a->req_raw_bitmap == b->req_raw_bitmap &&
           a->outline_stroke_width == b->outline_stroke_width &&
           a->resolved_font == b->resolved_font;
}

static bool hud_readout_source_glyph_valid(const lv_font_glyph_dsc_t *source)
{
    if (source == NULL || source->resolved_font == NULL || source->is_placeholder ||
        source->box_w == 0 || source->box_h == 0) return false;
    const uint32_t expected_stride = lv_draw_buf_width_to_stride(source->box_w, LV_COLOR_FORMAT_A8);
    return expected_stride != 0;
}

static void destroy_hud_readout_font(void)
{
    if (s_hud_readout_font.pixels != NULL) BG_FREE(s_hud_readout_font.pixels);
    memset(&s_hud_readout_font, 0, sizeof(s_hud_readout_font));
}

static bool build_hud_readout_font(void)
{
#if !BOOST_HUD_READOUT_CACHE
    return false;
#else
    destroy_hud_readout_font();

    size_t total = 0;
    for (uint32_t index = 0; index < HUD_READOUT_GLYPH_COUNT; ++index) {
        const uint32_t letter = index < 10 ? '0' + index : index == 10 ? '.' : '-';
        lv_font_glyph_dsc_t *g = &s_hud_readout_font.glyph[index];
        if (!lv_font_get_glyph_dsc(HUD_VALUE_FONT, g, letter, 0) ||
            !hud_readout_source_glyph_valid(g)) goto fail;
        g->gid.index = index;
        size_t aligned = 0;
        size_t bytes = 0;
        if (!hud_readout_round_up(total, LV_DRAW_BUF_ALIGN, &aligned) ||
            !hud_readout_size_mul((size_t)lv_draw_buf_width_to_stride(g->box_w, LV_COLOR_FORMAT_A8),
                                  (size_t)g->box_h, &bytes) ||
            aligned > SIZE_MAX - bytes) goto fail;
        total = aligned + bytes;
    }
    if (total == 0 || total > UINT32_MAX) goto fail;

    s_hud_readout_font.pixels = BG_ALLOC(total);
    if (s_hud_readout_font.pixels == NULL) goto fail;
    s_hud_readout_font.pixels_size = total;

    size_t offset = 0;
    for (uint32_t index = 0; index < HUD_READOUT_GLYPH_COUNT; ++index) {
        size_t aligned = 0;
        if (!hud_readout_round_up(offset, LV_DRAW_BUF_ALIGN, &aligned) || aligned > total) goto fail;
        offset = aligned;
        const uint32_t letter = index < 10 ? '0' + index : index == 10 ? '.' : '-';
        lv_font_glyph_dsc_t source;
        lv_font_glyph_dsc_t verify;
        memset(&source, 0, sizeof(source));
        memset(&verify, 0, sizeof(verify));
        if (!lv_font_get_glyph_dsc(HUD_VALUE_FONT, &source, letter, 0) ||
            !lv_font_get_glyph_dsc(HUD_VALUE_FONT, &verify, letter, 0) ||
            !hud_readout_source_glyph_valid(&source) ||
            !hud_readout_glyph_consistent(&source, &verify)) goto fail;
        const uint32_t stride = lv_draw_buf_width_to_stride(source.box_w, LV_COLOR_FORMAT_A8);
        size_t bytes = 0;
        if (!hud_readout_size_mul((size_t)stride, (size_t)source.box_h, &bytes) ||
            stride == 0 || bytes > total - offset) goto fail;
        lv_draw_buf_t draw_buf;
        if (lv_draw_buf_init(&draw_buf, source.box_w, source.box_h, LV_COLOR_FORMAT_A8,
                             stride, s_hud_readout_font.pixels + offset, bytes) != LV_RESULT_OK) goto fail;
        const lv_draw_buf_t *bitmap = lv_font_get_glyph_bitmap(&source, &draw_buf);
        const bool valid = bitmap != NULL && bitmap->data == draw_buf.data &&
                           bitmap->header.cf == LV_COLOR_FORMAT_A8 && bitmap->header.w == source.box_w &&
                           bitmap->header.h == source.box_h && bitmap->header.stride == stride &&
                           bitmap->data_size >= bytes;
        lv_font_glyph_release_draw_data(&source);
        if (!valid) goto fail;

        if (offset > UINT32_MAX) goto fail;
        s_hud_readout_font.offset[index] = (uint32_t)offset;
        s_hud_readout_font.glyph[index].resolved_font = &s_hud_readout_font.font;
        s_hud_readout_font.glyph[index].format = LV_FONT_GLYPH_FORMAT_A8;
        s_hud_readout_font.glyph[index].stride = stride;
        offset += bytes;
    }
    if (offset > total) goto fail;

    s_hud_readout_font.font.get_glyph_dsc = hud_readout_get_glyph_dsc;
    s_hud_readout_font.font.get_glyph_bitmap = hud_readout_get_glyph_bitmap;
    s_hud_readout_font.font.line_height = HUD_VALUE_FONT->line_height;
    s_hud_readout_font.font.base_line = HUD_VALUE_FONT->base_line;
    s_hud_readout_font.font.static_bitmap = 1;
    s_hud_readout_font.font.dsc = &s_hud_readout_font;
    ESP_LOGI(TAG, "hud readout font cache: %u B", (unsigned)total);
    return true;

fail:
    ESP_LOGW(TAG, "hud readout font cache unavailable; using source font");
    destroy_hud_readout_font();
    return false;
#endif
}

static const lv_font_t *hud_readout_font(bool cached)
{
    return cached ? &s_hud_readout_font.font : HUD_VALUE_FONT;
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
    dsc.color = color_for_psi(active_theme(), s_arc_color_psi);
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

static void set_value_arc(float psi, float raw_color_psi)
{
    const float color_psi = isfinite(raw_color_psi) ? raw_color_psi : 0.0f;
    if (psi == s_arc_drawn_psi && color_psi == s_arc_color_psi) {
        /* The wedge is already committed at this endpoint and this colour.
         * The old code reached the same state, found no side flip and no
         * colour flip, and then invalidated zero-length spans - a no-op.
         * Skip the angle/colour recomputation and the empty invalidations
         * on dwell ticks. The theme is stable within a scene (any config
         * change rebuilds it), so equal colour inputs cannot have changed
         * the committed colour, and s_arc_color_psi is unchanged either way. */
        return;
    }
    float old_start, old_end, new_start, new_end;
    value_arc_angles(s_arc_drawn_psi, &old_start, &old_end);
    value_arc_angles(psi, &new_start, &new_end);
    const bool color_flip = !lv_color_eq(color_for_psi(active_theme(), s_arc_color_psi),
                                         color_for_psi(active_theme(), color_psi));
    const bool side_flip = (s_arc_drawn_psi < 0.0f) != (psi < 0.0f);

    if (side_flip) {
        /* Opposite sides of the zero gap are disjoint. */
        invalidate_value_arc(old_start, old_end);
        invalidate_value_arc(new_start, new_end);
    } else if (color_flip) {
        /* Same-side full spans overlap from zero to the endpoint. Repaint their
         * union once instead of submitting the shared pixels twice. */
        invalidate_value_arc(fminf(old_start, new_start), fmaxf(old_end, new_end));
    } else if (psi >= 0.0f) {
        invalidate_value_arc(fminf(old_end, new_end), fmaxf(old_end, new_end));
    } else {
        invalidate_value_arc(fminf(old_start, new_start), fmaxf(old_start, new_start));
    }
    s_arc_color_psi = color_psi;
}

/* Paint the static arc face â€” unfilled track, scale numerals and the "PSI"
 * unit mark â€” into an off-screen canvas ONCE. Redraws
 * then become a blit instead of re-rasterising a 270 degree, 45 px-wide ring
 * (by far the largest draw on this face) inside every wedge-invalidated dirty
 * region. Mirrors paint_vault_background()/build_hud()'s canvas fill; only
 * the moving wedge, readout, peak and zone label stay live above it.
 *
 * The unfilled-track arc below reproduces byte-for-byte the lv_draw_arc call
 * the live s_arc_track lv_arc widget used to issue for LV_PART_MAIN (radius
 * from its get_center(), which is ARC_DIAMETER/2 with zero padding here).
 * LV_PART_INDICATOR was always LV_OPA_0 and drew nothing, so it is not
 * reproduced. */
static void paint_arc_background(lv_obj_t *canvas, const boost_theme_t *theme)
{
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = c(theme->face);
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = { 0, 0, DISP_SIZE - 1, DISP_SIZE - 1 };
    lv_draw_rect(&layer, &bg, &full);

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = c(theme->track);
    arc.width = ARC_WIDTH;
    arc.start_angle = ARC_START;
    arc.end_angle = ARC_END;
    arc.center.x = (int32_t)lroundf(cx);
    arc.center.y = (int32_t)lroundf(cy);
    arc.radius = ARC_DIAMETER / 2;
    arc.opa = LV_OPA_60;
    arc.rounded = true;
    lv_draw_arc(&layer, &arc);

    compute_tick_psis();
    for (int i = 0; i < 5; ++i) {
        if (!isfinite(s_tick_psi[i])) continue;
        char text[12];
        format_tick_text(text, sizeof(text), s_tick_psi[i]);

        const float deg = psi_to_angle(s_tick_psi[i]);
        const float rad = deg * (float)M_PI / 180.0f;
        float r = TICK_RADIUS;
        if (fabsf(s_tick_psi[i]) < 0.01f) r = TICK_RADIUS - 18.0f;

        lv_point_t size;
        lv_text_get_size(&size, text, TICK_FONT, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const float x = cx + r * cosf(rad) - (float)size.x * 0.5f;
        const float y = cy + r * sinf(rad) - (float)size.y * 0.5f;

        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = TICK_FONT;
        d.text = text;
        /* `text` is a loop-local stack buffer, and lv_canvas_finish_layer()
         * only dispatches (rasterises) queued tasks after every iteration has
         * returned, so the draw task must own a copy rather than a pointer
         * into a since-reused stack slot. */
        d.text_local = 1;
        d.color = c(theme->muted);
        d.align = LV_TEXT_ALIGN_LEFT;
        lv_area_t a = {
            (int32_t)lroundf(x), (int32_t)lroundf(y),
            (int32_t)lroundf(x) + size.x - 1, (int32_t)lroundf(y) + size.y - 1,
        };
        lv_draw_label(&layer, &d, &a);
    }

    /* "PSI" unit mark: text never changes, so it bakes in with everything
     * else. The live label used lv_obj_align(LV_ALIGN_CENTER, 0, 26) against
     * a DISP_SIZE-sized parent centred at (cx, cy); reproduce that placement
     * from measured text size. */
    {
        static const char *const unit_text = "PSI";
        lv_point_t size;
        lv_text_get_size(&size, unit_text, &lv_font_montserrat_16, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        const float x = cx - (float)size.x * 0.5f;
        const float y = cy + 26.0f - (float)size.y * 0.5f;

        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = &lv_font_montserrat_16;
        d.text = unit_text;
        d.color = c(theme->muted);
        d.align = LV_TEXT_ALIGN_LEFT;
        lv_area_t a = {
            (int32_t)lroundf(x), (int32_t)lroundf(y),
            (int32_t)lroundf(x) + size.x - 1, (int32_t)lroundf(y) + size.y - 1,
        };
        lv_draw_label(&layer, &d, &a);
    }

    lv_canvas_finish_layer(canvas, &layer);
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

static void refresh_zero_notch(void)
{
    if (s_zero_notch == NULL) return;
    static lv_point_precise_t points[2];
    const float rad = psi_to_angle(0.0f) * (float)M_PI / 180.0f;
    const float cx = DISP_SIZE * 0.5f;
    const float cy = DISP_SIZE * 0.5f;
    const float r_outer = (float)ARC_DIAMETER * 0.5f - 1.0f;
    const float r_inner = r_outer - (float)ARC_WIDTH + 1.0f;
    points[0].x = cx + r_inner * cosf(rad);
    points[0].y = cy + r_inner * sinf(rad);
    points[1].x = cx + r_outer * cosf(rad);
    points[1].y = cy + r_outer * sinf(rad);
    lv_line_set_points(s_zero_notch, points, 2);
}

static void build_arc(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    /* Static face (unfilled track, scale numerals, "PSI" mark) is
     * rasterised once into PSRAM and blitted thereafter â€” the same win
     * vault/hud got. A failed allocation degrades the same way vault's does:
     * warn and skip the cached art rather than adding a second fallback
     * convention. */
    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    s_arc_bg_buf = BG_ALLOC(bg_bytes);
    if (s_arc_bg_buf != NULL) {
        s_arc_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_arc_bg, s_arc_bg_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_arc_bg);
        lv_obj_clear_flag(s_arc_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        paint_arc_background(s_arc_bg, theme);
    } else {
        ESP_LOGW(TAG, "arc background cache alloc failed (%u B)", (unsigned)bg_bytes);
    }

    s_arc_value_canvas = lv_obj_create(scr);
    lv_obj_remove_style_all(s_arc_value_canvas);
    lv_obj_set_size(s_arc_value_canvas, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_arc_value_canvas);
    lv_obj_clear_flag(s_arc_value_canvas, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_arc_value_canvas, draw_value_arc, LV_EVENT_DRAW_MAIN, &s_arc_drawn_psi);

    /* Created after the value arc so the zero reference remains visible over
     * both vacuum and boost fills, matching the original verified layering. */
    s_zero_notch = lv_line_create(scr);
    refresh_zero_notch();
    lv_obj_set_style_line_width(s_zero_notch, ZERO_LINE_W, 0);
    lv_obj_set_style_line_color(s_zero_notch, c(theme->zero), 0);
    lv_obj_set_style_line_rounded(s_zero_notch, true, 0);
    lv_obj_set_style_line_opa(s_zero_notch, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_zero_notch, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

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
    const float visual_psi = update_visual_arc_animation(sample->psi);
    set_value_arc(visual_psi, sample->psi);
    s_arc_drawn_psi = visual_psi;

    const lv_color_t zone_color = zone_color_for_psi(theme, sample->psi);
    const char *zone = zone_for_psi(sample->psi);
    if (!lv_color_eq(lv_obj_get_style_text_color(s_zone_label, 0), zone_color)) {
        lv_obj_set_style_text_color(s_zone_label, zone_color, 0);
    }
    if (strcmp(lv_label_get_text(s_zone_label), zone) != 0) {
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

    /* Only regenerate the "PEAK  x.x" text when the peak value actually
     * moves; the soft-float double conversion in `%.1f` is otherwise wasted
     * on every tick while the peak sits at the sweep's maximum. */
    if (s_peak_psi != s_arc_peak_text_psi) {
        snprintf(s_arc_peak_text, sizeof(s_arc_peak_text), "PEAK  %.1f",
                 (double)s_peak_psi);
        s_arc_peak_text_psi = s_peak_psi;
    }
    if (strcmp(lv_label_get_text(s_peak_label), s_arc_peak_text) != 0) {
        lv_label_set_text(s_peak_label, s_arc_peak_text);
    }
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
static bool paint_vault_background(lv_obj_t *canvas, const boost_theme_t *theme)
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
     * nothing after build. Scanlines are NOT baked here â€” they belong on top.
     *
     * The ramp is then dithered, and it has to be. The face is #02100a, whose
     * green sits at level 4 of 63 in RGB565, so darkening it toward black has
     * only four distinct values to land on â€” an exact gradient still resolves
     * to four flat rings. The web mirror looks smooth only because canvas is
     * 8-bit (green 16 -> 6). Ordered dithering trades spatial noise for tonal
     * resolution, which is the only way to get a smooth ramp out of a channel
     * this dark. The pattern is baked, so it never shimmers. */
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
    if (db == NULL) return false;

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
        return false; /* face is usable, but retry the dither on the next build */
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
    return true;
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
     * rather than staying pinned to absolute rows â€” otherwise the three rows
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

static float vault_needle_tail_px(void)
{
    return boost_theme_vault_needle_tail() ? (float)VAULT_NEEDLE_TAIL_LEN : 0.0f;
}

static void draw_vault_needle(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const float cx = px_cx();
    const float cy = px_cy();
    const float rad = s_vault_needle_deg * (float)M_PI / 180.0f;

    const lv_color_t needle_col = s_vault_needle_red
                                      ? c(VAULT_NEEDLE_RED)
                                      : (s_vault_needle_over ? c(theme->overboost)
                                                              : c(theme->text));
    const float nx = cosf(rad);
    const float ny = sinf(rad);
    const float px = -ny; /* perpendicular */
    const float py = nx;
    const float hw = (float)VAULT_NEEDLE_HALFW;

    /* Tapered wedge: wide at the hub, a point at the tip. Two triangles form
     * the quad, and triangles are cheaper than a wide rounded line. */
    const float tail = vault_needle_tail_px();
    const float bx = cx - tail * nx;
    const float by = cy - tail * ny;
    const float tx = cx + (float)VAULT_NEEDLE_LEN * nx;
    const float ty = cy + (float)VAULT_NEEDLE_LEN * ny;

    /* Two triangles exactly tile the convex trapezoid. A former third A-B-D
     * triangle was wholly inside this union and paid LVGL's full mask/raster
     * cost again without adding geometry. */
    const float tipw = (float)VAULT_NEEDLE_TIP_HALF;
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
        /* Both configured radial ends; the trial uses a zero-length tail while
         * the baseline override restores the 26 px counterweight. */
        const float rs[2] = { -vault_needle_tail_px(), (float)VAULT_NEEDLE_LEN };
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

    /* Shaft: one box per radial slice, spanning both angles. The wedge is
     * tapered - VAULT_NEEDLE_HALFW wide at the base, VAULT_NEEDLE_TIP_HALF at
     * the tip - so the perpendicular pad only needs the local half-width at
     * this slice's inner radius (the half-width shrinks linearly outward),
     * plus the AA pixel. Slice 0 keeps the full base width and the hub fold,
     * so the change only tightens the outer slices' boxes. */
    const float c0 = cosf(old_deg * (float)M_PI / 180.0f);
    const float s0 = sinf(old_deg * (float)M_PI / 180.0f);
    const float c1 = cosf(new_deg * (float)M_PI / 180.0f);
    const float s1 = sinf(new_deg * (float)M_PI / 180.0f);
    const float r_lo = -vault_needle_tail_px();
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
            /* Fold the hub into slice 0 whether the configured tail is zero or
             * extends behind the pivot. */
            if (cx - hub < minx) minx = cx - hub;
            if (cx + hub > maxx) maxx = cx + hub;
            if (cy - hub < miny) miny = cy - hub;
            if (cy + hub > maxy) maxy = cy + hub;
        }
        const float hw_at_ra = (float)VAULT_NEEDLE_HALFW
            + ((float)VAULT_NEEDLE_TIP_HALF - (float)VAULT_NEEDLE_HALFW)
              * (ra - r_lo) / (r_hi - r_lo);
        const float rmax = (ra < 0.0f ? -ra : ra) > rb ? (ra < 0.0f ? -ra : ra) : rb;
        vault_inv_box(minx, miny, maxx, maxy, hw_at_ra + 2.0f + rmax * bulge);
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

static void vault_readout_area(int index, lv_area_t *area)
{
    if (area == NULL || index < 0 || index >= VAULT_SLOT_COUNT) return;
    const int32_t x = px_icx() + k_vault_slot_x[index];
    const int32_t y = px_icy() + 130;
    area->x1 = x - 13;
    area->y1 = y - 17;
    area->x2 = x + 12;
    area->y2 = y + 16;
}

static void draw_vault_readout(lv_event_t *e)
{
    if (s_vault_readout == NULL) return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = F_MONO40;
    d.color = s_vault_readout_color_valid ? s_vault_readout_color : c(active_theme()->text);
    d.align = LV_TEXT_ALIGN_CENTER;
    d.text_local = 1;
    for (int i = 0; i < VAULT_SLOT_COUNT; ++i) {
        if (s_vault_slot_text[i][0] == '\0') continue;
        lv_area_t area;
        vault_readout_area(i, &area);
        /* A digit change dirties one 26x34 slot; without this, LVGL allocates
         * a label draw task and clip-tests all six slots for every readout
         * repaint. A skipped slot's ink is inside its own box, and non-adjacent
         * slot boxes are 23 px apart, so it cannot reach this dirty region. */
        if (!neon_area_overlaps(&area, &layer->_clip_area)) continue;
        d.text = s_vault_slot_text[i];
        lv_draw_label(layer, &d, &area);
    }
}

static void build_vault(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();

    /* Keep the expensive completed face across theme switches. Its serial
     * error-diffusion pass touches every pixel, so rebuilding it here made
     * every return to Vault pause for about a second. */
    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16, LV_DRAW_BUF_STRIDE_ALIGN);
    if (s_vault_bg_buf == NULL) s_vault_bg_buf = BG_ALLOC(bg_bytes);
    if (s_vault_bg_buf != NULL) {
        s_vault_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_vault_bg, s_vault_bg_buf, DISP_SIZE, DISP_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_vault_bg);
        lv_obj_clear_flag(s_vault_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        const vault_bg_key_t key = {
            .psi_min = s_psi_min,
            .psi_max = s_psi_max,
            .psi_overboost = s_psi_overboost,
            .zero_angle = s_zero_angle,
            .face = boost_theme_vault_face(),
            .text = theme->text,
            .muted = theme->muted,
            .overboost = theme->overboost,
            .vignette_pct = boost_theme_vault_vignette_pct(),
            .valid = true,
        };
        if (!s_vault_bg_key.valid ||
            s_vault_bg_key.psi_min != key.psi_min ||
            s_vault_bg_key.psi_max != key.psi_max ||
            s_vault_bg_key.psi_overboost != key.psi_overboost ||
            s_vault_bg_key.zero_angle != key.zero_angle ||
            s_vault_bg_key.face != key.face ||
            s_vault_bg_key.text != key.text ||
            s_vault_bg_key.muted != key.muted ||
            s_vault_bg_key.overboost != key.overboost ||
            s_vault_bg_key.vignette_pct != key.vignette_pct) {
            if (paint_vault_background(s_vault_bg, theme)) s_vault_bg_key = key;
        }
        /* The cached face is exactly screen-sized, so the burn-in shift slides
         * a margin of up to two pixels off one edge and exposes the screen's
         * own background at the other. By the time the face reaches its rim the
         * vignette has taken it down to (1 - VAULT_VIGN_MAX) of the face
         * colour, so match THAT, not the raw face â€” otherwise the margin reads
         * as a bright hairline tracing one side of the glass. The other three
         * styles need no such trick: hud's cached face and bigdigit's ground
         * are flat fills that already equal the screen background. */
        lv_obj_set_style_bg_color(s_scene_parent != NULL ? s_scene_parent : lv_screen_active(),
                                  c(scale_rgb(boost_theme_vault_face(),
                                              1.0f - (float)boost_theme_vault_vignette_pct() / 100.0f)), 0);
    } else {
        ESP_LOGW(TAG, "vault background cache alloc failed (%u B)", (unsigned)bg_bytes);
    }

    s_vault_peak_mark = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_peak_mark);
    lv_obj_set_size(s_vault_peak_mark, VAULT_PEAK_MARK_SIZE, VAULT_PEAK_MARK_SIZE);
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

    /* Fixed slots so digits never slide left/right as the value changes. One
     * draw object owns the same six 26x34 label boxes; unlike six separate
     * labels, it is visited once per dirty region. */
    s_vault_readout = lv_obj_create(scr);
    lv_obj_remove_style_all(s_vault_readout);
    /* Bound the object to the union of the six old 26x34 slots rather than
     * making a full-screen draw object: needle dirties outside this band must
     * not even enter the readout callback. */
    lv_obj_set_size(s_vault_readout, 146, 34);
    lv_obj_align(s_vault_readout, LV_ALIGN_CENTER, 0, 130);
    lv_obj_clear_flag(s_vault_readout, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_vault_readout, draw_vault_readout, LV_EVENT_DRAW_MAIN, NULL);
    memset(s_vault_slot_text, 0, sizeof(s_vault_slot_text));
    s_vault_slot_text[VAULT_SLOT_DOT][0] = '.';
    s_vault_readout_color = c(theme->text);
    s_vault_readout_color_valid = true;

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
    /* A scene rebuild does not reset the live reading. Seed the new needle
     * from the committed value so the first frame after a theme switch is
     * already in sync with the other faces. */
    s_vault_needle_deg = psi_to_sweep(s_display_psi, VAULT_A0, VAULT_A1);
    s_vault_needle_red = boost_theme_vault_needle_red();
    s_vault_needle_over = s_display_psi >= s_psi_overboost;

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
    const bool needle_red = boost_theme_vault_needle_red();
    const bool needle_over = sample->psi >= s_psi_overboost;
    /* Sub-degree jitter isn't visible but costs a full needle-sized repaint. */
    if (fabsf(deg - s_vault_needle_deg) > 0.35f || needle_red != s_vault_needle_red ||
        needle_over != s_vault_needle_over) {
        const float old = s_vault_needle_deg;
        s_vault_needle_deg = deg;
        s_vault_needle_red = needle_red;
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
    int dirty_lo = VAULT_SLOT_COUNT;
    int dirty_hi = -1;
    for (int i = 0; i < VAULT_SLOT_COUNT; ++i) {
        if (memcmp(s_vault_slot_text[i], slot_txt[i], sizeof(slot_txt[i])) != 0) {
            memcpy(s_vault_slot_text[i], slot_txt[i], sizeof(s_vault_slot_text[i]));
            if (i < dirty_lo) dirty_lo = i;
            if (i > dirty_hi) dirty_hi = i;
        }
    }
    const bool readout_color_changed = !s_vault_readout_color_valid ||
                                       !lv_color_eq(s_vault_readout_color, vc);
    if (readout_color_changed) {
        s_vault_readout_color = vc;
        s_vault_readout_color_valid = true;
        dirty_lo = 0;
        dirty_hi = VAULT_SLOT_COUNT - 1;
    }
    if (dirty_hi >= 0 && s_vault_readout != NULL) {
        lv_area_t dirty;
        vault_readout_area(dirty_lo, &dirty);
        lv_area_t last = { 0 };
        vault_readout_area(dirty_hi, &last);
        dirty.x1 -= 2;
        dirty.y1 -= 2;
        dirty.x2 = last.x2 + 2;
        dirty.y2 = last.y2 + 2;
        lv_obj_invalidate_area(s_vault_readout, &dirty);
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

#define HUD_ARC_WIDTH 15
#define HUD_ARC_RADIUS 225
#define HUD_TICK_INNER_RADIUS 217.0f
#define HUD_TICK_OUTER_RADIUS 233.0f
#define HUD_NOTCH_INNER_RADIUS 215.0f
#define HUD_NOTCH_OUTER_RADIUS 233.0f

static lv_color_t hud_face_color(const boost_theme_t *theme)
{
    return boost_theme_hud_true_black() ? lv_color_black() : c(theme->face);
}

/* Static face art. `cached` is set when painting into the background canvas at
 * build time: the clip early-out is meaningless there and everything must be
 * drawn once, in full.
 *
 * The centre is a parameter because the two callers work in different spaces.
 * Cached, it is the canvas's own centre and carries no burn-in offset â€” the
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

    /* Outer ring art (ticks, track, notch) lives at r >= 215. A digit update
     * dirties only the centre, so skip all of it in that common case. */
    if (!cached && !clip_reaches_radius(layer, cx, cy, 190.0f)) return;

    for (int i = 0; i <= 20; ++i) {
        const float v = s_psi_min + (s_psi_max - s_psi_min) * (float)i / 20.0f;
        const float rad = psi_to_sweep(v, HUD_A0, HUD_A1) * (float)M_PI / 180.0f;
        lv_draw_line_dsc_init(&ln);
        ln.color = (v >= s_psi_overboost) ? c(theme->overboost) : c(theme->vacuum);
        ln.width = (i % 2 == 0) ? 4 : 2;
        ln.opa = LV_OPA_COVER;
        ln.p1.x = cx + HUD_TICK_INNER_RADIUS * cosf(rad);
        ln.p1.y = cy + HUD_TICK_INNER_RADIUS * sinf(rad);
        ln.p2.x = cx + HUD_TICK_OUTER_RADIUS * cosf(rad);
        ln.p2.y = cy + HUD_TICK_OUTER_RADIUS * sinf(rad);
        lv_draw_line(layer, &ln);
    }

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.color = c(theme->track);
    arc.width = HUD_ARC_WIDTH;
    arc.start_angle = HUD_A0;
    arc.end_angle = HUD_A1;
    arc.center.x = icx;
    arc.center.y = icy;
    arc.radius = HUD_ARC_RADIUS;
    arc.opa = LV_OPA_COVER;
    lv_draw_arc(layer, &arc);

    const float zrad = psi_to_sweep(0.0f, HUD_A0, HUD_A1) * (float)M_PI / 180.0f;
    lv_draw_line_dsc_init(&ln);
    ln.color = c(theme->zero);
    ln.width = 5;
    ln.opa = LV_OPA_COVER;
    ln.round_start = true;
    ln.round_end = true;
    ln.p1.x = cx + HUD_NOTCH_INNER_RADIUS * cosf(zrad);
    ln.p1.y = cy + HUD_NOTCH_INNER_RADIUS * sinf(zrad);
    ln.p2.x = cx + HUD_NOTCH_OUTER_RADIUS * cosf(zrad);
    ln.p2.y = cy + HUD_NOTCH_OUTER_RADIUS * sinf(zrad);
    lv_draw_line(layer, &ln);
}

static void draw_hud_face(lv_event_t *e)
{
    paint_hud_face(lv_event_get_layer(e), active_theme(), false, px_cx(), px_cy());
}

/* One styleless readout object owns the complete draw order: chromatic ghosts
 * first, then the 96 px primary slots/sign. All sources are immutable for the
 * duration of the callback, so async software draw units never race label text
 * or image-source publication. */
static void draw_hud_readout(lv_event_t *e)
{
    if (s_hud_val_str[0] == '\0') return;
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t ghost_area = {
        px_icx() - 156,
        px_icy() + HUD_VALUE_Y - 39,
        px_icx() + 105,
        px_icy() + HUD_VALUE_Y + 39,
    };
    lv_area_t readout_area = {
        ghost_area.x1 - HUD_GLITCH_DX,
        ghost_area.y1,
        ghost_area.x2 + HUD_GLITCH_DX,
        ghost_area.y2,
    };
    const lv_area_t *clip = &layer->_clip_area;
    if (clip->x2 < readout_area.x1 || clip->x1 > readout_area.x2 ||
        clip->y2 < readout_area.y1 || clip->y1 > readout_area.y2) {
        return;
    }

    const lv_font_t *font = hud_readout_font(s_hud_readout_font.pixels != NULL);

    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.font = font;
    d.text = s_hud_val_str;
    d.align = LV_TEXT_ALIGN_RIGHT;
    d.text_local = 1;
    /* Ghost colours are pre-blended against the known true-black/dark face. */
    d.opa = LV_OPA_COVER;
    lv_area_t left = ghost_area;
    left.x1 -= HUD_GLITCH_DX;
    left.x2 -= HUD_GLITCH_DX;
    d.color = lv_color_mix(c(active_theme()->overboost), hud_face_color(active_theme()), LV_OPA_70);
    lv_draw_label(layer, &d, &left);
    lv_area_t right = ghost_area;
    right.x1 += HUD_GLITCH_DX;
    right.x2 += HUD_GLITCH_DX;
    d.color = lv_color_mix(c(active_theme()->vacuum), hud_face_color(active_theme()), LV_OPA_70);
    lv_draw_label(layer, &d, &right);

    /* Match the former label objects' fixed boxes and centred text exactly. */
    d.align = LV_TEXT_ALIGN_CENTER;
    d.color = s_hud_readout_color_valid ? s_hud_readout_color : c(active_theme()->boost);
    for (int i = 0; i < HUD_SLOT_COUNT; ++i) {
        d.text = s_hud_slot_text[i];
        lv_area_t a = {
            px_icx() + k_hud_slot_x[i] - 28,
            px_icy() + HUD_VALUE_Y - 35,
            px_icx() + k_hud_slot_x[i] + 27,
            px_icy() + HUD_VALUE_Y + 34,
        };
        lv_draw_label(layer, &d, &a);
    }
    d.text = s_hud_sign_text;
    lv_area_t sign = {
        px_icx() + s_hud_sign_x - 19,
        px_icy() + HUD_VALUE_Y - 35,
        px_icx() + s_hud_sign_x + 18,
        px_icy() + HUD_VALUE_Y + 34,
    };
    lv_draw_label(layer, &d, &sign);
}

static void invalidate_hud_readout(lv_area_t *area)
{
    if (s_hud_readout != NULL && area != NULL) lv_obj_invalidate_area(s_hud_readout, area);
}

static void invalidate_hud_readout_full(void)
{
    lv_area_t area = {
        px_icx() - 156,
        px_icy() + HUD_VALUE_Y - 42,
        px_icx() + 105,
        px_icy() + HUD_VALUE_Y + 42,
    };
    invalidate_hud_readout(&area);
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
    const float color_psi = s_hud_fill_valid ? s_hud_fill_color_psi : 0.0f;
    if (boost_theme_hud_gradient()) arc.color = gradient_color_for_psi(theme, color_psi);
    else if (color_psi >= s_psi_overboost) arc.color = c(theme->overboost);
    else if (color_psi < 0.0f) arc.color = c(theme->vacuum);
    else arc.color = c(theme->boost);
    arc.width = HUD_ARC_WIDTH;
    arc.start_angle = lo;
    arc.end_angle = hi;
    arc.center.x = px_icx();
    arc.center.y = px_icy();
    arc.radius = HUD_ARC_RADIUS;
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
        /* The live fill arc is drawn with flat ends (draw_hud_fill() never sets
         * arc.rounded), so a rounded invalidation box over-covers by
         * HUD_ARC_WIDTH / 2 + 1 px on every edge on every tick. Request the
         * exact flat stroke box instead and add a small AA margin: the software
         * arc rasteriser's coverage extends ~2 px beyond the nominal stroke
         * bbox (measured: 1 px strands the inner-edge AA, 3 px is clean). */
        lv_draw_arc_get_area(px_icx(), px_icy(), HUD_ARC_RADIUS, seg, seg_end,
                             HUD_ARC_WIDTH, false, &area);
        area.x1 -= 3; area.y1 -= 3; area.x2 += 3; area.y2 += 3;
        lv_obj_invalidate_area(s_hud_fill, &area);
        seg = seg_end;
    }
}

static void build_hud(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();
    const bool readout_cached = build_hud_readout_font();

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
        bg.bg_color = hud_face_color(theme);
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

    /* One styleless object owns ghosts and primary digits. It is created after
     * the fill and before the lower labels, with ghosts drawn first inside the
     * callback and the primary readout second. */
    s_hud_readout = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hud_readout);
    /* Unlike the old full-screen draw object, this bounds object traversal and
     * invalidation to the actual ghosts/primary glyphs. Coordinates are parent
     * relative; draw_hud_readout() remains absolute and keeps px offsets. */
    lv_obj_set_size(s_hud_readout, HUD_READOUT_OBJ_W, HUD_READOUT_OBJ_H);
    lv_obj_set_pos(s_hud_readout, DISP_SIZE / 2 + HUD_READOUT_OBJ_X1,
                   DISP_SIZE / 2 + HUD_READOUT_OBJ_Y1);
    lv_obj_clear_flag(s_hud_readout, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hud_readout, draw_hud_readout, LV_EVENT_DRAW_MAIN, NULL);
    memset(s_hud_slot_text, 0, sizeof(s_hud_slot_text));
    s_hud_slot_text[HUD_SLOT_ONES][0] = '0';
    s_hud_slot_text[HUD_SLOT_DOT][0] = '.';
    memset(s_hud_sign_text, 0, sizeof(s_hud_sign_text));
    s_hud_sign_x = HUD_SIGN_ONES_X;
    s_hud_readout_color = c(theme->boost);
    s_hud_readout_color_valid = true;
    (void)readout_cached;

    /* The remaining labels are not part of the primary 96 px readout. */


    lv_obj_t *unit = lv_label_create(scr);
    lv_label_set_text(unit, "PSI // FORCED INDUCTION");
    lv_obj_set_style_text_font(unit, F_COND18, 0);
    lv_obj_set_style_text_color(unit, c(theme->muted), 0);
    /* Clear of the lower reticle brackets, which end at +52. */
    lv_obj_align(unit, LV_ALIGN_CENTER, 0, 84);

    s_hud_map = lv_label_create(scr);
    /* Measured atmospheric baseline from the BMP280, never a synthesised value.
     * Built in the unknown state; update_hud() fills it once a fresh reading
     * exists. See HUD_BMP_FRESH_MS. */
    lv_label_set_text(s_hud_map, "ATM --kPa");
    lv_obj_set_style_text_font(s_hud_map, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_map, c(theme->vacuum), 0);
    lv_obj_align(s_hud_map, LV_ALIGN_CENTER, -100, 128);

    s_hud_pk = lv_label_create(scr);
    lv_label_set_text(s_hud_pk, "PK 0.0");
    lv_obj_set_style_text_font(s_hud_pk, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_pk, c(theme->vacuum), 0);
    lv_obj_align(s_hud_pk, LV_ALIGN_CENTER, 100, 128);

    /* Sample-source indicator. "SYS LIVE" and "SYS DEMO" are the same length, so
     * this centred label never changes width and cannot strand pixels. */
    s_hud_sys = lv_label_create(scr);
    lv_label_set_text(s_hud_sys, "SYS LIVE");
    lv_obj_set_style_text_font(s_hud_sys, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_sys, c(theme->muted), 0);
    lv_obj_align(s_hud_sys, LV_ALIGN_CENTER, -100, 152);

    /* Static counterpart to the web mirror's NC-2077 tag, on the PK column. */
    s_hud_tag = lv_label_create(scr);
    lv_label_set_text(s_hud_tag, "NC-2077");
    lv_obj_set_style_text_font(s_hud_tag, F_MONO16, 0);
    lv_obj_set_style_text_color(s_hud_tag, c(theme->muted), 0);
    lv_obj_align(s_hud_tag, LV_ALIGN_CENTER, 100, 152);

}

static void update_hud(const boost_sample_t *sample, const boost_theme_t *theme)
{
    const float visual_psi = update_visual_arc_animation(sample->psi);
    if (!s_hud_fill_valid) {
        const float zero_a = psi_to_sweep(0.0f, HUD_A0, HUD_A1);
        s_hud_fill_deg = psi_to_sweep(visual_psi, HUD_A0, HUD_A1);
        s_hud_fill_psi = visual_psi;
        s_hud_fill_color_psi = isfinite(sample->psi) ? sample->psi : 0.0f;
        s_hud_fill_valid = true;
        /* The scene-build repaint can finish before this first sample. Request
         * the complete zero-to-value span; otherwise later endpoint-only dirty
         * regions clip the full arc into a moving blob until zero is crossed. */
        invalidate_hud_fill(zero_a, s_hud_fill_deg);
    }
    const float old_a = s_hud_fill_deg;
    const float new_a = psi_to_sweep(visual_psi, HUD_A0, HUD_A1);
    const float zero_a = psi_to_sweep(0.0f, HUD_A0, HUD_A1);
    /* In gradient mode the whole fill recolours whenever the quantised step
     * changes, not only at the vacuum/boost/overboost boundaries. */
    const float raw_color_psi = isfinite(sample->psi) ? sample->psi : 0.0f;
    const bool grad_flip = boost_theme_hud_gradient() &&
        !lv_color_eq(gradient_color_for_psi(theme, s_hud_fill_color_psi),
                     gradient_color_for_psi(theme, raw_color_psi));
    const bool zone_flip = grad_flip ||
                           (s_hud_fill_color_psi < 0.0f) != (raw_color_psi < 0.0f) ||
                           (s_hud_fill_color_psi >= s_psi_overboost) != (raw_color_psi >= s_psi_overboost);
    if (zone_flip) {
        const bool side_flip = (old_a < zero_a) != (new_a < zero_a);
        s_hud_fill_deg = new_a;
        s_hud_fill_psi = visual_psi;
        s_hud_fill_color_psi = raw_color_psi;
        if (side_flip) {
            /* Opposite sides of the zero gap are disjoint. */
            invalidate_hud_fill(fminf(old_a, zero_a), fmaxf(old_a, zero_a));
            invalidate_hud_fill(fminf(new_a, zero_a), fmaxf(new_a, zero_a));
        } else {
            /* Same-side full spans overlap from zero to the endpoint. */
            invalidate_hud_fill(fminf(fminf(old_a, new_a), zero_a),
                                fmaxf(fmaxf(old_a, new_a), zero_a));
        }
    } else if (new_a != old_a) {
        s_hud_fill_deg = new_a;
        s_hud_fill_psi = visual_psi;
        /* Otherwise only the wedge between the old and new ends changed â€”
         * repainting the full arc every frame is what was costing cadence. */
        invalidate_hud_fill(old_a, new_a);
    }
    /* Commit the raw color source even when its current bucket did not change;
     * the next bucket/zone transition must compare against the immediately
     * previous raw sample, never against delayed geometry. */
    s_hud_fill_color_psi = raw_color_psi;

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
    /* Track which slots actually moved so the common tenths-only update keeps
     * both the primary and ghost dirty union narrow. */
    int dirty_lo = HUD_SLOT_COUNT;
    int dirty_hi = -1;
    for (int i = 0; i < HUD_SLOT_COUNT; ++i) {
        bool changed = strcmp(s_hud_slot_text[i], slot_txt[i]) != 0;
        if (changed) {
            memcpy(s_hud_slot_text[i], slot_txt[i], sizeof(s_hud_slot_text[i]));
            if (i < dirty_lo) dirty_lo = i;
            if (i > dirty_hi) dirty_hi = i;
        }
    }
    bool readout_color_changed = false;
    if (!s_hud_readout_color_valid || !lv_color_eq(s_hud_readout_color, vc)) {
        s_hud_readout_color = vc;
        s_hud_readout_color_valid = true;
        readout_color_changed = true;
        if (dirty_lo > 0) dirty_lo = 0;
        if (dirty_hi < HUD_SLOT_COUNT - 1) dirty_hi = HUD_SLOT_COUNT - 1;
    }
    /* Keep the flat string used by both chromatic ghost passes. */
    char prev_val[sizeof(s_hud_val_str)];
    snprintf(prev_val, sizeof(prev_val), "%s", s_hud_val_str);
    snprintf(s_hud_val_str, sizeof(s_hud_val_str), "%s%d.%d",
             sample->psi < -0.05f ? "-" : "", whole, tenths_total % 10);
    const bool value_changed = strcmp(prev_val, s_hud_val_str) != 0;
    /* Sign is resolved before the ghost invalidation so a sign flip or a slide
     * between the ones/tens anchors can widen the dirty box. */
    const char *sign = sample->psi < -0.05f ? "-" : "";
    bool sign_changed = false;
    if (strcmp(s_hud_sign_text, sign) != 0) {
        snprintf(s_hud_sign_text, sizeof(s_hud_sign_text), "%s", sign);
        sign_changed = true;
    }
    const int sign_x = has_tens ? HUD_SIGN_TENS_X : HUD_SIGN_ONES_X;
    if (sign_x != s_hud_sign_x) {
        s_hud_sign_x = sign_x;
        sign_changed = true;
    }

    if ((value_changed || readout_color_changed) && s_hud_readout != NULL) {
        /* Preserve the old exact dirty union: changed primary slots/sign, grown
         * by the shared ghost offset and AA margin. */
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
        invalidate_hud_readout(&ga);
    } else if (sign_changed) {
        invalidate_hud_readout_full();
    }

    char buf[24];
    /* Atmospheric baseline, reported only when the BMP280 actually measured it.
     * ambient_is_fallback marks the 101.325 kPa standard-atmosphere constant,
     * which must never be shown as a reading; bmp_age_ms guards a sensor that
     * answered at boot and has since gone quiet (UINT32_MAX = never read). In
     * demo mode the sim leaves all three zero/false, so bmp_present alone keeps
     * the sweep from rendering "ATM 0kPa". */
    const bool atm_fresh = sample->bmp_present && !sample->ambient_is_fallback &&
                           sample->bmp_age_ms <= HUD_BMP_FRESH_MS;
    if (atm_fresh) {
        snprintf(buf, sizeof(buf), "ATM %dkPa", (int)lroundf(sample->ambient_kpa));
    } else {
        snprintf(buf, sizeof(buf), "ATM --kPa");
    }
    if (strcmp(lv_label_get_text(s_hud_map), buf) != 0) lv_label_set_text(s_hud_map, buf);
    snprintf(buf, sizeof(buf), "PK %.1f", (double)s_peak_psi);
    if (strcmp(lv_label_get_text(s_hud_pk), buf) != 0) lv_label_set_text(s_hud_pk, buf);
    /* This one is a positive status indicator, not a demo watermark: LIVE means
     * the reading came from the MAP sensor, DEMO means the synthetic sweep. */
    const char *sys = sample->demo ? "SYS DEMO" : "SYS LIVE";
    if (strcmp(lv_label_get_text(s_hud_sys), sys) != 0) lv_label_set_text(s_hud_sys, sys);
}

/* ========================================================================== */
/*  Style: bigdigit  (Alvida numeral on a color-sweeping ground)              */
/* ========================================================================== */

/* Positive gradient step count is defined near the shared helper above. */

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
    if (!isfinite(psi) || psi <= 0.0f || !(s_psi_max > 0.0f)) return 0;
    const float positive = clampf(psi, 0.0f, s_psi_max);
    int step = (int)ceilf((positive / s_psi_max) * (float)BIG_POSITIVE_STEPS);
    if (step < 1) step = 1;
    if (step > BIG_POSITIVE_STEPS) step = BIG_POSITIVE_STEPS;
    return step;
}

static uint32_t big_color_uncached(const boost_theme_t *theme, int step)
{
    if (step <= 0 || !(s_psi_max > 0.0f)) return theme->vacuum;
    if (step > BIG_POSITIVE_STEPS) step = BIG_POSITIVE_STEPS;
    const float psi = s_psi_max * (float)step / (float)BIG_POSITIVE_STEPS;
    /* The red ramp used to be squeezed into overboost..max (about 2 psi), so it
     * snapped. Start blending earlier so the approach is gradual. */
    const float red_start = s_psi_overboost * 0.55f;
    if (psi <= red_start) {
        return lerp_rgb(theme->vacuum, theme->boost, psi / fmaxf(0.001f, red_start));
    }
    const float span = fmaxf(0.001f, s_psi_max - red_start);
    return lerp_rgb(theme->boost, theme->overboost, (psi - red_start) / span);
}

static lv_color_t gradient_lut_color(const boost_theme_t *theme, int step)
{
    static lv_color_t lut[BIG_POSITIVE_STEPS + 1];
    static uint32_t vacuum, boost, overboost;
    static float psi_max, psi_overboost;
    static bool valid;
    if (!valid || vacuum != theme->vacuum || boost != theme->boost ||
        overboost != theme->overboost || psi_max != s_psi_max ||
        psi_overboost != s_psi_overboost) {
        vacuum = theme->vacuum;
        boost = theme->boost;
        overboost = theme->overboost;
        psi_max = s_psi_max;
        psi_overboost = s_psi_overboost;
        for (int i = 0; i <= BIG_POSITIVE_STEPS; ++i) {
            /* Cache the panel-effective value so equality checks and drawing use
             * the exact same RGB565 result. */
            lut[i] = c(big_color_uncached(theme, i));
        }
        valid = true;
    }
    if (step < 0) step = 0;
    if (step > BIG_POSITIVE_STEPS) step = BIG_POSITIVE_STEPS;
    return lut[step];
}

static uint32_t big_color_for_step(const boost_theme_t *theme, int step)
{
    return lv_color_to_u32(gradient_lut_color(theme, step));
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
    lv_obj_set_style_bg_color(s_scene_parent != NULL ? s_scene_parent : lv_screen_active(), c(ground), 0);
    s_big_bg_step = -1;

    /* Recolouring the ground in one go dirties all 217k pixels at once: a ~69 ms
     * stall that reads as a lurch every colour step. The work is unavoidable â€”
     * a flat fill is already the cheapest primitive there is, so there is
     * nothing to pre-render â€” but it does not have to land in a single cycle.
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
            const uint32_t next_color = big_color_for_step(theme, step);
            /* Custom palettes can collapse adjacent logical buckets to the same
             * RGB565 value. Do not restart a full-screen wipe in that case. */
            if (!lv_color_eq(c(s_big_band_color), c(next_color))) {
                s_big_band_color = next_color;
                s_big_band_next = 0;
            }
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
                lv_obj_set_style_bg_color(s_scene_parent != NULL ? s_scene_parent : lv_screen_active(), c(s_big_band_color), 0);
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
            const uint32_t next_color = big_color_for_step(theme, tstep);
            const lv_color_t tc = c(next_color);
            if (!lv_color_eq(c(s_big_text_color), tc)) {
                s_big_text_color = next_color;
                lv_obj_t *const slots[4] = { s_big_tens, s_big_ones, s_big_dot, s_big_tenths };
                for (int i = 0; i < 4; ++i) {
                    if (slots[i] != NULL &&
                        !lv_color_eq(lv_obj_get_style_text_color(slots[i], 0), tc)) {
                        lv_obj_set_style_text_color(slots[i], tc, 0);
                    }
                }
                /* The sign is drawn, not a label, so it needs an explicit repaint. */
                if (s_big_minus != NULL) lv_obj_invalidate(s_big_minus);
            }
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
    /* A scene rebuild/style switch is a visual discontinuity: discard filter
     * history so the next sample seeds the new arc exactly once. */
    reset_visual_arc_animation(s_display_psi);
    s_arc_drawn_psi = isfinite(s_display_psi) ? s_display_psi : 0.0f;
    s_arc_color_psi = s_arc_drawn_psi;

    /* Draw tasks retain object styles and font bitmap pointers. Drain both
     * software draw units before deleting the objects or their PSRAM caches. */
    lv_draw_wait_for_finish();
    lv_obj_t *scr = s_scene_parent != NULL ? s_scene_parent : lv_screen_active();
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
    s_arc_value_canvas = NULL;
    s_zero_notch = NULL;
    s_value_sign_label = s_value_tens_label = s_value_ones_label = NULL;
    s_value_decimal_label = s_value_tenths_label = NULL;
    s_peak_label = s_mode_label = s_zone_label = NULL;

    if (s_arc_bg_buf != NULL) {
        BG_FREE(s_arc_bg_buf);
        s_arc_bg_buf = NULL;
    }
    s_arc_bg = NULL;

    /* s_vault_bg_buf is a memoized static face. Keep it across scene switches;
     * build_vault() repaints it when any static-art input changes. */
    s_vault_bg = s_vault_peak_mark = s_vault_crt = NULL;
    s_vault_needle = s_vault_window = s_vault_readout = NULL;
    s_vault_peak = s_vault_alert = s_vault_alert_marks = NULL;
    memset(s_vault_slot_text, 0, sizeof(s_vault_slot_text));
    s_vault_readout_color_valid = false;

    if (s_hud_bg_buf != NULL) {
        BG_FREE(s_hud_bg_buf);
        s_hud_bg_buf = NULL;
    }
    s_hud_bg = NULL;
    s_hud_face = s_hud_fill = NULL;
    s_hud_readout = NULL;
    memset(s_hud_slot_text, 0, sizeof(s_hud_slot_text));
    memset(s_hud_sign_text, 0, sizeof(s_hud_sign_text));
    s_hud_readout_color_valid = false;
    s_hud_val_str[0] = '\0';
    s_hud_fill_valid = false;
    s_hud_fill_deg = 0.0f;
    s_hud_fill_psi = 0.0f;
    s_hud_fill_color_psi = 0.0f;
    s_hud_sign_x = HUD_SIGN_ONES_X;
    s_hud_map = s_hud_pk = s_hud_sys = s_hud_tag = NULL;
    destroy_hud_readout_font();

    s_big_bg = s_big_minus = s_big_tens = s_big_ones = NULL;
    s_big_dot = s_big_tenths = s_big_unit = s_big_zone = s_big_peak = NULL;
    /* s_neon_bg_buf and the baked sprite tiles are memoized static art, kept
     * across scene switches for the same reason s_vault_bg_buf is: rebuilding
     * them made every return to neon pause. Measured on the board before this,
     * switching to neon cost ~350 ms against 45-100 ms for the other themes.
     * build_neon() repaints/rebakes when an input actually changes - the
     * background against neon_bg_key_t, the glyph tiles against the layout. */
    s_neon_bg = NULL;
    s_neon_face = s_neon_zone = s_neon_unit = s_neon_peak = NULL;
    s_neon_word_drawn = -1;
    s_neon_peak_idx = -1;
    s_neon_peak_in_run = false;
    s_neon_tube_peak_vis = false;
    s_neon_tube_peak_angle = NAN;
    s_big_bg_step = -1;
    s_big_text_step = -1;
    for (int k = 0; k < BIG_BANDS; ++k) s_big_band[k] = NULL;
    s_big_band_next = BIG_BANDS;
}

static void build_scene(boost_gauge_style_t style)
{
    const boost_theme_t *theme = active_theme();
    lv_obj_t *scr = s_scene_parent != NULL ? s_scene_parent : lv_screen_active();

    /* The screen's own opaque background is the face in legacy mode. A page
     * root owns the face when the coordinator is active, so page chrome can
     * remain separate from the renderer. */
    lv_obj_set_style_bg_color(scr,
                              style == BOOST_STYLE_HUD ? hud_face_color(theme) : c(theme->face),
                              0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* One styleless, screen-sized container that every style builds into, so
     * the burn-in shift is a single lv_obj_set_pos() instead of a walk over
     * every object â€” and, more importantly, so the cached PSRAM faces move as
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
        case BOOST_STYLE_NEON:     build_neon(s_root); break;
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

void boost_gauge_create_in(lv_obj_t *parent)
{
    if (parent == NULL) return;
    s_scene_parent = parent;
    load_range_from_config();
    const boost_theme_t *theme = active_theme();
    snprintf(s_theme_id, sizeof(s_theme_id), "%s", theme->id);

    s_display_psi = 0.0f;
    s_peak_psi = 0.0f;
    reset_visual_arc_animation(s_display_psi);
    s_arc_drawn_psi = s_display_psi;
    s_arc_color_psi = s_display_psi;
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

void boost_gauge_reset_peak(void)
{
    if (!s_ui_ready || boost_gauge_media_active()) return;
    reset_peak_ui();
}

bool boost_gauge_media_active(void)
{
#if LV_USE_GIF
    return s_media_gif != NULL;
#else
    return false;
#endif
}

void boost_gauge_apply_config(void)
{
    if (!s_ui_ready) return;
    load_range_from_config();
    /* Range and zero-angle move every style's static art â€” ticks, notch,
     * numerals â€” including the arc face's cached PSRAM background now that it
     * has one. Rebuild rather than patch cached pixels in place; this is the
     * same path vault/hud/bigdigit already take here. */
    destroy_scene();
    build_scene(s_built_style);
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
    if (s_neon_face != NULL) {
        neon_build_seg_boxes();
        neon_build_tube_boxes();
    }
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
     * a second and a half â€” not merely that this one tick was quiet, which a
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

    boost_display_gauge_update_begin();

    /* Before the per-style update: those compute dirty areas from px_cx()/
     * px_cy(), and an offset that changed between the invalidation and the
     * draw is precisely how stale pixels get stranded. */
    pixel_shift_tick(sample->psi);

    switch (s_built_style) {
        case BOOST_STYLE_VAULT:    update_vault(sample, theme); break;
        case BOOST_STYLE_HUD:      update_hud(sample, theme); break;
        case BOOST_STYLE_BIGDIGIT: update_bigdigit(sample, theme); break;
        case BOOST_STYLE_NEON:     update_neon(sample, theme); break;
        case BOOST_STYLE_ARC:
        default:                   update_arc(sample, theme); break;
    }

    boost_display_gauge_update_end();

    s_display_psi = sample->psi;
}

#ifndef ESP_PLATFORM
float boost_gauge_host_vault_needle_deg(void)
{
    return s_vault_needle_deg;
}
#endif
