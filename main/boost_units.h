#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Global pressure-display unit. This is a PRESENTATION concern only: every
 * canonical wire/config value stays PSI (gauge) or kPa (TPMS/sensor
 * diagnostics). Callers convert at the display/input boundary through this
 * module so the conversion factors and decimal counts live in exactly one
 * place on the firmware side. The persisted selector lives in the theme store
 * (NVS "unit"), mirroring demoMode/tpmsBle. */
typedef enum {
    BOOST_UNIT_PSI = 0,
    BOOST_UNIT_BAR = 1,
    BOOST_UNIT_KPA = 2,
} boost_unit_t;

#define BOOST_UNIT_DEFAULT BOOST_UNIT_PSI

/* Any stored byte outside the enum becomes the default rather than indexing
 * off the end of a dispatch table. */
boost_unit_t boost_units_clamp(int stored);

/* Stable lowercase token for the JSON API / web dispatch: "psi"|"bar"|"kPa". */
const char *boost_units_name(boost_unit_t unit);
/* Human-facing unit mark used on the gauge faces: "PSI"|"bar"|"kPa". */
const char *boost_units_label(boost_unit_t unit);
/* Decimal places for readouts: PSI 1, bar 2, kPa 0. */
int boost_units_decimals(boost_unit_t unit);

/* Parse the JSON token; returns false (and leaves *out untouched) when the
 * string is not a known unit. */
bool boost_units_parse(const char *text, boost_unit_t *out);

float boost_units_from_psi(boost_unit_t unit, float psi);
float boost_units_to_psi(boost_unit_t unit, float value);

/* Format a PSI value in the unit with the unit's exact decimal count, e.g.
 * "8.4" / "-1.03" / "69". `fold_deadband` applies the shared +-0.1 PSI fold
 * BEFORE conversion (the band is defined in PSI); vault-tec passes false. */
void boost_units_format(boost_unit_t unit, float psi, bool fold_deadband,
                        char *out, size_t out_len);

/* Format a dial scale numeral: integer when the unit-rounded value is whole,
 * otherwise the unit's decimals, e.g. "0" / "-15" / "0.69". */
void boost_units_format_tick(boost_unit_t unit, float psi,
                             char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
