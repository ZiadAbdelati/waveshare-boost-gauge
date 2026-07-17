#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

void boost_gauge_create(void);
/** Call under bsp_display_lock. */
void boost_gauge_update(const boost_sample_t *sample);
/** Call under bsp_display_lock. */
void boost_gauge_set_theme(uint8_t theme_id);
/** Call under bsp_display_lock. */
void boost_gauge_reset_peak(void);

float boost_gauge_last_psi(void);
bool boost_gauge_is_ready(void);

#ifdef __cplusplus
}
#endif
