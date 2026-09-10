#!/usr/bin/env python3
"""Readout dead zone - source-contract test (no device).

Guards the 2026-09-05 user-requested dead zone, extended 2026-09-06 to every
theme EXCEPT vault-tec, plus the BLE/Wi-Fi coex scan contract it shipped
with:

  * Readout dead zone: BOOST_READOUT_DEADBAND_PSI = 0.1 in
    boost_neon_geom.h (the one include shared by every theme's readout
    path) folds psi within +-0.1 to a solid 0.0 and passes raw values
    outside it (fold, NOT shift: a one-band shift breaks the sign at the
    edge - raw -0.15 shifted to -0.05 cleared the sign threshold and
    rendered positive "0.1"). Every folding theme's readout must route
    through boost_readout_display_psi() / the web arcReadoutDisplayPsi():
      - arc / dyno-cell: format_value_slots() via arc_readout_display_psi()
      - night-city HUD:  update_hud() digits + sign
      - big-digit:       update_bigdigit() digits + minus visibility
      - neon:            boost_neon_layout_readout() (draw AND invalidation
                         share this one function, so they fold identically)
    The web mirror (web/app.js arcReadoutDisplayPsi, ARC_READOUT_DEADBAND =
    0.1) must carry the same constant and the same fold semantics at the
    same four sites - firmware and browser must agree or the mirrored face
    disagrees with the panel.
  * Vault-Tec is the DELIBERATE exception (user request 2026-09-06): its
    two-decimal phosphor readout keeps the raw value. Neither update_vault()
    (firmware) nor drawVaultGauge()/splitNum(psi, 2) (web) may fold.
  * Neon zone colour/id (user override 2026-09-09): the zone decision folds
    through the SAME helper BEFORE its thresholds - neon_zone_rgb()/
    neon_zone_id() consume boost_readout_display_psi() and the web mirror's
    drawNeonGauge consumes neonZoneDisplayPsi() (which itself delegates to
    arcReadoutDisplayPsi - one band definition, never a second). Reason:
    engine-off noise (~+-0.05 psi) straddled the raw 0.05 zone threshold and
    flipped vacuum<->boost every sample, re-firing the marquee's deferred
    zone-flip repaint (word-first, arc-next-frame) as visible second-ring
    flashing. dyno-cell's zone_color_for_psi()/value_arc_angles() KEEP the
    raw psi: its arc draws nothing at atmosphere by the zero-gap guard.
  * The dyno-cell arc wedge and zone colours keep the RAW psi (draw_value_arc
    and value_arc_angles must not reference the dead band).
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
GEOM_H = REPO_ROOT / "main" / "boost_neon_geom.h"
GEOM_C = REPO_ROOT / "main" / "boost_neon_geom.c"
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


def function_body(source: str, start_marker: str) -> str:
    """Slice from start_marker to the first '\\n}\\n' - good enough for these
    flat C/JS functions and keeps the checks anchored to real code."""
    begin = source.index(start_marker)
    chunk = source[begin:]
    return chunk[: chunk.index("\n}\n")]


def main() -> int:
    result = Result()
    gauge_c = GAUGE_C.read_text(encoding="utf-8")
    geom_h = GEOM_H.read_text(encoding="utf-8")
    geom_c = GEOM_C.read_text(encoding="utf-8")
    app_js = APP_JS.read_text(encoding="utf-8")
    net_c = NET_C.read_text(encoding="utf-8")

    # --- firmware constant + fold shape (single shared definition) ----------
    m = re.search(r"#define\s+BOOST_READOUT_DEADBAND_PSI\s+([0-9.]+)f", geom_h)
    result.check(m is not None,
                 "boost_neon_geom.h defines BOOST_READOUT_DEADBAND_PSI",
                 "missing #define BOOST_READOUT_DEADBAND_PSI")
    band = float(m.group(1)) if m else 0.1
    result.check(band == 0.1,
                 "dead band is 0.1 psi (user request 2026-09-05)",
                 f"band={band}")

    result.check("if (psi >= -BOOST_READOUT_DEADBAND_PSI && psi <= BOOST_READOUT_DEADBAND_PSI)" in geom_h
                 and "return psi;" in geom_h,
                 "firmware FOLDS the band to 0.0 and passes raw values outside",
                 "expected fold shape in boost_readout_display_psi")

    result.check(re.search(
        r"static void format_value_slots\(char \*sign, char \*tens, char \*ones, char \*tenths, float psi\)\s*\{\s*psi = arc_readout_display_psi\(psi\);",
        gauge_c) is not None,
        "format_value_slots routes through arc_readout_display_psi",
        "dead zone not applied in the arc readout formatter")

    # arc_readout_display_psi must delegate to the shared helper (no second
    # definition of the band - one convention beside an existing one is the
    # thing AGENTS.md forbids).
    arc_helper = function_body(gauge_c, "static float arc_readout_display_psi(float psi)")
    result.check("boost_readout_display_psi(psi)" in arc_helper
                 and "#define" not in arc_helper,
                 "arc_readout_display_psi delegates to the shared fold helper",
                 "arc helper redefines the band locally")

    # --- every folding theme routes through the shared helper ---------------
    hud_body = function_body(gauge_c, "static void update_hud(const boost_sample_t *sample, const boost_theme_t *theme)")
    result.check("boost_readout_display_psi(" in hud_body,
                 "night-city HUD readout folds through the shared dead zone",
                 "update_hud uses raw psi for its digit slots")
    hud_sign = re.search(r"const char \*sign = (\w+) < -0\.05f \? \"-\" : \"\";", hud_body)
    result.check(hud_sign is not None and hud_sign.group(1) == "readout_psi",
                 "HUD sign uses the folded value (same threshold as its digits)",
                 "HUD sign still reads the raw sample")

    big_body = function_body(gauge_c, "static void update_bigdigit(const boost_sample_t *sample, const boost_theme_t *theme)")
    result.check("boost_readout_display_psi(" in big_body,
                 "big-digit readout folds through the shared dead zone",
                 "update_bigdigit uses raw psi for its digit slots")
    big_neg = re.search(r"const bool neg = (\w+) < -0\.05f;", big_body)
    result.check(big_neg is not None and big_neg.group(1) == "readout_psi",
                 "big-digit minus sign uses the folded value",
                 "big-digit minus still reads the raw sample")

    layout_body = function_body(geom_c, "void boost_neon_layout_readout(float psi, int slot_w, int dot_w,")
    result.check("boost_readout_display_psi(psi)" in layout_body,
                 "neon readout layout folds (draw and invalidation share this path)",
                 "boost_neon_layout_readout uses raw psi")

    # --- neon zone colour/id fold (2026-09-09 user override) -----------------
    # The zone decision (colour + id) must consume the READOUT-FOLDED value
    # BEFORE its thresholds: engine-off noise straddling raw 0.05 flipped
    # vacuum<->boost every sample and re-fired the marquee's deferred
    # zone-flip repaint. Draw and flip detection both call these two
    # functions, so folding here keeps them on one decision.
    zone_rgb_body = function_body(
        gauge_c, "static uint32_t neon_zone_rgb(const boost_theme_t *t, float psi)\n{")
    result.check("boost_readout_display_psi(" in zone_rgb_body,
                 "neon_zone_rgb folds psi through the shared helper before the zone thresholds",
                 "neon_zone_rgb decides the zone colour on raw psi")
    result.check("(psi > 0.05f)" not in zone_rgb_body
                 and "(psi >= s_psi_overboost)" not in zone_rgb_body,
                 "neon_zone_rgb has no raw-psi zone threshold left",
                 "thresholds must test the folded value, not raw psi")
    zone_id_body = function_body(gauge_c, "static inline int neon_zone_id(float psi)\n{")
    result.check("boost_readout_display_psi(" in zone_id_body,
                 "neon_zone_id folds psi through the shared helper before the zone thresholds",
                 "neon_zone_id decides the zone id on raw psi")
    result.check("(psi > 0.05f)" not in zone_id_body
                 and "(psi >= s_psi_overboost)" not in zone_id_body,
                 "neon_zone_id has no raw-psi zone threshold left",
                 "thresholds must test the folded value, not raw psi")

    # --- vault-tec is the deliberate exception ------------------------------
    vault_body = function_body(gauge_c, "static void update_vault(const boost_sample_t *sample, const boost_theme_t *theme)")
    result.check("boost_readout_display_psi" not in vault_body
                 and "readout_psi" not in vault_body,
                 "vault-tec keeps the RAW psi (deliberate exception, 2026-09-06)",
                 "update_vault must not fold")

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

    result.check("splitNum(arcReadoutDisplayPsi(psi), 1)" in app_js,
                 "web HUD readout folds through arcReadoutDisplayPsi",
                 "web HUD still splits the raw psi")
    result.check("splitNum(psi, 2)" in app_js,
                 "web vault readout keeps the raw psi (deliberate exception)",
                 "web vault must not fold")

    vault_js = function_body(app_js, "function drawVaultGauge(sample, psi, g)")
    result.check("arcReadoutDisplayPsi" not in vault_js,
                 "web drawVaultGauge contains no fold call",
                 "vault web mirror must stay raw")

    bigdigit_js = function_body(app_js, "function drawBigDigitGauge(sample, psi, g)")
    result.check("arcReadoutDisplayPsi(psi)" in bigdigit_js
                 and "isNeg = readoutPsi < -0.05" in bigdigit_js,
                 "web big-digit readout + minus fold",
                 "web big-digit still uses raw psi")

    neon_js = function_body(app_js, "function drawNeonGauge(sample, psi, g)")
    result.check("const readoutPsi = arcReadoutDisplayPsi(psi);" in neon_js
                 and "if (readoutPsi < 0 && tenthsTotal !== 0)" in neon_js,
                 "web neon readout + sign fold",
                 "web neon still uses raw psi for the digit composition")

    # --- web mirror: neon zone colour/id fold (2026-09-09 user override) -----
    neon_zone_js = function_body(app_js, "function neonZoneDisplayPsi(psi)")
    result.check("return arcReadoutDisplayPsi(psi);" in neon_zone_js
                 and "0.1" not in neon_zone_js,
                 "web neonZoneDisplayPsi delegates to the shared fold (no second band constant)",
                 "zone helper redefines the band locally")
    result.check(neon_js.count("neonZoneDisplayPsi(psi)") >= 2,
                 "web neon zone colour AND zone id consume the folded value",
                 "drawNeonGauge must route both zone sites through neonZoneDisplayPsi")
    result.check("psi > 0.05 ? p.boost : p.vacuum" not in neon_js
                 and "? 2 : psi > 0.05" not in neon_js,
                 "no raw-psi zone threshold remains at the web neon zone sites",
                 "zone thresholds must test the folded value, not raw psi")

    # --- wedge/zone use raw psi ---------------------------------------------
    value_arc = function_body(gauge_c, "static void value_arc_angles")
    result.check("ARC_READOUT_DEADBAND" not in value_arc
                 and "BOOST_READOUT_DEADBAND_PSI" not in value_arc,
                 "value_arc_angles uses raw psi (dead zone is readout-only)")

    zone_fn = function_body(gauge_c, "static lv_color_t zone_color_for_psi")
    result.check("ARC_READOUT_DEADBAND" not in zone_fn
                 and "BOOST_READOUT_DEADBAND_PSI" not in zone_fn,
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
