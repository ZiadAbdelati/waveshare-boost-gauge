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
    # (the SSID arrives via qr_ap_info() -> ap.ap_ssid since the swipeable
    #  connections overlay unified the widget tree across host + device)
    m = re.search(r'snprintf\(payload,\s*sizeof\(payload\),\s*"WIFI:T:WPA;S:%s;P:%s;;",\s*ap\.ap_ssid,\s*BOOST_AP_PASSWORD\)', page)
    result.check(bool(m), "QR payload format WIFI:T:WPA;S:<ssid>;P:<BOOST_AP_PASSWORD>;; uses ap.ap_ssid (qr_ap_info) + BOOST_AP_PASSWORD")

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

    # --- Overlay gesture wiring (ledger 2026-10-05) ---------------------------
    # The fix that made the settings overlay swipeable is a set of EVENT
    # REGISTRATIONS plus a state-machine rule, and the host harness cannot see
    # either: it drives the state machine directly, so a missing or
    # mis-targeted registration (or a handler that forgets the new drag latch)
    # would still leave `--qr-test` green. Pin them structurally.
    def body_of(sig: str, text: str) -> str:
        """Return the body of the definition of `sig` (skipping declarations)."""
        for m in re.finditer(re.escape(sig), text):
            rest = text[m.end():]
            brace = rest.find("{")
            semi = rest.find(";")
            if brace < 0 or (semi >= 0 and semi < brace):
                continue
            start = m.end() + brace
            depth = 0
            for i in range(start, len(text)):
                if text[i] == "{":
                    depth += 1
                elif text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        return text[start:i + 1]
        return ""

    # Target-ANCHORED on purpose: the point of this check is a mis-targeted
    # registration, so a bare `[^,]+` target pattern is not enough - it passes
    # any comma-free target, e.g. moving the overlay's press callback onto a
    # square inside show_qr(). The target identifier is part of the assertion.
    for site, sig, target in (("the overlay", "static void show_qr(void)", "s_qr_overlay"),
                              ("the square factory",
                               "static lv_obj_t *qr_make_square(lv_obj_t *parent", "b")):
        body = body_of(sig, page)
        result.check(bool(body), f"{site} definition found for the wiring check")
        for cb, ev in (("qr_press_cb", "LV_EVENT_PRESSED"),
                       ("qr_pressing_cb", "LV_EVENT_PRESSING"),
                       ("qr_release_cb", "LV_EVENT_RELEASED")):
            result.check(
                bool(re.search(
                    rf"lv_obj_add_event_cb\({re.escape(target)},\s*{cb},\s*{ev},", body)),
                f"{site} registers {cb} for {ev} on {target}",
                "registration missing, or targeted at the wrong object")

    # One gesture = one touch-down: the mid-drag rebuild must drop only the
    # origin, and show_qr() (also the rebuild path) must touch no gesture state.
    # The overlay's classifier must not reach for the theme either: a vertical
    # flick there does nothing (user decision 2026-10-05). Page 0's own theme
    # swipe (finish_press) is a different function and is unaffected.
    drag_body = body_of("static void qr_drag_update(int32_t x, int32_t y)", page)
    result.check("apply_theme_delta" not in drag_body,
                 "the overlay classifier never changes the theme",
                 "a vertical flick on the settings overlay would switch themes")
    goto_body = body_of("static void qr_goto_page(int32_t page)", page)
    result.check("qr_gesture_rebuild()" in goto_body and "qr_gesture_end()" not in goto_body,
                 "qr_goto_page keeps the one-shot latch across the page rebuild",
                 "must call qr_gesture_rebuild(), not qr_gesture_end()")
    show_body = body_of("static void show_qr(void)", page)
    result.check(
        not re.search(r"s_qr_(press_tracking|drag_classified|swipe_suppress|drag_seen)\s*=", show_body),
        "show_qr() leaves gesture state alone (it is the mid-gesture rebuild path)")

    # A drag is not a tap on the SWITCHES either: all four must consult both
    # latches, not only the classified-swipe one. The overlay BACKGROUND
    # (qr_click_cb) is the same rule and must not rot either.
    for cb in ("qr_click_cb", "qr_tap_obd_cb", "qr_tap_app_cb", "qr_tap_units_cb",
               "qr_tap_ref_cb"):
        body = body_of(f"static void {cb}(lv_event_t *event)", page)
        result.check(bool(re.search(r"s_qr_swipe_suppress\s*\|\|\s*s_qr_drag_seen", body)),
                     f"{cb} treats a short drag as a drag (both latches)",
                     "a 12-47 px flick would act as a tap and dismiss/toggle")

    # A toggle must REPAINT. show_qr() bakes the square's glow, LED and ON/OFF
    # line from the live link state, and nothing else repaints the overlay (the
    # 16 ms gauge path is gated on s_qr_active), so applying a toggle without a
    # rebuild left the button looking dead until the next page step - the "BLE
    # buttons don't respond to taps" report (2026-10-05). Exactly THREE returns
    # are legitimate: the no-request guard and the unit/reference branches, which
    # each do their own scene rebuild. Every BLE request must FALL THROUGH to the
    # tail rebuild - checking only the tail statement would stay green if a BLE
    # branch grew its own `return;`, leaving the button stale. The harness also
    # catches that behaviourally (its stubs are stateful, so a stale square shows
    # up in the rendered text and the frame diff); this pins the source shape too,
    # because a source contract is checkable without building or running the sim.
    toggle_body = body_of("static void qr_toggle_apply_cb(lv_timer_t *timer)", page)
    result.check(bool(toggle_body), "qr_toggle_apply_cb definition found for the repaint check")
    tail = toggle_body.rstrip()
    if tail.endswith("}"):
        tail = tail[:-1]
    tail_lines = [ln.strip() for ln in tail.splitlines()
                  if ln.strip() and not ln.strip().startswith(("/*", "*", "//"))]
    result.check(bool(tail_lines) and tail_lines[-1] == "qr_goto_page(s_qr_page);",
                 "a BLE toggle repaints its square (the applier's tail rebuilds the overlay)",
                 "applying a BLE toggle without qr_goto_page(s_qr_page) leaves the button "
                 "stale until a page step")
    returns = re.findall(r"\breturn\s*;", toggle_body)
    result.check(len(returns) == 3,
                 "every BLE toggle branch falls through to that rebuild",
                 f"expected 3 early returns (no-request, unit, reference), found {len(returns)}: "
                 "a BLE branch that returns skips the repaint")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
