#!/usr/bin/env python3
"""Measure marquee physical cadence after the pre-scaled readout sprites.

Selects neon / marquee layout / ring-spin on, demo mode, and controls pixel
shift OFF so its 90 s full-screen repaint cannot land inside the window (the
established confound; see AGENTS.md). Reports the same display metrics the
cadence ledger rows quote: renderFps, framesOverBudget, worstRenderUs,
renderGap*, pixelsPerSecond, teWaits/teTimeouts/teSkips. Restores the theme
and config it found before exit.

Usage:
    python tools/bench_neon_readout.py --url http://<board-ip> [--seconds 30]
"""
from __future__ import annotations

import argparse
import json
import time
from urllib.request import Request, urlopen

METRICS = (
    "renderFps", "framesOverBudget", "worstRenderUs",
    "renderGapP50Us", "renderGapMaxUs", "pixelsPerSecond",
    "teWaits", "teTimeouts", "teSkips",
)


def api_get(base: str, path: str) -> dict:
    with urlopen(f"{base}{path}", timeout=5) as response:
        return json.load(response)


def api_put(base: str, path: str, body: dict) -> dict:
    request = Request(f"{base}{path}", data=json.dumps(body).encode(),
                      method="PUT", headers={"Content-Type": "application/json"})
    with urlopen(request, timeout=8) as response:
        return json.load(response)


def report(name: str, samples: dict[str, list]) -> None:
    print(f"{name} - {len(samples['renderFps'])} samples")
    for m in METRICS:
        v = samples.get(m)
        if not v:
            print(f"  {m:20s}: (absent)")
            continue
        v_sorted = sorted(v)
        p50 = v_sorted[len(v_sorted) // 2]
        if m in ("renderFps", "pixelsPerSecond"):
            print(f"  {m:20s}: min {min(v):.0f}  median {p50:.0f}  max {max(v):.0f}")
        else:
            print(f"  {m:20s}: min {min(v)}  median {p50}  max {max(v)}")
    if samples.get("framesOverBudget"):
        n = len(samples["framesOverBudget"])
        print(f"  framesOverBudget/s  : {sum(samples['framesOverBudget']) / (n * 0.25):.1f}")


def capture(base: str, seconds: float) -> dict[str, list]:
    samples: dict[str, list] = {m: [] for m in METRICS}
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        d = api_get(base, "/api/v1/state")["display"]
        for m in METRICS:
            if m in d:
                samples[m].append(d[m])
        time.sleep(0.25)
    return samples


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://192.168.50.102")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--settle", type=float, default=6.0)
    args = ap.parse_args()

    base = args.url
    t0 = api_get(base, "/api/v1/themes")
    cfg0 = {
        "activeThemeId": t0["activeThemeId"],
        "neonLayout": t0.get("neonLayout", 1),
        "neonMarqueeSpin": bool(t0.get("neonMarqueeSpin", False)),
        "demoMode": bool(t0.get("demoMode", False)),
        "pixelShift": bool(t0.get("pixelShift", True)),
    }

    try:
        api_put(base, "/api/v1/themes/config",
                {"activeThemeId": "neon", "neonLayout": 2, "neonMarqueeSpin": True,
                 "demoMode": True, "pixelShift": False})
        time.sleep(args.settle)
        report(f"neon marquee (spin on, demo, pixelShift off) {args.seconds:.0f}s",
               capture(base, args.seconds))
    finally:
        api_put(base, "/api/v1/themes/config", cfg0)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
