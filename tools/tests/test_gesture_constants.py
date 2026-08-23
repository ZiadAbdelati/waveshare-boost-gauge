#!/usr/bin/env python3
"""Source-contract test for the physical-input / gesture constants (ledger rows
20, 27-31 area and the 2026-08-10 gesture review).

A silent change to any of these thresholds changes the way the physical panel
feels and is not caught by the HTTP/telemetry gates, so this test pins the exact
values in source. The authoritative QR hold is QR_HOLD_MS (2200 ms) in
main/boost_page.c: it matches the regression ledger ("Two-finger QR (2.2 s
hold)", 2026-08-14/15) and the hardware-verified CST9217 two-point read. The
AGENTS.md top-of-file prose says "3 s" - that is known documentation drift,
reported as informational, not asserted.

Also asserts the TPMS capsule grow contract (ledger 2026-08-15) in both
firmware (main/boost_tpms_ui.c) and the web mirror (web/app.js).

Run:  python3 tools/tests/test_gesture_constants.py
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
PAGE_C = REPO_ROOT / "main" / "boost_page.c"
TPMS_UI_C = REPO_ROOT / "main" / "boost_tpms_ui.c"
WEB_APP_JS = REPO_ROOT / "web" / "app.js"
AGENTS_MD = REPO_ROOT / "AGENTS.md"


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


def source_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def main() -> int:
    result = Result()
    if not PAGE_C.is_file():
        print(f"FAIL missing {PAGE_C}")
        return 1
    page = source_text(PAGE_C)
    tpms_ui = source_text(TPMS_UI_C)
    app_js = source_text(WEB_APP_JS)

    values: dict[str, int] = {}

    def define(name: str, value: int):
        m = re.search(rf"#define\s+{name}\s+(\d+)", page)
        got = int(m.group(1)) if m else None
        values[name] = got
        result.check(got == value, f"{name} == {value}",
                     f"got {got}")

    define("TAP_SLOP_PX", 12)      # tap slop: movement within 12 px resets peak
    define("SWIPE_MIN_PX", 48)      # valid swipe starts at 48 px excursion
    define("HOLD_DIM_MS", 1000)     # one-second hold-to-dim
    define("QR_HOLD_MS", 2200)      # two-finger AP-join QR hold (ledger: 2.2 s)
    define("QR_POLL_MS", 100)       # QR poll cadence

    # 4:5 horizontal / vertical ratio tests used by the classifier.
    h_ratio = re.search(r"ax\s*>=\s*SWIPE_MIN_PX\s*&&\s*\(int64_t\)ax\s*\*\s*4\s*>=\s*\(int64_t\)ay\s*\*\s*5", page)
    v_ratio = re.search(r"ay\s*>=\s*SWIPE_MIN_PX\s*&&\s*\(int64_t\)ay\s*\*\s*4\s*>=\s*\(int64_t\)ax\s*\*\s*5", page)
    result.check(bool(h_ratio), "horizontal page-swipe requires >=48 px and 4:5 ratio")
    result.check(bool(v_ratio), "vertical theme-swipe requires >=48 px and 4:5 ratio")

    # The two-finger QR overlay payload: WIFI:T:WPA;S:<ap_ssid>;P:boost1234;;
    m = re.search(r'snprintf\(payload,\s*sizeof\(payload\),\s*"WIFI:T:WPA;S:%s;P:%s;;",\s*net\.ap_ssid,\s*BOOST_AP_PASSWORD\)', page)
    result.check(bool(m), "QR payload format WIFI:T:WPA;S:<ssid>;P:<BOOST_AP_PASSWORD>;; uses net.ap_ssid + BOOST_AP_PASSWORD")

    # TPMS capsule grow contract: +2 px, firmware and web mirror in lockstep.
    m = re.search(r"#define\s+TPMS_CAPSULE_GROW\s+2", tpms_ui)
    result.check(bool(m), "main/boost_tpms_ui.c #define TPMS_CAPSULE_GROW 2")
    m = re.search(r"const\s+TPMS_CAPSULE_GROW\s*=\s*2\s*;", app_js)
    result.check(bool(m), "web/app.js TPMS_CAPSULE_GROW = 2")

    # Informational: AGENTS.md top-of-file says "for 3 s" while source+ledger say
    # 2.2 s. This is documentation drift to fix; the test does not fail on it.
    agents = source_text(AGENTS_MD)
    if re.search(r"for 3 s", agents):
        print("WARN: AGENTS.md prose says 'for 3 s' but QR_HOLD_MS == 2200 ms "
              "(ledger/source are authoritative); documentation drift - update AGENTS.md")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
