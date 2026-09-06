#!/usr/bin/env python3
"""dyno-cell readout dead zone - source-contract test (no device).

Guards the 2026-09-05 user-requested dead zone and the BLE/Wi-Fi coex scan
contract it shipped with:

  * Readout dead zone: ARC_READOUT_DEADBAND = 0.1 in boost_gauge.c folds
    psi within +-0.1 to a solid 0.0 and SHIFTS (not clamps) outside it, so
    the mapping stays continuous at the edges; format_value_slots() routes
    through arc_readout_display_psi(). The web mirror (web/app.js
    drawFixedPsi -> arcReadoutDisplayPsi, ARC_READOUT_DEADBAND = 0.1) must
    carry the same constant and the same shift semantics - firmware and
    browser must agree or the mirrored face disagrees with the panel.
  * The arc wedge and zone colours keep the RAW psi (draw_value_arc and
    value_arc_angles must not reference the dead band).
  * BLE coex scan: both esp_wifi_scan_start() call sites leave scan_time
    zeroed (default dwell REQUIRED when Bluetooth is enabled - custom
    40/80 ms values returned WIFI_STATE_INIT and BLE /network/scan answered
    "scan_failed", hardware log 2026-09-05), and boost_network_scan() retries
    once when the background saved-network scan holds the radio instead of
    surfacing a transient failure to the phone.

Stdlib only. Run:  python3 tools/tests/test_readout_deadzone.py
"""

from __future__ import annotations

import math
import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GAUGE_C = REPO_ROOT / "main" / "boost_gauge.c"
APP_JS = REPO_ROOT / "web" / "app.js"
NET_C = REPO_ROOT / "main" / "boost_network.c"


class Result:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        if ok:
            print(f"PASS {label}")
        else:
            self.failures.append(label)
            print(f"FAIL {label}  [{detail if detail else 'assertion failed'}]")


def display_psi(psi: float, band: float = 0.1) -> float:
    """Reference implementation of the dead-zone mapping (both sides must
    implement exactly this shape): fold the open band to zero, pass through
    outside. A one-band shift would break the sign at the edge (raw -0.15
    shifted to -0.05 clears the sign threshold and renders positive)."""
    if -band <= psi <= band:
        return 0.0
    return psi


def main() -> int:
    result = Result()
    gauge_c = GAUGE_C.read_text(encoding="utf-8")
    app_js = APP_JS.read_text(encoding="utf-8")
    net_c = NET_C.read_text(encoding="utf-8")

    # --- firmware constant + shape ------------------------------------------
    m = re.search(r"#define\s+ARC_READOUT_DEADBAND\s+([0-9.]+)f", gauge_c)
    result.check(m is not None,
                 "boost_gauge.c defines ARC_READOUT_DEADBAND",
                 "missing #define ARC_READOUT_DEADBAND")
    band = float(m.group(1)) if m else 0.1
    result.check(band == 0.1,
                 "dead band is 0.1 psi (user request 2026-09-05)",
                 f"band={band}")

    result.check("if (psi >= -ARC_READOUT_DEADBAND && psi <= ARC_READOUT_DEADBAND)" in gauge_c
                 and "return psi;" in gauge_c,
                 "firmware FOLDS the band to 0.0 and passes raw values outside",
                 "expected fold shape in arc_readout_display_psi")

    result.check(re.search(
        r"static void format_value_slots\(char \*sign, char \*tens, char \*ones, char \*tenths, float psi\)\s*\{\s*psi = arc_readout_display_psi\(psi\);",
        gauge_c) is not None,
        "format_value_slots routes through arc_readout_display_psi",
        "dead zone not applied in the readout formatter")

    # --- web mirror parity ---------------------------------------------------
    m_js = re.search(r"const\s+ARC_READOUT_DEADBAND\s*=\s*([0-9.]+)", app_js)
    result.check(m_js is not None,
                 "web/app.js defines ARC_READOUT_DEADBAND",
                 "missing const ARC_READOUT_DEADBAND")
    if m_js:
        result.check(float(m_js.group(1)) == band,
                     "web dead band matches firmware",
                     f"web={m_js.group(1)} fw={band}")

    result.check("psi >= -ARC_READOUT_DEADBAND && psi <= ARC_READOUT_DEADBAND" in app_js
                 and "return psi;" in app_js,
                 "web mapping FOLDS the band and passes raw outside (parity)",
                 "expected fold shape in arcReadoutDisplayPsi")

    result.check(re.search(
        r"function drawFixedPsi\(psi, decimalX, baselineY, scale\)\s*\{\s*const value = arcReadoutDisplayPsi\(Number\(psi\)\);",
        app_js) is not None,
        "drawFixedPsi routes through arcReadoutDisplayPsi",
        "web dyno readout not using the dead zone")

    # --- wedge/zone use raw psi ---------------------------------------------
    value_arc = gauge_c[gauge_c.index("static void value_arc_angles"):]
    value_arc = value_arc[:value_arc.index("\n}\n")]
    result.check("ARC_READOUT_DEADBAND" not in value_arc,
                 "value_arc_angles uses raw psi (dead zone is readout-only)")

    zone_fn = gauge_c[gauge_c.index("static lv_color_t zone_color_for_psi"):]
    zone_fn = zone_fn[:zone_fn.index("\n}\n")]
    result.check("ARC_READOUT_DEADBAND" not in zone_fn,
                 "zone_color_for_psi uses raw psi (dead zone is readout-only)")

    # --- reference-value sanity for the shared mapping ----------------------
    # Inside the band: solid 0.0. Outside: raw value, correct sign.
    cases = [(-0.14, "-0.1"), (-0.11, "-0.1"), (-0.1, "0.0"), (-0.05, "0.0"),
             (0.0, "0.0"), (0.05, "0.0"), (0.1, "0.0"), (0.11, "0.1"),
             (0.14, "0.1"), (0.2, "0.2")]
    for psi, expect in cases:
        got = display_psi(psi, band)
        rendered = f"{got:.1f}" if abs(got) > 1e-9 else "0.0"
        result.check(rendered == expect,
                     f"display psi {psi:+.2f} renders {expect}",
                     f"got {rendered}")

    # --- BLE coex scan contract ----------------------------------------------
    # Two productive call sites (background saved-network scan + API scan);
    # the third textual occurrence is the retry inside boost_network_scan().
    scan_starts = [m.start() for m in re.finditer(r"esp_wifi_scan_start\(&", net_c)]
    result.check(len(scan_starts) == 3,
                 "three esp_wifi_scan_start(&) sites (bg scan, API scan, API retry)",
                 f"found {len(scan_starts)}")
    for pos in scan_starts:
        window = net_c[pos:pos + 400]
        head = net_c[max(0, pos - 500):pos]
        block = head + window
        has_custom = re.search(r"scan_time\.active\.(min|max)\s*=", block) is not None
        result.check(not has_custom,
                     "scan call site leaves scan_time default (BLE coex)",
                     "custom active scan time found near esp_wifi_scan_start()")

    result.check("WIFI_SCAN_ACTIVE_MIN_MS" not in net_c
                 and "WIFI_SCAN_ACTIVE_MAX_MS" not in net_c,
                 "custom scan-time macros removed",
                 "WIFI_SCAN_ACTIVE_* still referenced")

    retry = re.search(
        r"ESP_ERR_WIFI_STATE && s_background_scan_running", net_c)
    result.check(retry is not None,
                 "boost_network_scan retries once when the background scan holds the radio",
                 "expected ESP_ERR_WIFI_STATE + s_background_scan_running retry")

    result.check("scan_failed" not in net_c,
                 "no 'scan_failed' string in boost_network.c",
                 "BLE route owns that error path, not the driver")

    print()
    if result.failures:
        print(f"{len(result.failures)} failure(s) of {result.checks} checks")
        return 1
    print(f"all {result.checks} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
