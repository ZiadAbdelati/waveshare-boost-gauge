#!/usr/bin/env python3
"""Single-arm state capture against the live board.

Sets an optional theme/layout/spin/regionDBuf/teSync, settles, then samples the
once-a-second display telemetry for N seconds and prints min/median/max for the
pacing metrics. Writes a JSON line to --out. Leaves the board at the arm config
(no restore) so multiple arms can be chained and compared.

Usage:
    python tools/capture_state.py --name marquee-rdbuf-off --seconds 30 --settle 6 \
        --theme neon --layout 2 --spin on --region-dbuf off
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.request

BASE = "http://192.168.50.102"
METRICS = (
    "renderFps", "framesOverBudget", "worstRenderUs",
    "renderGapP50Us", "renderGapMaxUs", "pixelsPerSecond",
    "teWaits", "teTimeouts", "teSkips",
)


def get(base: str, path: str) -> dict:
    with urllib.request.urlopen(f"{base.rstrip('/')}{path}", timeout=5) as r:
        return json.load(r)


def put(base: str, path: str, payload: dict) -> dict:
    req = urllib.request.Request(
        f"{base.rstrip('/')}{path}", data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json"}, method="PUT")
    with urllib.request.urlopen(req, timeout=6) as r:
        return json.load(r)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default=BASE)
    ap.add_argument("--name", default="arm")
    ap.add_argument("--seconds", type=float, default=30.0)
    ap.add_argument("--settle", type=float, default=6.0)
    ap.add_argument("--theme", default=None)
    ap.add_argument("--layout", type=int, default=None)
    ap.add_argument("--spin", choices=("on", "off"), default=None)
    ap.add_argument("--region-dbuf", choices=("on", "off"), default=None)
    ap.add_argument("--te-sync", choices=("on", "off"), default=None)
    ap.add_argument("--out", default="capture_state.jsonl")
    args = ap.parse_args()

    base = args.url.rstrip("/")
    if args.theme:
        put(base, "/api/v1/themes/active", {"id": args.theme})
    cfg: dict = {}
    if args.layout is not None:
        cfg["neonLayout"] = args.layout
    if args.spin is not None:
        cfg["neonMarqueeSpin"] = args.spin == "on"
    if args.region_dbuf is not None:
        cfg["regionDBuf"] = args.region_dbuf == "on"
    if args.te_sync is not None:
        cfg["teSync"] = args.te_sync == "on"
    if cfg:
        put(base, "/api/v1/themes/config", cfg)
    time.sleep(args.settle)

    samples: list[dict] = []
    end = time.monotonic() + args.seconds
    while time.monotonic() < end:
        d = get(base, "/api/v1/state")["display"]
        if d.get("renderFps"):
            samples.append(d)
        time.sleep(0.25)

    if not samples:
        print(f"{args.name}: no samples", file=sys.stderr)
        return 1

    # Collapse the 4 Hz polling to distinct 1 s buckets.
    seen: dict[tuple, dict] = {}
    for s in samples:
        key = (s["renderFps"], s["worstRenderUs"], s["framesOverBudget"],
               s["teWaits"], s["teSkips"])
        seen[key] = s
    buckets = list(seen.values())

    fps = sorted(s["renderFps"] for s in buckets)
    worst = sorted(s["worstRenderUs"] for s in buckets)
    ob = sorted(s["framesOverBudget"] for s in buckets)
    te_w = sum(s["teWaits"] for s in buckets)
    te_s = sum(s["teSkips"] for s in buckets)
    te_t = sum(s["teTimeouts"] for s in buckets)
    pps = sorted(s["pixelsPerSecond"] for s in buckets)

    def med(xs):
        return statistics.median(xs)

    n = len(buckets)
    line = {
        "name": args.name, "n": n,
        "fps_min": fps[0], "fps_median": med(fps), "fps_max": fps[-1],
        "worstUs_median": med(worst), "worstUs_max": worst[-1],
        "ob_s_median": med(ob), "ob_s_max": ob[-1],
        "teWaits_per_s": te_w / n, "teSkips_per_s": te_s / n, "teTimeouts": te_t,
        "pps_median": med(pps),
        "theme": args.theme, "layout": args.layout, "spin": args.spin,
        "regionDBuf": args.region_dbuf, "teSync": args.te_sync,
    }
    with open(args.out, "a", encoding="utf-8") as f:
        f.write(json.dumps(line) + "\n")
    print(json.dumps(line, indent=1))
    print(f"fps samples: {fps}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
