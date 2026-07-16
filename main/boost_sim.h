#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Simulated manifold pressure state (demo until ADS1115 is wired). */
typedef struct {
    float psi;       /**< Gauge pressure: negative = vacuum, positive = boost */
    float peak_psi;  /**< Peak boost since last reset */
    bool demo;       /**< True while running the synthetic sweep */
} boost_sample_t;

void boost_sim_init(void);
void boost_sim_reset_peak(void);

/**
 * Advance the demo waveform and return the latest sample.
 * Call from a FreeRTOS task at ~50–100 Hz.
 */
boost_sample_t boost_sim_tick(void);

#ifdef __cplusplus
}
#endif
