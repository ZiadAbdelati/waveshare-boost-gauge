#pragma once

#include "boost_sim.h"
#include "boost_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Build the full-screen boost gauge on the active LVGL screen. */
void boost_gauge_create(void);

/** Push a new sample into the UI. Must be called under bsp_display_lock. */
void boost_gauge_update(const boost_sample_t *sample);

/** Re-apply colors from the active runtime theme. Must be called under lock. */
void boost_gauge_apply_theme(const boost_theme_t *theme);

#ifdef __cplusplus
}
#endif
