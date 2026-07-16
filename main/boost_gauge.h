#pragma once

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the full-screen boost gauge on the active LVGL screen. */
void boost_gauge_create(void);

/** Push a new sample into the UI. Must be called under bsp_display_lock. */
void boost_gauge_update(const boost_sample_t *sample);

#ifdef __cplusplus
}
#endif
