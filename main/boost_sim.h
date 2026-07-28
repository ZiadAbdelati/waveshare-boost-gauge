#pragma once

#include <stdbool.h>
#include <stdint.h>

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
    float map_volts;       /**< Raw ADS1115 A0 voltage from the MAP sensor */
    float map_abs_kpa;     /**< Corrected absolute manifold pressure */
    float map_nominal_kpa; /**< Absolute MAP before the calibration offset */
    float ambient_kpa;     /**< BMP280 ambient/atmospheric baseline */
    bool ads_present;      /**< ADS1115 answered on the bus at boot */
    bool bmp_present;      /**< BMP280 answered on the bus at boot */
    bool sensor_fault;     /**< A required read failed this cycle (holding last good) */

    /* Freshness. Boot-time presence flags alone are not evidence that a sensor
     * is still answering, so calibration gates on these rather than on
     * ads_present / bmp_present. UINT32_MAX means "never read successfully". */
    uint32_t ads_age_ms;      /**< ms since the last successful ADS1115 read */
    uint32_t bmp_age_ms;      /**< ms since the last successful BMP280 read */
    uint32_t bmp_updates;     /**< Monotonic count of successful BMP280 reads */
    bool ambient_is_fallback; /**< ambient_kpa is the standard-atmosphere
                               *   constant, not a measurement */
} boost_sample_t;

void boost_sim_init(void);
void boost_sim_reset_peak(void);

/**
 * Advance the demo waveform and return the latest sample.
 * Call from a FreeRTOS task at ~50–100 Hz.
 */
boost_sample_t boost_sim_tick(void);

/**
 * Diagnostic-only, transient (NOT persisted, reset to false by boost_sim_init()
 * on every boot): swap the sine-envelope demo waveform for a symmetric triangle
 * sweeping the full PSI_MIN..PSI_MAX range at a constant slew rate matching the
 * envelope waveform's own measured peak trend |dpsi/dt|. Exists so a fast-motion
 * measurement window can be sustained for an entire sampling run instead of
 * hunting for the brief fast segments the organic waveform produces only near
 * its sinusoid's zero-crossings. See tools/bench_fast_motion.py, which derives
 * the exact constant from the same formula this file uses. No effect unless
 * demo mode is also on (boost_theme_demo_mode()).
 */
void boost_sim_set_fast_sweep(bool enabled);
bool boost_sim_fast_sweep(void);

#ifdef __cplusplus
}
#endif
