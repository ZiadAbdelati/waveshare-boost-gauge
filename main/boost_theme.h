#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOST_THEME_ID_MAX 24

typedef struct {
    char id[BOOST_THEME_ID_MAX];
    const char *name;
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

const boost_theme_t *boost_theme_default(void);
const boost_theme_t *boost_theme_find(const char *id);
const boost_theme_t *boost_theme_at(size_t index);
size_t boost_theme_count(void);

#ifdef __cplusplus
}
#endif
