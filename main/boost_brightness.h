#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fallback levels, used only until boost_brightness_set_levels() supplies the
 * configured pair. These are not the toggle targets: long-press follows the
 * user's own high/low settings so the dim step matches the dim schedule. */
#define BOOST_BRIGHTNESS_MAX 100
#define BOOST_BRIGHTNESS_MIN 12

void boost_brightness_init(int initial_percent);
void boost_brightness_set(int percent);
int boost_brightness_get(void);
bool boost_brightness_is_max(void);

/**
 * Supply the configured high/low pair that long-press toggles between. Called
 * whenever the config is loaded or changed, so the dim step always matches the
 * `brightnessLow` the dim schedule uses. Values are clamped to 0-100 and the
 * low level is held below the high one so the toggle cannot collapse.
 */
void boost_brightness_set_levels(int high, int low);

/** Toggle between the configured high and low brightness levels. */
void boost_brightness_toggle_max_min(void);

#ifdef __cplusplus
}
#endif
