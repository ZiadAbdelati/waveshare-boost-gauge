#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** High cabin brightness (percent). */
#define BOOST_BRIGHTNESS_MAX 100
/** Night / dim cabin brightness (percent). Still readable on AMOLED. */
#define BOOST_BRIGHTNESS_MIN 12

void boost_brightness_init(int initial_percent);
void boost_brightness_set(int percent);
int boost_brightness_get(void);
bool boost_brightness_is_max(void);

/** Toggle between BOOST_BRIGHTNESS_MAX and BOOST_BRIGHTNESS_MIN. */
void boost_brightness_toggle_max_min(void);

#ifdef __cplusplus
}
#endif
