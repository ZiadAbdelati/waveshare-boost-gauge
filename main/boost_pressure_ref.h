#pragma once
/* Pressure DISPLAY reference: relative (gauge) vs absolute.
 *
 * Canonical storage stays GAUGE psi everywhere: /state.psi, the CSV and BLE
 * logs, TPMS, calibration diagnostics, needle position, arc geometry and the
 * zone-colour decision. This module owns ONLY what a numeral shows.
 *
 *   absolute = gauge + atmospheric
 *
 * where the atmospheric reference is the BMP280 ambient the gauge is already
 * zeroed against - boost_sensors.c computes gauge psi as `map_abs - ambient`,
 * so absolute mode displays the MAP sensor's absolute manifold pressure and
 * derives nothing new. A BMP280 that never answered leaves the sensor layer
 * publishing the standard-atmosphere constant with `ambient_is_fallback` set,
 * so the reference degrades to 101.325 kPa rather than to zero.
 *
 * ORDERING (load-bearing): the shared +-0.1 psi readout fold applies FIRST, in
 * gauge psi - the band is defined in gauge psi - and the reference is added
 * after it. A sample folded flat to 0.0 gauge therefore displays 14.7 absolute,
 * and the neon zone colour/id, which folds but stays gauge-relative, is
 * untouched by this module.
 *
 * Scope: every BOOST-side numeral (readouts, peaks, dial scale numerals).
 * NEVER TPMS (boost_units_format() stays raw there), never a zone threshold,
 * never geometry, never the wire. */

#include <stdbool.h>
#include <stddef.h>

#include "boost_units.h"

/* The standard atmosphere in kPa, and the sensor layer's fallback too:
 * main/boost_sensors.c aliases this constant so the two cannot drift apart. */
#define BOOST_STANDARD_ATM_KPA 101.325f

/* The persisted display mode. Global, not per-theme; the theme store owns it. */
bool boost_pressure_ref_absolute(void);

/* The live atmospheric reference, in psi. */
float boost_pressure_ref_psi(void);

/* The effective atmospheric baseline in kPa: the test/sim override when one is
 * set, else the published BMP280 value, else the standard atmosphere. */
float boost_pressure_ref_atmosphere_kpa(void);

/* Gauge psi -> displayed psi under the current reference. `fold_deadband`
 * applies the shared fold first. In relative mode this is an EXACT
 * passthrough - no arithmetic at all - which is what keeps every psi render
 * byte-identical to the pre-reference build. */
float boost_pressure_ref_display(float gauge_psi, bool fold_deadband);

/* Boost-side formatters: the reference transform lives here so no call site can
 * forget it. Each is boost_units_*() with the display value substituted. */
float boost_pressure_ref_from_psi(boost_unit_t unit, float gauge_psi, bool fold_deadband);
void boost_pressure_ref_format(boost_unit_t unit, float gauge_psi, bool fold_deadband,
                               char *out, size_t out_len);
void boost_pressure_ref_format_tick(boost_unit_t unit, float gauge_psi,
                                    char *out, size_t out_len);

/* Publish the atmospheric baseline from the current sample. A non-positive or
 * fallback reading becomes the standard atmosphere; age deliberately does NOT
 * invalidate it (a barometric baseline holds its value, and the constant is
 * 1.5 % away in the worst case). */
void boost_pressure_ref_update(float ambient_kpa, bool ambient_is_fallback);

/* Test/sim override for the atmosphere, in kPa; <= 0 restores the live value. */
void boost_pressure_ref_set_atmosphere_kpa(float kpa);
