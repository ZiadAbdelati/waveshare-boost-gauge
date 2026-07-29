#pragma once
#include <stdbool.h>
#include <stdint.h>

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

/** Rebuild ticks/arc mapping from live PSI config. Must be called under lock. */
void boost_gauge_apply_config(void);

/** Synchronously load the committed mapped GIF under the display lock. */
bool boost_gauge_media_load(void);

/** Destroy the GIF before releasing its raw-partition mapping. */
void boost_gauge_media_delete(void);

#ifndef ESP_PLATFORM
/** Host regression hook for checking scene-build needle state. */
float boost_gauge_host_vault_needle_deg(void);
#endif

#ifdef __cplusplus
}
#endif
