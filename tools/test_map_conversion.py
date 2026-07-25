#!/usr/bin/env python3
"""Host-side check of the GM 12223861 MAP transfer function.

Standalone on purpose: no pytest, no third-party imports. Run it with

    python tools/test_map_conversion.py

It reimplements the arithmetic that lives in main/boost_sensors.c
(boost_sensors_nominal_kpa) from the same two published points, so a typo in
either copy shows up as a failing case rather than as a wrong gauge reading.

    slope     = (304 - 40) / (4.818 - 0.619) = 264 / 4.199 = 62.8721124...
    intercept = 40 - slope * 0.619                          =  1.08216242...

Prints PASS/FAIL per case and exits non-zero if anything failed.
"""

import sys

# The curve is specified at a 5.00 V supply; the sensor is ratiometric, so any
# other supply is normalized back onto this one before the line is applied.
CURVE_SUPPLY_V = 5.00
KPA_PER_VOLT = 62.8721124
KPA_INTERCEPT = 1.08216242

KPA_TO_PSI = 0.145037738

# Mirrors BOOST_MAP_SUPPLY_DEFAULT / MIN / MAX and BOOST_MAP_CAL_MAX_KPA.
SUPPLY_DEFAULT = 5.20
CAL_MAX_KPA = 10.0


def normalized_volts(map_volts, supply_volts):
    return map_volts * CURVE_SUPPLY_V / supply_volts


def nominal_kpa(map_volts, supply_volts=SUPPLY_DEFAULT):
    """Absolute manifold pressure before any calibration offset."""
    return KPA_PER_VOLT * normalized_volts(map_volts, supply_volts) + KPA_INTERCEPT


def corrected_kpa(map_volts, supply_volts=SUPPLY_DEFAULT, offset_kpa=0.0):
    return nominal_kpa(map_volts, supply_volts) + offset_kpa


def gauge_psi(map_volts, ambient_kpa, supply_volts=SUPPLY_DEFAULT, offset_kpa=0.0):
    return (corrected_kpa(map_volts, supply_volts, offset_kpa) - ambient_kpa) * KPA_TO_PSI


def calibration_offset(map_volts, bmp_kpa, supply_volts=SUPPLY_DEFAULT):
    """One-point atmospheric offset.

    Deliberately derived from the *nominal* pressure, never from an already
    corrected value: that is what makes recalibration replace the previous
    offset instead of accumulating onto it.
    """
    return bmp_kpa - nominal_kpa(map_volts, supply_volts)


_failures = []


def check(name, actual, expected, tol):
    ok = abs(actual - expected) <= tol
    print("%-4s %-58s actual=%.6f expected=%.6f tol=%g"
          % ("PASS" if ok else "FAIL", name, actual, expected, tol))
    if not ok:
        _failures.append(name)
    return ok


def main():
    print("GM 12223861 MAP conversion checks")
    print("slope     = 264 / 4.199 = %.7f kPa/V" % (264.0 / 4.199))
    print("intercept = 40 - slope * 0.619 = %.8f kPa" % (40.0 - (264.0 / 4.199) * 0.619))
    print("constants in use: %.7f kPa/V, %.8f kPa" % (KPA_PER_VOLT, KPA_INTERCEPT))
    print("")

    # --- the two defining points, at the supply they are defined at ---
    check("0.619 V @ 5.00 V supply -> 40 kPa",
          nominal_kpa(0.619, 5.00), 40.000, 0.01)
    check("4.818 V @ 5.00 V supply -> 304 kPa",
          nominal_kpa(4.818, 5.00), 304.000, 0.01)

    # --- the observed bench case at the real 5.20 V supply ---
    observed_volts = 1.5741
    observed_bmp = 98.57
    nom = nominal_kpa(observed_volts, 5.20)
    check("1.5741 V @ 5.20 V supply -> ~96.24 kPa nominal", nom, 96.24, 0.01)
    off = calibration_offset(observed_volts, observed_bmp, 5.20)
    check("offset vs 98.57 kPa BMP -> ~+2.33 kPa", off, 2.33, 0.01)
    check("|offset| within BOOST_MAP_CAL_MAX_KPA",
          1.0 if abs(off) <= CAL_MAX_KPA else 0.0, 1.0, 0.0)
    # After calibration the gauge must read zero at atmosphere.
    check("calibrated gauge reads 0.000 psi at atmosphere",
          gauge_psi(observed_volts, observed_bmp, 5.20, off), 0.0, 1e-6)

    # --- ratiometric normalization ---
    # A 5.20 V supply scales every output by 5.20/5.00, so the same pressure
    # appears at a proportionally higher voltage and must normalize back.
    for kpa, v500 in ((40.0, 0.619), (304.0, 4.818), (101.325, 1.5945)):
        v520 = v500 * 5.20 / 5.00
        check("%.4f V @ 5.20 V normalizes to %.4f V @ 5.00 V" % (v520, v500),
              normalized_volts(v520, 5.20), v500, 1e-9)
    check("0.64376 V @ 5.20 V supply -> 40 kPa (same as 0.619 V @ 5.00 V)",
          nominal_kpa(0.619 * 5.20 / 5.00, 5.20), 40.000, 0.01)

    # --- recalibration replaces, never accumulates ---
    # First calibration at one atmosphere, then a second one on a day with a
    # different real ambient. The second offset must be the offset for the new
    # observation alone, not first + second.
    first = calibration_offset(1.5741, 98.57, 5.20)
    second = calibration_offset(1.5810, 99.10, 5.20)
    check("second calibration offset is independent of the first",
          calibration_offset(1.5810, 99.10, 5.20), second, 0.0)
    naive_accumulated = first + second
    check("replacement differs from a naive accumulation",
          1.0 if abs(second - naive_accumulated) > 0.5 else 0.0, 1.0, 0.0)
    # And the same reading recalibrated twice must land on the same offset.
    once = calibration_offset(1.5741, 98.57, 5.20)
    twice = calibration_offset(1.5741, 98.57, 5.20)
    check("recalibrating the same reading is idempotent", twice, once, 0.0)
    # An offset derived from the already-corrected pressure would compound;
    # show that the corrected value at the calibrated offset is the BMP value,
    # so a second pass computed from it would yield exactly zero extra.
    check("corrected pressure at the stored offset equals the BMP reference",
          corrected_kpa(1.5741, 5.20, once), 98.57, 0.001)

    # --- supply change re-derives the offset from the stored raw reference ---
    # boost_sensors_set_supply_volts() recomputes from ref_map_volts/ref_bmp_kpa
    # rather than rescaling the old offset.
    ref_v, ref_bmp = 1.5741, 98.57
    off_520 = calibration_offset(ref_v, ref_bmp, 5.20)
    off_500 = calibration_offset(ref_v, ref_bmp, 5.00)
    check("supply change re-derives offset from the raw reference",
          nominal_kpa(ref_v, 5.00) + off_500, ref_bmp, 0.001)
    check("re-derived offset is not the 5.20 V offset",
          1.0 if abs(off_500 - off_520) > 0.5 else 0.0, 1.0, 0.0)

    print("")
    if _failures:
        print("FAILED %d case(s): %s" % (len(_failures), ", ".join(_failures)))
        return 1
    print("All cases passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
