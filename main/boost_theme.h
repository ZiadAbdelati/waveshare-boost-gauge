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
    int brightness_high;
    int brightness_low;
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

#ifdef __cplusplus
}
#endif
