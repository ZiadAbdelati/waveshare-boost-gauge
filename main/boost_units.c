#include "boost_units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "boost_neon_geom.h"

/* psi -> bar / kPa. 0.145037738 is the firmware's existing psi-per-kPa
 * constant (boost_sensors.c / boost_tpms_protocol.h); kPa uses its exact
 * reciprocal so the two directions cannot drift apart, and bar is kPa/100. */
#define BOOST_PSI_TO_KPA (6.89475729f)
#define BOOST_PSI_TO_BAR (0.0689475729f)

boost_unit_t boost_units_clamp(int stored)
{
    switch (stored) {
        case BOOST_UNIT_PSI:
        case BOOST_UNIT_BAR:
        case BOOST_UNIT_KPA:
            return (boost_unit_t)stored;
        default:
            return BOOST_UNIT_DEFAULT;
    }
}

const char *boost_units_name(boost_unit_t unit)
{
    switch (unit) {
        case BOOST_UNIT_BAR: return "bar";
        case BOOST_UNIT_KPA: return "kPa";
        case BOOST_UNIT_PSI:
        default:             return "psi";
    }
}

const char *boost_units_label(boost_unit_t unit)
{
    switch (unit) {
        case BOOST_UNIT_BAR: return "bar";
        case BOOST_UNIT_KPA: return "kPa";
        case BOOST_UNIT_PSI:
        default:             return "PSI";
    }
}

int boost_units_decimals(boost_unit_t unit)
{
    switch (unit) {
        case BOOST_UNIT_BAR: return 2;
        case BOOST_UNIT_KPA: return 0;
        case BOOST_UNIT_PSI:
        default:             return 1;
    }
}

bool boost_units_parse(const char *text, boost_unit_t *out)
{
    if (text == NULL || out == NULL) return false;
    if (strcmp(text, "psi") == 0) { *out = BOOST_UNIT_PSI; return true; }
    if (strcmp(text, "bar") == 0) { *out = BOOST_UNIT_BAR; return true; }
    if (strcmp(text, "kPa") == 0 || strcmp(text, "kpa") == 0 ||
        strcmp(text, "KPA") == 0) {
        *out = BOOST_UNIT_KPA;
        return true;
    }
    return false;
}

float boost_units_from_psi(boost_unit_t unit, float psi)
{
    switch (unit) {
        case BOOST_UNIT_BAR: return psi * BOOST_PSI_TO_BAR;
        case BOOST_UNIT_KPA: return psi * BOOST_PSI_TO_KPA;
        case BOOST_UNIT_PSI:
        default:             return psi;
    }
}

float boost_units_to_psi(boost_unit_t unit, float value)
{
    switch (unit) {
        case BOOST_UNIT_BAR: return value / BOOST_PSI_TO_BAR;
        case BOOST_UNIT_KPA: return value / BOOST_PSI_TO_KPA;
        case BOOST_UNIT_PSI:
        default:             return value;
    }
}

void boost_units_format(boost_unit_t unit, float psi, bool fold_deadband,
                        char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    float value = fold_deadband ? boost_readout_display_psi(psi) : psi;
    value = boost_units_from_psi(unit, value);
    /* A value that rounds to zero is not negative, however it was measured. */
    if (fabsf(value) < 0.5f / powf(10.0f, (float)boost_units_decimals(unit))) {
        value = 0.0f;
    }
    switch (boost_units_decimals(unit)) {
        case 2:  snprintf(out, out_len, "%.2f", (double)value); break;
        case 0:  snprintf(out, out_len, "%.0f", (double)value); break;
        default: snprintf(out, out_len, "%.1f", (double)value); break;
    }
}

static void trim_trailing_zeros(char *buf)
{
    char *dot = strchr(buf, '.');
    if (dot == NULL) return;
    char *end = buf + strlen(buf);
    while (end > dot + 1 && end[-1] == '0') *--end = '\0';
    if (end > dot && end[-1] == '.') *--end = '\0';
}

void boost_units_format_tick(boost_unit_t unit, float psi,
                             char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) return;
    const float value = boost_units_from_psi(unit, psi);
    const int decimals = boost_units_decimals(unit);
    /* Whole values print without a decimal point, matching the historic psi
     * tick numerals ("0" / "-15" / "10"). */
    const float rounded = roundf(value);
    if (fabsf(value - rounded) < 0.5f / powf(10.0f, (float)decimals) || fabsf(value) < 0.05f) {
        snprintf(out, out_len, "%d", (int)lroundf(value));
        if (strcmp(out, "-0") == 0) snprintf(out, out_len, "0");
        return;
    }
    switch (decimals) {
        case 2:  snprintf(out, out_len, "%.2f", (double)value); break;
        case 0:  snprintf(out, out_len, "%.0f", (double)value); break;
        default: snprintf(out, out_len, "%.1f", (double)value); break;
    }
    trim_trailing_zeros(out);
}
