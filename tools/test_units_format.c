/*
 * Host test: the bar/kPa big-digit magnitude must have ONE definition.
 *
 * The per-sample big-digit readout paints an integer-decomposed magnitude
 * through boost_units_format_scaled() (no float printf, no heap on the 16 ms
 * path). That string is only correct if it agrees with the contract formatter
 * boost_units_format() in EVERY unit. This test mirrors the readout's exact
 * arithmetic and compares its magnitude against boost_units_format() over the
 * gauge's full psi domain, so a future edit to either one cannot quietly drift
 * from the other (the previous duplicate implementation had no such pin).
 *
 * Wired in sim/CMakeLists.txt as `test_units_format`, like test_neon_geom.
 */

#include "boost_units.h"
#include "boost_neon_geom.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Exactly the readout's arithmetic (main/boost_gauge.c update_bigdigit): fold
 * the psi-defined dead band, convert, scale, round, decompose. */
static void magnitude_through_scaled(boost_unit_t unit, float psi,
                                     char *out, size_t out_len)
{
    const float readout_psi = boost_readout_display_psi(psi);
    const float value = boost_units_from_psi(unit, readout_psi);
    const int decimals = boost_units_decimals(unit);
    long scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const long scaled = (long)lroundf(fabsf(value) * (float)scale);
    boost_units_format_scaled(out, out_len, scaled, decimals);
}

/* Magnitude of the contract formatter's output (it carries the sign). */
static const char *magnitude_of(const char *s)
{
    return (s[0] == '-') ? s + 1 : s;
}

int main(void)
{
    /* Fixed reference points for the decomposition itself. */
    {
        char b[24];
        boost_units_format_scaled(b, sizeof(b), 207, 2);
        if (strcmp(b, "2.07") != 0) { fprintf(stderr, "2.07 != %s\n", b); return 1; }
        boost_units_format_scaled(b, sizeof(b), 7, 2);
        if (strcmp(b, "0.07") != 0) { fprintf(stderr, "0.07 != %s\n", b); return 1; }
        boost_units_format_scaled(b, sizeof(b), 0, 2);
        if (strcmp(b, "0.00") != 0) { fprintf(stderr, "0.00 != %s\n", b); return 1; }
        boost_units_format_scaled(b, sizeof(b), 221, 0);
        if (strcmp(b, "221") != 0) { fprintf(stderr, "221 != %s\n", b); return 1; }
        boost_units_format_scaled(b, sizeof(b), 0, 0);
        if (strcmp(b, "0") != 0) { fprintf(stderr, "0 != %s\n", b); return 1; }
        boost_units_format_scaled(b, sizeof(b), 1234, 0);
        if (strcmp(b, "1234") != 0) { fprintf(stderr, "1234 != %s\n", b); return 1; }
    }

    /* The big-digit bar/kPa readout is the only scaled-formatter caller, so
     * the psi domain is exercised through the two units it actually paints. */
    static const boost_unit_t units[2] = { BOOST_UNIT_BAR, BOOST_UNIT_KPA };
    static const char *names[2] = { "bar", "kPa" };

    /* Full gauge domain: psi in [-30, 40] step 0.01. */
    int compares = 0;
    for (int u = 0; u < 2; ++u) {
        for (int i = -3000; i <= 4000; ++i) {
            const float psi = (float)i * 0.01f;
            char scaled[32], ref[32];
            magnitude_through_scaled(units[u], psi, scaled, sizeof(scaled));
            boost_units_format(units[u], psi, true, ref, sizeof(ref));
            compares++;
            if (strcmp(scaled, magnitude_of(ref)) != 0) {
                fprintf(stderr, "MISMATCH unit=%s psi=%.2f scaled=%s ref=%s\n",
                        names[u], (double)psi, scaled, magnitude_of(ref));
                return 1;
            }
        }
    }

    printf("test_units_format: PASS (%d compares, psi in [-30, 40] step 0.01,\n"
           "units bar/kPa)\n", compares);
    return 0;
}
