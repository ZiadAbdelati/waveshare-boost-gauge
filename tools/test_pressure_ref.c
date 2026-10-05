/*
 * Host test: the pressure DISPLAY REFERENCE has exactly one definition.
 *
 * Two properties are load-bearing and are pinned here so no future edit can
 * quietly break either:
 *
 *  1. ATMOSPHERIC MODE IS A BYTE-FOR-BYTE PASSTHROUGH. The gauge's psi render
 *     path is a measured 60 FPS guard; adding a display reference must not
 *     perturb a single character of it. Every formatter is compared - string
 *     identical, not "close" - against the pre-reference boost_units_*() call
 *     over the gauge's whole psi domain.
 *
 *  2. THE FOLD PRECEDES THE REFERENCE, IN GAUGE PSI. The +-0.1 psi readout band
 *     is defined in gauge psi, so an engine-off sample that folds flat must
 *     display as EXACTLY the atmosphere (14.7), not as the raw noise plus the
 *     atmosphere. Applying the reference first would make the folded value
 *     drift by the sensor's own jitter - which is the flicker the band exists
 *     to remove.
 *
 * Wired in sim/CMakeLists.txt as `test_pressure_ref`, like test_units_format.
 * No LVGL or IDF dependency.
 */

#include "boost_pressure_ref.h"
#include "boost_neon_geom.h"
#include "boost_theme.h"
#include "boost_units.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

/* The gauge's own psi domain (clamp_psi_min/clamp_psi_max in boost_model.c). */
#define DOMAIN_LO (-30.0f)
#define DOMAIN_HI (40.0f)

static const boost_unit_t k_units[3] = { BOOST_UNIT_PSI, BOOST_UNIT_BAR, BOOST_UNIT_KPA };
static const char *k_unit_names[3] = { "psi", "bar", "kPa" };

int main(void)
{
    /* ---- baseline: the reference defaults to the standard atmosphere ---- */
    boost_pressure_ref_set_atmosphere_kpa(0.0f);
    boost_pressure_ref_update(0.0f, false);          /* never read */
    check(fabsf(boost_pressure_ref_psi() - 14.6959f) < 0.0005f,
          "standard-atmosphere reference in psi");
    boost_pressure_ref_update(0.0f, true);           /* explicit fallback */
    check(fabsf(boost_pressure_ref_psi() - 14.6959f) < 0.0005f,
          "ambient_is_fallback lands on the standard atmosphere");
    boost_pressure_ref_update(NAN, false);           /* corrupt reading */
    check(fabsf(boost_pressure_ref_psi() - 14.6959f) < 0.0005f,
          "a NaN reading lands on the standard atmosphere");

    /* A real reading is used as-is. */
    boost_pressure_ref_update(95.0f, false);
    check(fabsf(boost_pressure_ref_psi() - 95.0f * 0.145037738f) < 1e-5f,
          "a live BMP280 reading is the reference");

    /* The sim/test override wins over the published baseline, and clearing it
     * hands control back. */
    boost_pressure_ref_set_atmosphere_kpa(101.325f);
    check(fabsf(boost_pressure_ref_atmosphere_kpa() - 101.325f) < 1e-4f, "override wins");
    boost_pressure_ref_update(95.0f, false);
    check(fabsf(boost_pressure_ref_atmosphere_kpa() - 101.325f) < 1e-4f,
          "override still wins after a publish");
    boost_pressure_ref_set_atmosphere_kpa(0.0f);
    check(fabsf(boost_pressure_ref_atmosphere_kpa() - 95.0f) < 1e-4f, "clearing restores live");

    /* ---- property 1: relative mode is an exact passthrough ---- */
    boost_theme_set_pressure_absolute(false);
    check(!boost_pressure_ref_absolute(), "mode flag reaches the reference module");

    int passthrough = 0;
    for (int u = 0; u < 3; ++u) {
        for (int i = -3000; i <= 4000; ++i) {
            const float psi = (float)i * 0.01f;
            for (int fold = 0; fold < 2; ++fold) {
                const float got = boost_pressure_ref_display(psi, fold != 0);
                const float want = fold ? boost_readout_display_psi(psi) : psi;
                if (memcmp(&got, &want, sizeof(got)) != 0) {
                    fprintf(stderr, "FAIL passthrough unit=%s psi=%.2f fold=%d "
                                    "got=%.9g want=%.9g\n",
                            k_unit_names[u], (double)psi, fold, (double)got, (double)want);
                    failures++;
                    return 1;
                }
                char got_s[32], want_s[32];
                boost_pressure_ref_format(k_units[u], psi, fold != 0, got_s, sizeof(got_s));
                boost_units_format(k_units[u], psi, fold != 0, want_s, sizeof(want_s));
                if (strcmp(got_s, want_s) != 0) {
                    fprintf(stderr, "FAIL format passthrough unit=%s psi=%.2f fold=%d "
                                    "got=%s want=%s\n",
                            k_unit_names[u], (double)psi, fold, got_s, want_s);
                    failures++;
                    return 1;
                }
                passthrough++;
            }
        }
        /* Ticks never fold; relative mode must still change nothing. */
        for (int i = -3000; i <= 4000; ++i) {
            const float psi = (float)i * 0.01f;
            char got_s[32], want_s[32];
            boost_pressure_ref_format_tick(k_units[u], psi, got_s, sizeof(got_s));
            boost_units_format_tick(k_units[u], psi, want_s, sizeof(want_s));
            if (strcmp(got_s, want_s) != 0) {
                fprintf(stderr, "FAIL tick passthrough unit=%s psi=%.2f got=%s want=%s\n",
                        k_unit_names[u], (double)psi, got_s, want_s);
                failures++;
                return 1;
            }
            passthrough++;
        }
    }

    /* ---- property 2: the fold precedes the reference ---- */
    boost_theme_set_pressure_absolute(true);
    check(boost_pressure_ref_absolute(), "absolute mode flag");

    /* Back to a known atmosphere: the assertions below quote real strings, and
     * the live-reading case above deliberately left 95 kPa in the baseline. */
    boost_pressure_ref_update(BOOST_STANDARD_ATM_KPA, false);
    const float ref = boost_pressure_ref_psi();

    /* Inside the band the fold wins outright: the display is the atmosphere,
     * whatever the raw jitter was. Outside it the raw value passes through. */
    static const float band[][2] = {
        { 0.0f, 0.0f }, { 0.05f, 0.0f }, { -0.05f, 0.0f }, { 0.099f, 0.0f }, { -0.099f, 0.0f },
        { 0.2f, 0.2f }, { -0.4f, -0.4f }, { 5.0f, 5.0f }, { -12.0f, -12.0f },
    };
    for (size_t i = 0; i < sizeof(band) / sizeof(band[0]); ++i) {
        const float folded = boost_readout_display_psi(band[i][0]);
        if (folded != band[i][1]) {
            fprintf(stderr, "FAIL test table: fold(%.3f)=%.3f expected %.3f\n",
                    (double)band[i][0], (double)folded, (double)band[i][1]);
            failures++;
            return 1;
        }
        const float got = boost_pressure_ref_display(band[i][0], true);
        if (fabsf(got - (band[i][1] + ref)) > 1e-4f) {
            fprintf(stderr, "FAIL ordering psi=%.3f: display %.4f != folded %.3f + ref %.4f\n",
                    (double)band[i][0], (double)got, (double)band[i][1], (double)ref);
            failures++;
            return 1;
        }
    }

    /* The engine-off case, spelled out: 0.0 gauge -> 14.7 absolute in psi, and
     * the value the dial's own numerals would carry. */
    char buf[32];
    boost_pressure_ref_format(BOOST_UNIT_PSI, 0.0f, true, buf, sizeof(buf));
    check(strcmp(buf, "14.7") == 0, "0.0 gauge folds flat and reads 14.7 absolute");
    boost_pressure_ref_format(BOOST_UNIT_PSI, 0.02f, true, buf, sizeof(buf));
    check(strcmp(buf, "14.7") == 0, "in-band jitter does not move the absolute readout");
    boost_pressure_ref_format(BOOST_UNIT_PSI, 5.0f, true, buf, sizeof(buf));
    check(strcmp(buf, "19.7") == 0, "5.0 gauge reads 19.7 absolute");
    boost_pressure_ref_format(BOOST_UNIT_PSI, -12.0f, true, buf, sizeof(buf));
    check(strcmp(buf, "2.7") == 0, "-12.0 gauge reads 2.7 absolute");

    /* Dial numerals carry the reference and never fold. */
    boost_pressure_ref_format_tick(BOOST_UNIT_PSI, 0.0f, buf, sizeof(buf));
    check(strcmp(buf, "14.7") == 0, "the atmosphere tick reads 14.7 in absolute mode");
    boost_pressure_ref_format_tick(BOOST_UNIT_KPA, 0.0f, buf, sizeof(buf));
    check(strcmp(buf, "101") == 0, "the atmosphere tick reads 101 kPa in absolute mode");

    /* ---- the reference composes with the unit conversion ---- */
    for (int u = 0; u < 3; ++u) {
        for (int i = -3000; i <= 4000; ++i) {
            const float psi = (float)i * 0.01f;
            const float got = boost_pressure_ref_from_psi(k_units[u], psi, true);
            const float want = boost_units_from_psi(
                k_units[u], boost_readout_display_psi(psi) + ref);
            if (fabsf(got - want) > 1e-3f) {
                fprintf(stderr, "FAIL compose unit=%s psi=%.2f got=%.5f want=%.5f\n",
                        k_unit_names[u], (double)psi, (double)got, (double)want);
                failures++;
                return 1;
            }
        }
    }

    /* ---- the wire is never touched ---- */
    /* Nothing in this module writes a psi value back to the model or the JSON;
     * the transform is a pure function of (gauge psi, mode, reference). The
     * strongest available host check is that the module exposes no setter for
     * a displayed value: the only mutators are the atmospheric baseline and the
     * persisted mode. Assert the module's transform is referentially
     * transparent - same inputs, same outputs, no hidden state carry-over. */
    boost_pressure_ref_update(101.325f, false);
    const float a = boost_pressure_ref_display(3.5f, true);
    const float b = boost_pressure_ref_display(3.5f, true);
    check(memcmp(&a, &b, sizeof(a)) == 0, "the transform is a pure function");

    boost_theme_set_pressure_absolute(false);
    boost_pressure_ref_set_atmosphere_kpa(0.0f);

    if (failures == 0) {
        printf("test_pressure_ref: PASS (%d passthrough compares, psi in [%.0f, %.0f] "
               "step 0.01, units psi/bar/kPa; fold-before-reference and "
               "relative-passthrough pinned)\n",
               passthrough, (double)DOMAIN_LO, (double)DOMAIN_HI);
        return 0;
    }
    fprintf(stderr, "test_pressure_ref: %d failure(s)\n", failures);
    return 1;
}
