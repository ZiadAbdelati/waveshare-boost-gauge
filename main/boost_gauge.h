#pragma once

#include <stdint.h>

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the full-screen boost gauge on the active LVGL screen. */
void boost_gauge_create(void);

/** Push a new sample into the UI. Must be called under bsp_display_lock. */
void boost_gauge_update(const boost_sample_t *sample);

/** Theme face: 0=night black, 1=ghost gray. Call under display lock. */
void boost_gauge_set_theme(uint8_t theme_id);

/** Reset peak hold (also available via short tap). */
void boost_gauge_reset_peak(void);

/** Apply any pending theme set while the LVGL lock was busy. */
void boost_gauge_service(void);

#ifdef __cplusplus
}
#endif
