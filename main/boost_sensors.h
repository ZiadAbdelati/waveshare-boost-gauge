#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Real manifold-pressure path: a GM 12223861 three-bar MAP sensor on an ADS1115
 * (A0, single-ended, 0x48) plus a BMP280 (0x76) supplying the ambient/
 * atmospheric baseline. Both hang off a bus that is deliberately separate from
 * the BSP's touch/IO-expander bus (SCL=GPIO14, SDA=GPIO15, port 1). We own I2C
 * port 0 on SCL=GPIO18 / SDA=GPIO17 at 100 kHz.
 *
 * The reads happen on a dedicated FreeRTOS task at the 16 ms cadence contract;
 * the display timer and the web sample task only ever call
 * boost_sensors_get_sample(), which copies the last computed value under a
 * short mutex and never touches the bus. That is what keeps a slow BMP280 read
 * from ever stalling the 16 ms LVGL render.
 *
 * Conversion (see boost_sensors.c for the derivation):
 *   normalized_volts = map_volts * 5.00 / supply_volts
 *   nominal_kpa      = 62.8721124 * normalized_volts + 1.08216242
 *   corrected_kpa    = nominal_kpa + calibration.offset_kpa
 *   gauge_kpa        = corrected_kpa - ambient_kpa
 */

/* ------------------------------------------------------------ calibration */

#define BOOST_MAP_CAL_VERSION   1

/* Configured MAP sensor supply. The GM curve is defined at 5.00 V and the
 * sensor is ratiometric, so a supply that is not 5.00 V must be normalized out
 * before the transfer function is applied. */
#define BOOST_MAP_SUPPLY_DEFAULT 5.20f
#define BOOST_MAP_SUPPLY_MIN     4.50f
#define BOOST_MAP_SUPPLY_MAX     5.50f

/* Largest atmospheric correction we will accept. A real sensor/supply pairing
 * lands within a few kPa; anything larger means the wrong sensor, the wrong
 * supply setting, or a port that is not actually open to atmosphere. */
#define BOOST_MAP_CAL_MAX_KPA    10.0f

/**
 * Persisted one-point atmospheric calibration. Stored as a versioned blob under
 * its own NVS key rather than inside boost_config_t, so growing it never forces
 * a migration of (or risks discarding) the existing gauge settings.
 *
 * The reference fields exist so the offset can be recomputed if the configured
 * supply voltage changes after calibration, preserving the same atmospheric
 * reference instead of carrying an offset derived under the old normalization.
 *
 * One known pressure identifies zero error but cannot independently identify
 * sensor gain, so the correction is deliberately additive in kPa: it is exact at
 * atmosphere, which is the reading that must be right, and degrades gracefully
 * toward full boost. Do not "upgrade" this to a multiplicative fit without a
 * second calibration point — ref_map_volts is stored so one can be added later
 * without a schema migration.
 */
typedef struct {
    uint16_t version;         /**< BOOST_MAP_CAL_VERSION; 0 when never calibrated */
    uint16_t samples;         /**< Snapshots averaged into the reference */
    float    offset_kpa;      /**< Additive correction applied to nominal kPa */
    float    supply_volts;    /**< Configured supply in force at calibration */
    float    ref_map_volts;   /**< Averaged raw ADS1115 A0 volts */
    float    ref_nominal_kpa; /**< Nominal MAP kPa at ref_map_volts / supply */
    float    ref_bmp_kpa;     /**< Averaged BMP280 atmospheric reference */
    int64_t  epoch_ms;        /**< Wall clock at calibration; 0 if unset */
} boost_map_cal_t;

/** Outcome of an atmospheric calibration attempt. Every failure leaves both the
 *  live and the stored calibration untouched. */
typedef enum {
    BOOST_CAL_OK = 0,
    BOOST_CAL_ERR_NO_ADS,        /**< ADS1115 absent */
    BOOST_CAL_ERR_NO_BMP,        /**< BMP280 absent, or ambient is the fallback */
    BOOST_CAL_ERR_STALE,         /**< No fresh reads, or too few BMP updates */
    BOOST_CAL_ERR_UNSTABLE,      /**< Readings moved too much during the window */
    BOOST_CAL_ERR_IMPLAUSIBLE,   /**< Non-finite or out-of-band pressure */
    BOOST_CAL_ERR_OUT_OF_RANGE,  /**< |correction| > BOOST_MAP_CAL_MAX_KPA */
    BOOST_CAL_ERR_PERSIST,       /**< NVS write failed */
    BOOST_CAL_ERR_BUSY,          /**< Another calibration is already running */
} boost_cal_result_t;

/**
 * Bring up I2C port 0, probe both sensors, load the BMP280 factory calibration,
 * configure the ADS1115 for continuous single-ended A0, restore the persisted
 * supply voltage and atmospheric calibration, and start the background reader
 * task. Safe to call once at boot, and only after boost_theme_init() has mounted
 * NVS. Returns even if a sensor is absent (the gauge runs degraded rather than
 * crashing); the return value is true when at least one sensor answered.
 */
bool boost_sensors_init(void);

/**
 * Copy the latest computed sample (gauge PSI derived from corrected MAP minus
 * ambient, peak, plus raw diagnostics and freshness). demo is always false here.
 * Thread-safe; does no I2C. If the mutex is momentarily contended it returns the
 * previous snapshot, so it never blocks the display.
 */
boost_sample_t boost_sensors_get_sample(void);

/** Zero the real-path peak (wired to the same tap that resets the sim peak). */
void boost_sensors_reset_peak(void);

/**
 * Run a one-point atmospheric calibration. Observes published sensor snapshots
 * for roughly two seconds — it issues no I2C of its own, because the reader task
 * remains the single owner of bus traffic — then validates freshness, stability,
 * plausibility, and correction magnitude before persisting and activating the
 * new offset. Blocks the caller for the observation window, which is acceptable
 * for a manual, operator-triggered action.
 *
 * On BOOST_CAL_OK the new record is written to NVS *before* it goes live, and
 * the real-sensor peak is reset so a peak computed under the previous conversion
 * cannot remain on screen. On any error nothing changes. `out` is optional and
 * receives the resulting record on success.
 */
boost_cal_result_t boost_sensors_calibrate_atmosphere(boost_map_cal_t *out);

/** Stable machine-readable code for a calibration result, for the JSON `error`
 *  field. Returns "ok" for BOOST_CAL_OK. */
const char *boost_sensors_cal_error_code(boost_cal_result_t result);

/** Copy the active calibration record. version == 0 means never calibrated. */
boost_map_cal_t boost_sensors_get_calibration(void);

/** Configured MAP supply voltage currently in force. */
float boost_sensors_get_supply_volts(void);

/**
 * Persist and apply a new configured MAP supply voltage without running an
 * atmospheric calibration. If a calibration exists, its offset is recomputed
 * from the stored reference MAP voltage and BMP pressure under the new
 * normalization and the updated record is persisted, so the same atmospheric
 * reference is preserved. Returns ESP_ERR_INVALID_ARG outside
 * [BOOST_MAP_SUPPLY_MIN, BOOST_MAP_SUPPLY_MAX].
 */
esp_err_t boost_sensors_set_supply_volts(float volts);

/** Nominal (pre-offset) absolute kPa for a raw A0 voltage at the configured
 *  supply. Exposed so the API can report nominal beside corrected. */
float boost_sensors_nominal_kpa(float map_volts);

/* ------------------------------------------------------------ diagnostics */

/** Count of in-place I2C bus recoveries since boot. */
uint32_t boost_sensors_recoveries(void);

/**
 * Live I2C bus scan for diagnostics: probes every 7-bit address 0x08..0x77 on
 * the sensor bus and writes addresses that ACK four consecutive probes into
 * `out` (up to `max`). Returns -1 if the bus never came up. Serialized against
 * bus recovery. Used by /api/v1/sensors/scan so the bus can be inspected without
 * serial — stable 0x48 and 0x76 identify the expected sensors; an empty result
 * is correct when no external devices are connected.
 */
int boost_sensors_i2c_scan(uint8_t *out, int max);

#ifdef __cplusplus
}
#endif
