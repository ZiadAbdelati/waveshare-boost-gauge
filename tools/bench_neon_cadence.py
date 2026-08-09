#!/usr/bin/env python3
"""Per-layout neon + reference cadence harness (30 s windows, demo mode).

Drives the board through the three neon layouts (plus an optional dyno-cell
reference arm), sampling the same display telemetry the theme-matrix and
cadence-guard tools use, and prints one PASS/FAIL line per arm against the
canonical >=60 median-renderFps guard.

Usage:
  python3 tools/bench_neon_cadence.py --url http://192.168.50.102 \
      --seconds 30 --settle 8 --spin on --dyno
Each arm is a PUT of theme/layout/spin followed by a settle and a sample
window; the board's initial theme/layout/spin are restored afterwards. This is
the harness for the "locked 60" neon re-run: run with the production display
config (regionDBuf/teSync as persisted) untouched.
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from urllib.request import Request, urlopen

BASE = "http://192.168.50.102"


def api_get(base: str, path: str) -> dict:
    with urlopen(f"{base.rstrip('/')}{path}", timeout=5) as resp:
        return json.load(resp)


def api_put(base: str, path: str, payload: dict) -> dict:
    req = Request(
        f"{base.rstrip('/')}{path}",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"},
        method="PUT",
    )
    with urlopen(req, timeout=5) as resp:
        return json.load(resp)


def run_arm(base: str, theme: str, layout: int | None, spin: bool | None,
            seconds: float, settle: float) -> dict:
    print(f"arm theme={theme} layout={layout} spin={spin} ...", file=sys.stderr)
    if theme:
        api_put(base, "/api/v1/themes/active", {"id": theme})
    cfg: dict = {}
    if layout is not None:
        cfg["neonLayout"] = layout
    if spin is not None:
        cfg["neonMarqueeSpin"] = spin
    if cfg:
        applied = api_put(base, "/api/v1/themes/config", cfg)
        for key, val in cfg.items():
            assert applied.get(key) == val, f"{key} did not take effect: {applied}"
    time.sleep(settle)

    samples: list[dict] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        d = api_get(base, "/api/v1/state")["display"]
        if d.get("renderFps"):
            samples.append(d)
        time.sleep(1.02)

    if not samples:
        raise SystemExit("no samples collected")

    fps = [s["renderFps"] for s in samples]
    worst = [s["worstRenderUs"] for s in samples]
    ob = [s["framesOverBudget"] for s in samples]
    te = {k: sum(s[k] for s in samples) for k in ("teSkips", "teWaits", "teTimeouts")}
    result = {
        "theme": theme,
        "layout": layout,
        "spin": spin,
        "n": len(samples),
        "fps_min": min(fps),
        "fps_median": statistics.median(fps),
        "worstUs_median": statistics.median(worst),
        "worstUs_max": max(worst),
        "ob_s_median": statistics.median(ob),
        "teSkips": te["teSkips"],
        "teWaits": te["teWaits"],
        "teTimeouts": te["teTimeouts"],
    }
    gate = "PASS" if result["fps_median"] >= 60 else "FAIL"
    print(
        f"{theme:12s} layout={result['layout']} spin={result['spin']} "
        f"fps {result['fps_min']}/{result['fps_median']} "
        f"worstUs {result['worstUs_median']}/{result['worstUs_max']} "
        f"ob/s {result['ob_s_median']:.1f} "
        f"teS/W/T {result['teSkips']}/{result['teWaits']}/{result['teTimeouts']} "
        f"n={result['n']} [{gate}]"
    )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=BASE)
    parser.add_argument("--seconds", type=float, default=30.0)
    parser.add_argument("--settle", type=float, default=8.0)
    parser.add_argument("--spin", choices=("on", "off", "keep"), default="keep",
                        help="marquee spin state for neon arms (default: keep)")
    parser.add_argument("--dyno", action="store_true",
                        help="also run the dyno-cell reference arm first")
    parser.add_argument("--no-marquee", action="store_true",
                        help="skip the marquee arm (layout 2)")
    parser.add_argument("--out", default="neon_cadence_results.json")
    args = parser.parse_args()

    spin = None if args.spin == "keep" else (args.spin == "on")
    start = api_get(args.url, "/api/v1/themes")
    start_state = {
        "theme": start["activeThemeId"],
        "layout": start.get("neonLayout"),
        "spin": start.get("neonMarqueeSpin"),
    }
    print(f"start state: {start_state}", file=sys.stderr)

    results: list[dict] = []
    if args.dyno:
        results.append(run_arm(args.url, "dyno-cell", None, None, args.seconds, args.settle))
    for layout in (0, 1, 2):
        if args.no_marquee and layout == 2:
            continue
        results.append(run_arm(args.url, "neon", layout, spin, args.seconds, args.settle))

    api_put(args.url, "/api/v1/themes/active", {"id": start_state["theme"]})
    restore: dict = {}
    if start_state["layout"] is not None:
        restore["neonLayout"] = start_state["layout"]
    if start_state["spin"] is not None:
        restore["neonMarqueeSpin"] = start_state["spin"]
    if restore:
        api_put(args.url, "/api/v1/themes/config", restore)
    print(f"restored theme={start_state['theme']} layout={start_state['layout']} "
          f"spin={start_state['spin']}", file=sys.stderr)

    with open(args.out, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nresults written to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
