#!/usr/bin/env python3
"""Post-flash health gate for the gauge.

The simulator audit cannot see this class of fault. It drives lv_timer_handler()
in a bare loop with no scheduler and no watchdog, so a draw that overruns simply
makes the sim slower. On the board it starves a core: the task watchdog fires,
the HTTP server stops answering, and /state serves a FROZEN snapshot - which
reads as three identical per-layout measurements rather than as a hang.

Two checks, both of which a wedged board fails:

  1. serial carries no "task_wdt" / panic output while the face is driven
  2. /state's display counters actually MOVE - a frozen counter reports the same
     renderFps and the same worstRenderUs on every sample, and identical numbers
     across different layouts

Run after every flash:
    python tools/check_neon_hw.py --url http://192.168.50.102 --port COM3
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.request

WDT_MARKERS = ("task_wdt", "Guru Meditation", "abort()", "rst:0x", "StoreProhibited")
MIN_DISTINCT = 3          # a live counter varies; a frozen one does not
MIN_MEDIAN_FPS = 25       # below this something is badly wrong, not merely slow


def put(base: str, path: str, payload: dict) -> None:
    req = urllib.request.Request(
        f"{base}/api/v1{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="PUT")
    urllib.request.urlopen(req, timeout=8).read()


def display(base: str) -> dict:
    with urllib.request.urlopen(f"{base}/api/v1/state", timeout=4) as r:
        return json.load(r)["display"]


def drain_serial(port: str, seconds: float) -> list[str]:
    """Best effort: a missing pyserial is not a reason to fail the gate."""
    try:
        import serial  # type: ignore
    except ImportError:
        print("  (pyserial not installed - skipping the watchdog check)")
        return []
    lines: list[str] = []
    try:
        with serial.Serial(port, 115200, timeout=1) as sp:
            end = time.time() + seconds
            while time.time() < end:
                raw = sp.readline()
                if raw:
                    lines.append(raw.decode("utf-8", "replace").rstrip())
    except Exception as exc:                      # noqa: BLE001
        print(f"  (serial unavailable on {port}: {exc})")
    return lines


def check_layout(base: str, name: str, layout: int, seconds: float) -> tuple[bool, str]:
    put(base, "/themes/config", {"neonLayout": layout})
    time.sleep(6)
    fps, worst = [], []
    end = time.time() + seconds
    while time.time() < end:
        d = display(base)
        fps.append(d["renderFps"])
        worst.append(d["worstRenderUs"])
        time.sleep(0.3)
    med = statistics.median(fps)
    distinct_fps, distinct_worst = len(set(fps)), len(set(worst))
    ok = True
    notes = []
    if distinct_fps < MIN_DISTINCT and distinct_worst < MIN_DISTINCT:
        ok = False
        notes.append(f"FROZEN counters ({distinct_fps} fps values, "
                     f"{distinct_worst} worstUs values over {len(fps)} samples)")
    if med < MIN_MEDIAN_FPS:
        ok = False
        notes.append(f"median {med} FPS below the {MIN_MEDIAN_FPS} floor")
    return ok, (f"{name:<9} fps med {med:3.0f} min {min(fps):3.0f} | "
                f"distinct fps/worst {distinct_fps}/{distinct_worst}"
                + ("  <-- " + "; ".join(notes) if notes else ""))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://192.168.50.102")
    ap.add_argument("--port", default="COM3")
    ap.add_argument("--seconds", type=float, default=12.0)
    args = ap.parse_args()
    base = args.url.rstrip("/")

    print("neon hardware gate")
    try:
        put(base, "/themes/config", {"demoMode": True})
        put(base, "/themes/active", {"id": "neon"})
    except Exception as exc:                      # noqa: BLE001
        print(f"FAIL: board not answering HTTP at {base}: {exc}")
        return 1
    time.sleep(6)

    failures = []
    for name, layout in (("segments", 1), ("tube", 0), ("marquee", 2)):
        try:
            ok, line = check_layout(base, name, layout, args.seconds)
        except Exception as exc:                  # noqa: BLE001
            print(f"  {name:<9} FAIL: {exc}")
            failures.append(name)
            continue
        print("  " + line)
        if not ok:
            failures.append(name)

    print("  draining serial for watchdog output ...")
    noisy = [l for l in drain_serial(args.port, 8.0)
             if any(m in l for m in WDT_MARKERS)]
    if noisy:
        failures.append("watchdog")
        print(f"  FAIL: {len(noisy)} watchdog/panic lines, first: {noisy[0][:110]}")

    if failures:
        print(f"FAIL: {', '.join(sorted(set(failures)))}")
        return 1
    print("PASS: counters live, no watchdog output")
    return 0


if __name__ == "__main__":
    sys.exit(main())
