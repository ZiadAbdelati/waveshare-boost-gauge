#pragma once

#include <stdint.h>

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the full-screen boost gauge on the active LVGL screen. */
void boost_gauge_create(void);

/** Push a new sample into the UI. Safe from any task; applied on LVGL timer. */
void boost_gauge_update(const boost_sample_t *sample);

/** Theme face: 0=night black, 1=ghost gray. Safe from any task. */
void boost_gauge_set_theme(uint8_t theme_id);

/** Reset peak hold (also available via short tap). Safe from any task. */
void boost_gauge_reset_peak(void);

#ifdef __cplusplus
}
#endif
