#!/usr/bin/env python3
"""Full-theme performance matrix against the live board.

Drives every theme (and each neon layout) in demo mode, samples the
once-a-second /api/v1/state display telemetry, and reports pacing/work
metrics per arm. This is the hardware side of the "locked 60 fps" goal:
the gate is a sustained median renderFps >= 60, and the supporting metrics
(worstRenderUs, renderGapP50Us, framesOverBudget, pixelsPerSecond) expose
stutter that a single average would hide.

Arms (all in demo mode, matching the cadence-guard precondition from
AGENTS.md - a real MAP sensor at constant atmosphere invalidates nothing
and legitimately reports single-digit renderFps):

    dyno-cell, vault-tec, night-city, big-digit,
    neon tube, neon segments, neon marquee (spin as persisted),
    neon marquee spin-off

Usage:
    python tools/bench_theme_matrix.py --url http://192.168.50.102
                                        [--seconds 30] [--settle 8]

The board's starting theme/layout/spin state is restored at the end.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.request

BASE = "http://192.168.50.102"
MIN_MEDIAN_RENDER_FPS = 60
SAMPLE_INTERVAL_SECONDS = 0.25
WARMUP_SAMPLES = 4


def get(base: str, path: str) -> dict:
    with urllib.request.urlopen(f"{base}/api/v1{path}", timeout=4) as r:
        return json.load(r)


def put(base: str, path: str, payload: dict) -> None:
    req = urllib.request.Request(
        f"{base}/api/v1{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="PUT")
    urllib.request.urlopen(req, timeout=8).read()


def med(xs):
    return statistics.median(xs) if xs else float("nan")


def collect(base: str, seconds: float) -> list[dict]:
    out = []
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            out.append(get(base, "/state")["display"])
        except Exception:  # noqa: BLE001 - transient HTTP blip
            pass
        time.sleep(SAMPLE_INTERVAL_SECONDS)
    return out[WARMUP_SAMPLES:]


def stats(samples: list[dict], key: str) -> tuple:
    vals = [int(s[key]) for s in samples if key in s]
    if not vals:
        return float("nan"), float("nan"), 0
    return min(vals), med(vals), len(vals)


def run_arm(base: str, name: str, theme: str, layout: int | None,
            spin: bool | None, seconds: float, settle: float) -> None:
    put(base, "/themes/active", {"id": theme})
    cfg: dict = {}
    if layout is not None:
        cfg["neonLayout"] = layout
    if spin is not None:
        cfg["neonMarqueeSpin"] = spin
    if cfg:
        put(base, "/themes/config", cfg)
    time.sleep(settle)
    samples = collect(base, seconds)

    fps_min, fps_med, _ = stats(samples, "renderFps")
    w_min, w_med, _ = stats(samples, "worstRenderUs")
    _, w_med_all, _ = stats(samples, "worstRenderUs")
    w_max = max((int(s["worstRenderUs"]) for s in samples), default=0)
    _, gap_p50, _ = stats(samples, "renderGapP50Us")
    gap_max = max((int(s["renderGapMaxUs"]) for s in samples), default=0)
    ob_med = med([int(s["framesOverBudget"]) for s in samples])
    ob_max = max((int(s["framesOverBudget"]) for s in samples), default=0)
    _, pps, _ = stats(samples, "pixelsPerSecond")
    _, flushes, _ = stats(samples, "flushesPerSecond")
    te_skips = sum(int(s.get("teSkips", 0)) for s in samples)
    te_waits = sum(int(s.get("teWaits", 0)) for s in samples)
    te_timeouts = sum(int(s.get("teTimeouts", 0)) for s in samples)
    n = len(samples)
    gate = "PASS" if fps_med >= MIN_MEDIAN_RENDER_FPS else "FAIL"

    print(
        f"{name:<26} fps {fps_min:3.0f}/{fps_med:3.0f}  "
        f"worstUs {w_med_all:7.0f}/{w_max:7.0f}  "
        f"gapP50 {gap_p50:5.0f} gapMax {gap_max:6.0f}  "
        f"ob/s {ob_med:4.0f}/{ob_max:4.0f}  pps {pps:8.0f}  "
        f"fl {flushes:5.0f}  teS/W/T {te_skips}/{te_waits}/{te_timeouts}  "
        f"n={n}  [{gate}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default=BASE)
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--settle", type=float, default=8.0)
    args = ap.parse_args()
    base = args.url.rstrip("/")

    initial = get(base, "/themes")
    start_theme = initial["activeThemeId"]
    start_layout = initial.get("neonLayout")
    start_spin = initial.get("neonMarqueeSpin")
    print(f"restore target: theme={start_theme} layout={start_layout} spin={start_spin}")
    print(f"arm matrix, {args.seconds:.0f}s per arm after {args.settle:.0f}s settle\n")

    put(base, "/themes/config", {"demoMode": True})

    arms = [
        ("dyno-cell",            "dyno-cell", None, None),
        ("vault-tec",            "vault-tec", None, None),
        ("night-city",           "night-city", None, None),
        ("big-digit",            "big-digit", None, None),
        ("neon tube",            "neon", 0, None),
        ("neon segments",        "neon", 1, None),
        ("neon marquee (spin)",  "neon", 2, True),
        ("neon marquee no-spin", "neon", 2, False),
    ]
    try:
        for name, theme, layout, spin in arms:
            try:
                run_arm(base, name, theme, layout, spin, args.seconds, args.settle)
            except Exception as exc:  # noqa: BLE001
                print(f"{name:<26} FAILED: {exc}")
    finally:
        put(base, "/themes/active", {"id": start_theme})
        put(base, "/themes/config", {"neonLayout": start_layout})
        if start_spin is not None:
            put(base, "/themes/config", {"neonMarqueeSpin": start_spin})
        print(f"\nrestored theme={start_theme} layout={start_layout} spin={start_spin}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
