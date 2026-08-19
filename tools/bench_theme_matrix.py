#!/usr/bin/env python3
"""Full-theme performance matrix against the live board.

Drives every theme (and each neon layout) under the organic demo waveform,
samples the once-a-second /api/v1/state display telemetry, and reports
pacing/work metrics per arm. Organic acceptance compares completed renders to
the visible gauge demand: quantized faces legitimately demand fewer than 60
renders in dwell seconds. Older firmware without demand telemetry retains the
legacy median renderFps >= 60 gate. The constant-slew harness keeps the direct
median renderFps >= 60 capacity gate.

Determinism: demoMode is forced on, demoFastSweep is forced off so this is
the organic waveform (not the constant-slew fast-motion sweep), and
pixelShift is controlled off so the anti-burn-in full-panel repaint cannot
land inside a measurement window. The board's prior values are restored at
the end.

Arms (all in organic demo mode, matching the cadence-guard precondition
from AGENTS.md - a real MAP sensor at constant atmosphere invalidates
nothing and legitimately reports single-digit renderFps):

    dyno-cell, vault-tec, night-city, big-digit,
    neon tube, neon segments, neon marquee (spin as persisted),
    neon marquee spin-off

Usage:
    python tools/bench_theme_matrix.py --url http://192.168.1.100
                                        [--seconds 30] [--settle 8]

The board's starting theme/layout/spin/demo/pixel-shift state is restored
at the end.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.request

BASE = "http://192.168.1.100"
MIN_MEDIAN_RENDER_FPS = 60
SAMPLE_INTERVAL_SECONDS = 0.25
WARMUP_SAMPLES = 4


def get(base: str, path: str) -> dict:
    with urllib.request.urlopen(f"{base}/api/v1{path}", timeout=4) as r:
        return json.load(r)


def put(base: str, path: str, payload: dict) -> dict:
    req = urllib.request.Request(
        f"{base}/api/v1{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="PUT")
    with urllib.request.urlopen(req, timeout=8) as r:
        return json.load(r)


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


def demand_stats(samples: list[dict]) -> tuple[float, float, int] | None:
    rows = [(int(s["renderFps"]), int(s["gaugeDemandPerSecond"]))
            for s in samples if "gaugeDemandPerSecond" in s]
    if not rows:
        return None
    demanded = [(fps, demand) for fps, demand in rows if demand > 0]
    if not demanded:
        return float("nan"), float("nan"), len(rows)
    coverage = [min(1.0, fps / demand) for fps, demand in demanded]
    shortfall = [max(0, demand - fps) for fps, demand in demanded]
    zero_demand = len(rows) - len(demanded)
    return med(coverage), med(shortfall), zero_demand


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
    demand = demand_stats(samples)
    if demand is None:
        gate = "PASS legacy" if fps_med >= MIN_MEDIAN_RENDER_FPS else "FAIL legacy"
        demand_text = "demand n/a"
    else:
        coverage_med, shortfall_med, zero_demand = demand
        if coverage_med != coverage_med:  # all sampled windows had no demand
            gate = "NO DEMAND"
            demand_text = f"demand idle({zero_demand})"
        else:
            gate = "PASS" if coverage_med >= 0.95 else "FAIL"
            demand_text = (f"coverage {coverage_med * 100:5.1f}% "
                           f"short {shortfall_med:3.0f} idle {zero_demand}")

    print(
        f"{name:<26} fps {fps_min:3.0f}/{fps_med:3.0f}  "
        f"worstUs {w_med_all:7.0f}/{w_max:7.0f}  "
        f"gapP50 {gap_p50:5.0f} gapMax {gap_max:6.0f}  "
        f"ob/s {ob_med:4.0f}/{ob_max:4.0f}  pps {pps:8.0f}  "
        f"fl {flushes:5.0f}  teS/W/T {te_skips}/{te_waits}/{te_timeouts}  "
        f"{demand_text}  n={n}  [{gate}]")


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
    start_demo = bool(initial.get("demoMode", False))
    start_fastsweep = bool(initial.get("demoFastSweep", False))
    start_pixelshift = bool(initial.get("pixelShift", True))
    print(f"restore target: theme={start_theme} layout={start_layout} spin={start_spin} "
          f"demo={start_demo} fastSweep={start_fastsweep} pixelShift={start_pixelshift}")
    print(f"organic demo waveform matrix (demoFastSweep off, pixelShift controlled off), "
          f"{args.seconds:.0f}s per arm after {args.settle:.0f}s settle\n")

    forced = put(base, "/themes/config", {
        "demoMode": True,
        "demoFastSweep": False,
        "pixelShift": False,
    })
    for key, expected in (("demoMode", True), ("demoFastSweep", False),
                          ("pixelShift", False)):
        assert forced.get(key) == expected, \
            f"{key} did not take effect: {forced}"

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
        put(base, "/themes/config", {
            "demoMode": start_demo,
            "demoFastSweep": start_fastsweep,
            "pixelShift": start_pixelshift,
        })
        print(f"\nrestored theme={start_theme} layout={start_layout} spin={start_spin} "
              f"demo={start_demo} fastSweep={start_fastsweep} pixelShift={start_pixelshift}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
