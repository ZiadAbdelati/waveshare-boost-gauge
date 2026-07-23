#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Real manifold-pressure path: a GM 3-bar MAP sensor on an ADS1115 (A0,
 * single-ended, 0x48) plus a BMP280 (0x76) supplying the ambient/atmospheric
 * baseline. Both hang off a second I2C bus (SCL=GPIO18, SDA=GPIO17) that is
 * entirely separate from the BSP's touch/IO-expander bus (SCL=GPIO14,
 * SDA=GPIO15, port 1). We own I2C port 0.
 *
 * The reads happen on a dedicated FreeRTOS task at a modest cadence; the
 * display timer and the web sample task only ever call
 * boost_sensors_get_sample(), which copies the last computed value under a
 * short mutex and never touches the bus. That is what keeps a slow BMP280 read
 * from ever stalling the 16 ms LVGL render.
 */

/**
 * Bring up I2C port 0 on GPIO17/18, probe both sensors, load the BMP280 factory
 * calibration, configure the ADS1115 for continuous single-ended A0, and start
 * the background reader task. Safe to call once at boot. Logs which sensors
 * were detected and their addresses. Never blocks the caller on the bus beyond
 * the one-time bring-up, and returns even if a sensor is absent (the gauge then
 * runs degraded / faulted rather than crashing).
 */
bool boost_sensors_init(void);

/**
 * Copy the latest computed sample (gauge PSI derived from MAP minus ambient,
 * peak, plus raw diagnostics). demo is always false here. Thread-safe; does no
 * I2C. If the mutex is momentarily contended it returns the previous snapshot,
 * so it never blocks the display.
 */
boost_sample_t boost_sensors_get_sample(void);

/** Zero the real-path peak (wired to the same tap that resets the sim peak). */
void boost_sensors_reset_peak(void);

/**
 * Live I2C bus scan for diagnostics: probes every 7-bit address 0x08..0x77 on
 * the sensor bus and writes the ones that ACK into `out` (up to `max`),
 * returning the count. Returns -1 if the bus never came up. Used by the
 * /api/v1/sensors/scan endpoint so the bus can be inspected without serial —
 * an empty result points at wiring/power/pull-ups, expected 0x48+0x76 confirms
 * both are present, and other addresses reveal a misconfigured device.
 */
int boost_sensors_i2c_scan(uint8_t *out, int max);

#ifdef __cplusplus
}
#endif
