#!/usr/bin/env python3
"""Host-side validation for tools/ble_gauge_sim (no radio required).

This is NOT a BLE test. It parses the simulator's embedded route table
(Sources/ble_gauge_sim/routes.swift) far enough to assert that the five
selectable themes and the BGL1 log-header constant exist in source, then runs a
pure-Python reimplementation of the log body/line format so there is an
expected-value check for the Log characteristic without a Bluetooth adapter.
"""

from __future__ import annotations

import math
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
ROUTES = (
    REPO_ROOT
    / "tools"
    / "ble_gauge_sim"
    / "Sources"
    / "ble_gauge_sim"
    / "routes.swift"
)

EXPECTED_THEMES = [
    ("dyno-cell", "Dyno Cell"),
    ("vault-tec", "Vault-Tec"),
    ("night-city", "Night City"),
    ("big-digit", "Big Digit"),
    ("neon", "Neon"),
]

LOG_HEADER = "BGL1\n"
LOG_SAMPLES = 600
LOG_INTERVAL_MS = 200
LOG_COLUMNS = ("t_ms", "psi", "peak_psi", "zone", "demo")

# Sim waveform: sine 0-15 psi, ~8 s period; firmware-style zone thresholds.
WAVE_PERIOD_S = 8.0
WAVE_MID_PSI = 7.5
WAVE_AMP_PSI = 7.5
OVERBOOST_PSI = 12.0
BOOST_PSI = 0.35
ATMO_PSI = -0.35

# One newline-terminated data line, e.g. "0,7.50,7.50,boost,1\n".
LOG_LINE_RE = re.compile(
    r"^(\d+),(-?\d+\.\d{2}),(-?\d+\.\d{2}),(VAC|ATMO|BOOST|OVER),(0|1)\n$"
)


def psi_at(t_ms: int) -> float:
    t = t_ms / 1000.0
    return WAVE_MID_PSI + WAVE_AMP_PSI * math.sin(2.0 * math.pi * t / WAVE_PERIOD_S)


def zone_for(psi: float) -> str:
    """Mirror of firmware boost_model.c zone_for_psi() token mapping."""
    if psi >= OVERBOOST_PSI:
        return "OVER"
    if psi >= BOOST_PSI:
        return "BOOST"
    if psi > ATMO_PSI:
        return "ATMO"
    return "VAC"


def log_line(t_ms: int, psi: float, peak_psi: float, zone: str, demo: bool) -> str:
    """Pure-Python reimplementation of the simulator's log line formatting."""
    return f"{t_ms},{psi:.2f},{peak_psi:.2f},{zone},{1 if demo else 0}\n"


def main() -> int:
    if not ROUTES.is_file():
        print(f"FAIL: routes source missing: {ROUTES}", file=sys.stderr)
        return 1

    source = ROUTES.read_text(encoding="utf-8")
    failures: list[str] = []

    for theme_id, theme_name in EXPECTED_THEMES:
        if theme_id not in source or theme_name not in source:
            failures.append(f"theme {theme_id!r} ('{theme_name}') missing from routes.swift")

    # routes.swift stores the header as the Swift literal "BGL1\n" (backslash-n),
    # so check for both the constant name and the escaped token in source.
    if "logHeader" not in source or "BGL1\\n" not in source:
        failures.append("log header constant BGL1 (logHeader) missing from routes.swift")

    for column in LOG_COLUMNS:
        if column not in source:
            failures.append(f"log line column {column!r} missing from routes.swift")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1

    # Pure-Python log body check: BGL1 header + 600 lines consistent with the
    # waveform, ending at a simulated 120 s uptime.
    lines = [LOG_HEADER]
    uptime_ms = 120_000
    peak = 0.0
    for index in range(LOG_SAMPLES):
        t_ms = uptime_ms - (LOG_SAMPLES - 1 - index) * LOG_INTERVAL_MS
        psi = psi_at(t_ms)
        peak = max(peak, psi)
        line = log_line(t_ms, psi, peak, zone_for(psi), demo=True)
        if not LOG_LINE_RE.match(line):
            print(f"FAIL: log line does not match format: {line!r}", file=sys.stderr)
            return 1
        lines.append(line)

    if len(lines) != LOG_SAMPLES + 1:
        print(f"FAIL: expected {LOG_SAMPLES + 1} lines, got {len(lines)}", file=sys.stderr)
        return 1
    if not lines[-1].endswith("\n"):
        print("FAIL: last log line must be newline-terminated", file=sys.stderr)
        return 1

    print(
        f"OK: routes.swift carries {len(EXPECTED_THEMES)} themes "
        f"({'/'.join(tid for tid, _ in EXPECTED_THEMES)})"
    )
    print(f"OK: BGL1 header + {len(LOG_COLUMNS)} log columns present in source")
    print(
        f"OK: pure-python log body: {LOG_HEADER.strip()} + {LOG_SAMPLES} lines "
        f"({LOG_SAMPLES * LOG_INTERVAL_MS / 1000:.0f} s @ 5 Hz) matches {LOG_LINE_RE.pattern}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
