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
    if (strcmp(text, "kPa") == 0) {
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

/* Format a non-negative scaled integer (display magnitude x 10^decimals) as
 * the unit's magnitude string: 2.07 -> "2.07" (bar), 221 -> "221" (kPa).
 * Integer arithmetic only, so a per-sample (16 ms) caller spends no float
 * printf and allocates nothing. This is the ONE owner of the magnitude shape
 * shared with boost_units_format(); the big-digit per-sample readout calls it
 * rather than carrying its own copy of the digit-splitting rules. */
void boost_units_format_scaled(char *buf, size_t cap, long scaled, int decimals)
{
    if (buf == NULL || cap == 0) return;
    char digits[24];
    int n = 0;
    if (scaled <= 0) {
        digits[n++] = '0';
    } else {
        while (scaled > 0 && n < (int)sizeof(digits)) {
            digits[n++] = (char)('0' + (int)(scaled % 10));
            scaled /= 10;
        }
    }
    /* digits[] is least-significant first; the integer part is the top
     * `n - decimals` digits, the fraction the rest (with a leading "0." when
     * the value is below one unit). */
    const int int_digits = n - decimals;
    size_t w = 0;
    if (int_digits > 0) {
        for (int i = n - 1; i >= decimals && w + 1 < cap; --i) buf[w++] = digits[i];
        if (decimals > 0 && w + 1 < cap) buf[w++] = '.';
        for (int i = decimals - 1; i >= 0 && w + 1 < cap; --i) buf[w++] = digits[i];
    } else {
        if (w + 1 < cap) buf[w++] = '0';
        if (decimals > 0) {
            if (w + 1 < cap) buf[w++] = '.';
            for (int i = 0; i < -int_digits && w + 1 < cap; ++i) buf[w++] = '0';
            for (int i = n - 1; i >= 0 && w + 1 < cap; --i) buf[w++] = digits[i];
        }
    }
    buf[w] = '\0';
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
     * tick numerals ("0" / "-15" / "10"). One threshold only: it is already
     * measured in the unit's own precision (psi 0.5/10 == 0.05), so a separate
     * psi-scaled epsilon would widen bar's integer branch tenfold and make a
     * tick disagree with boost_units_format(). */
    const float rounded = roundf(value);
    if (fabsf(value - rounded) < 0.5f / powf(10.0f, (float)decimals)) {
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
