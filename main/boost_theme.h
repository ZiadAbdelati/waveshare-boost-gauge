#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOST_THEME_ID_MAX 24

/* A theme selects a distinct gauge layout (not just a palette). Keep these in
 * sync with the web renderers in web/app.js (activeThemeStyle dispatch) and the
 * `style` strings served by the /themes API. */
typedef enum {
    BOOST_STYLE_ARC = 0,   /* dual-climate arc face (Dyno Cell) */
    BOOST_STYLE_VAULT,     /* phosphor needle dial (Vault-Tec) */
    BOOST_STYLE_HUD,       /* cyberpunk targeting HUD (Night City) */
    BOOST_STYLE_BIGDIGIT,  /* huge Alvida numeral on a color-sweep ground */
} boost_gauge_style_t;

typedef struct {
    char id[BOOST_THEME_ID_MAX];
    const char *name;
    boost_gauge_style_t style;
    uint32_t face;
    uint32_t track;
    uint32_t text;
    uint32_t muted;
    uint32_t vacuum;
    uint32_t boost;
    uint32_t overboost;
    uint32_t zero;
    /* Deliberately no brightness here. Brightness is a display setting owned by
     * the user and the dim schedule; themes carried their own pair and silently
     * overwrote the configured value on every theme switch. A theme changes how
     * the gauge looks, never how bright the panel is. */
} boost_theme_t;

/* Stable lowercase token for the JSON API / web dispatch, e.g. "arc". */
const char *boost_style_name(boost_gauge_style_t style);

/**
 * Load persisted colour overrides. Call once at start-up, before the first
 * boost_theme_find(); until then the built-in palettes are served.
 */
void boost_theme_init(void);

const boost_theme_t *boost_theme_default(void);
const boost_theme_t *boost_theme_find(const char *id);
const boost_theme_t *boost_theme_at(size_t index);
size_t boost_theme_count(void);

/*
 * The three zone colours are user-editable; face/track/text/muted/zero are
 * structural and stay fixed, because a theme whose face and text can be set
 * independently is a theme that can be made unreadable.
 */
typedef struct {
    uint32_t vacuum;
    uint32_t boost;
    uint32_t overboost;
} boost_theme_colors_t;

/** Overwrite a theme's zone colours and persist. Returns false on unknown id. */
bool boost_theme_set_colors(const char *id, const boost_theme_colors_t *colors);

/** Restore a theme's built-in palette and persist. False on unknown id. */
bool boost_theme_reset_colors(const char *id);

/** True when a theme currently differs from its built-in palette. */
bool boost_theme_is_customized(const char *id);

/*
 * Big Digit normally sweeps its whole ground through the zone colours. With a
 * static ground it never repaints full-screen at all, which removes this face's
 * only stall — see the BIG_BANDS discussion in README.md.
 */
bool boost_theme_bigdigit_static_bg(void);
void boost_theme_set_bigdigit_static_bg(bool enabled);

/*
 * Sweep the READOUT through the zone colours instead of the ground. The digits
 * occupy roughly 31k px against the ground's 217k, so a colour step repaints
 * about a seventh as much - the cheap way to keep the colour cue.
 */
bool boost_theme_bigdigit_color_text(void);
void boost_theme_set_bigdigit_color_text(bool enabled);

/*
 * Ground colour for Big Digit's static-background mode. Defaults to true black
 * (0x000000) so the AMOLED pixels are actually OFF - the theme's `face` is a
 * dark grey, which on an emissive panel lit the whole field like a backlit LCD.
 * Only consulted when bigdigit_static_bg is on.
 */
uint32_t boost_theme_bigdigit_static_color(void);
void boost_theme_set_bigdigit_static_color(uint32_t rgb);

/* Readout text colour for Big Digit, default white. Overridden while
 * bigdigit_color_text is on (the readout sweeps the zone colours then). */
uint32_t boost_theme_bigdigit_text_color(void);
void boost_theme_set_bigdigit_text_color(uint32_t rgb);

/* Smooth (quantised-gradient) fill colour for the arc and hud faces instead of
 * a hard vacuum/boost/overboost switch - the same ramp Big Digit sweeps. */
bool boost_theme_arc_gradient(void);
void boost_theme_set_arc_gradient(bool enabled);
bool boost_theme_hud_gradient(void);
void boost_theme_set_hud_gradient(bool enabled);

/* Tearing-effect sync preference, persisted. Applied to the display at boot and
 * whenever changed. */
bool boost_theme_te_sync(void);
void boost_theme_set_te_sync(bool enabled);

/*
 * Region double-buffering preference, persisted. Default OFF, like te_sync.
 * When on, one render cycle's dirty strips are rasterised into a PSRAM
 * staging canvas instead of being transferred immediately, then pushed to
 * the panel back-to-back after a single TE wait - see boost_display.c for
 * the measured feasibility numbers and the mechanism itself. Applied to the
 * display at boot and whenever changed, same as te_sync.
 */
bool boost_theme_region_dbuf(void);
void boost_theme_set_region_dbuf(bool enabled);

/**
 * Persisted panel rotation in degrees: **0, 90, 180 or 270 only**.
 *
 * Not an arbitrary angle. The LVGL adapter maps rotation onto the panel's scan
 * order, which is why quarter turns are free; any other angle would need a
 * full-frame affine resample every render, on a CPU-rasterised partial pipeline
 * with no 2D accelerator. The ledger already records a single full-screen
 * repaint costing ~45 ms, so an arbitrary angle would not hold 60 FPS.
 *
 * The adapter consumes rotation when the display is registered, so a change
 * takes effect on the next boot. set_rotation() ignores any other value rather
 * than snapping, so the API can report the rejection.
 */
uint16_t boost_theme_rotation(void);
void boost_theme_set_rotation(uint16_t degrees);

/*
 * Demo mode. OFF (the default) reads the real ADS1115/BMP280 sensors and shows
 * no DEMO indicator; ON runs the synthetic sweep and shows DEMO on every face.
 * Persisted so a panel keeps whichever source it was left on across reboots.
 * Default is deliberately OFF so a normal boot goes straight to the sensors.
 */
bool boost_theme_demo_mode(void);
void boost_theme_set_demo_mode(bool enabled);

/* Vault-Tec dial glow: the face colour the dial fills with and the vignette
 * darkens from, plus the vignette depth as a percentage (0 = flat, higher =
 * darker edges). Both re-bake the cached face on change. Defaults are the
 * dialled-in #05281a / 60%. */
uint32_t boost_theme_vault_face(void);
void boost_theme_set_vault_face(uint32_t rgb);
uint8_t boost_theme_vault_vignette_pct(void);
void boost_theme_set_vault_vignette_pct(uint8_t pct);
bool boost_theme_vault_needle_red(void);
void boost_theme_set_vault_needle_red(bool enabled);

/*
 * AMOLED burn-in countermeasure. The gauge shows one static-heavy face for
 * hours at a time at 85-92% brightness, so the tick rings, titles and readout
 * outlines sit on the same emitters long enough to age them differentially.
 * With this on, the whole scene is nudged by a pixel or two every so often so
 * those edges are smeared across several columns and rows instead.
 *
 * Defaults ON: the cost is one full-screen repaint every couple of minutes,
 * taken only when the reading is steady, and the damage it prevents is
 * permanent. Off is for anyone who would rather have the last few pixels of
 * alignment stability, or who is bench-testing frame timing.
 */
bool boost_theme_pixel_shift(void);
void boost_theme_set_pixel_shift(bool enabled);

/*
 * How long the scene rests at one offset before stepping to the next, in
 * seconds. The exact value barely matters for burn-in — anything far quicker
 * than emitter aging works — so this is a comfort control: how often you are
 * willing to spend a ~45 ms full-screen repaint. The dashboard offers 90 s /
 * 3 min / 10 min; the range below is wider so an API client is not boxed in by
 * the picker's choices.
 *
 * 30 s floor: below that the repaints stop being a rounding error against the
 * frame budget. 1 h ceiling: the eight-step ring would take most of a day to
 * close, which is slow enough that a long static session still ages one offset
 * disproportionately, and there is no point pretending otherwise.
 *
 * Default 90 s is the value this shipped with, so a panel that predates the
 * setting keeps exactly the cadence it was tuned and screenshotted at.
 */
#define BOOST_PXSHIFT_SEC_MIN     30u
#define BOOST_PXSHIFT_SEC_MAX     3600u
#define BOOST_PXSHIFT_SEC_DEFAULT 90u

uint16_t boost_theme_pixel_shift_sec(void);

/** Clamped to [BOOST_PXSHIFT_SEC_MIN, BOOST_PXSHIFT_SEC_MAX], then persisted. */
void boost_theme_set_pixel_shift_sec(uint16_t seconds);

#ifdef __cplusplus
}
#endif
