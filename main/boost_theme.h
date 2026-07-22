#pragma once

#include <stdint.h>
#include <stddef.h>

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

const boost_theme_t *boost_theme_default(void);
const boost_theme_t *boost_theme_find(const char *id);
const boost_theme_t *boost_theme_at(size_t index);
size_t boost_theme_count(void);

#ifdef __cplusplus
}
#endif
