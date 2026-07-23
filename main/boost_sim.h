#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Manifold pressure state. `demo` marks the synthetic sweep; the diagnostic
 *  fields below are populated only on the real-sensor path (the sim leaves them
 *  zero/false) and let the parent sanity-check readings over the API. */
typedef struct {
    float psi;       /**< Gauge pressure: negative = vacuum, positive = boost */
    float peak_psi;  /**< Peak boost since last reset */
    bool demo;       /**< True while running the synthetic sweep */

    /* Real-sensor diagnostics (see boost_sensors.c). */
    float map_volts;    /**< Raw ADS1115 A0 voltage from the MAP sensor */
    float map_abs_kpa;  /**< Absolute manifold pressure from the MAP transfer fn */
    float ambient_kpa;  /**< BMP280 ambient/atmospheric baseline */
    bool ads_present;   /**< ADS1115 answered on the bus at boot */
    bool bmp_present;   /**< BMP280 answered on the bus at boot */
    bool sensor_fault;  /**< A required read failed this cycle (holding last good) */
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
