#!/usr/bin/env python3
"""Capture per-second FPS + zone-flip detail on a live board (spin ON).

Enables neonMarqueeSpin, polls /api/v1/state at ~4 Hz for N seconds, then
prints the lowest-FPS seconds with the zone transitions and over-budget
frame counts that occurred inside them.

Usage: python3 tools/corr_zone_flip_fps.py --url http://<ip> --seconds 180 --worst 10
"""
import argparse
import json
import time
import urllib.request
from collections import defaultdict

def get(url):
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.load(r)

def put(url, payload):
    req = urllib.request.Request(url, data=json.dumps(payload).encode(),
                                 headers={"Content-Type": "application/json"},
                                 method="PUT")
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.load(r)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://192.168.1.100")
    ap.add_argument("--seconds", type=int, default=180)
    ap.add_argument("--worst", type=int, default=10)
    ap.add_argument("--no-spin", action="store_true", help="leave spin as-is")
    args = ap.parse_args()

    # Match the A/B conditions: spin ON, demo, pixelShift off.
    if not args.no_spin:
        th = get(args.url + "/api/v1/themes")
        cur = None
        if isinstance(th, dict):
            cur = th.get("neonMarqueeSpin")
        print(f"spin was {cur}; enabling")
        put(args.url + "/api/v1/themes/config", {"neonMarqueeSpin": True})
        time.sleep(1.0)

    samples = []   # (wall, psi, zone, fps, ob, worstUs)
    t0 = time.time()
    while time.time() - t0 < args.seconds:
        try:
            st = get(args.url + "/api/v1/state")
            d = st.get("display", {})
            samples.append((int(time.time()), st.get("psi", 0.0),
                            st.get("zone", ""), d.get("renderFps", 0),
                            d.get("framesOverBudget", 0), d.get("worstRenderUs", 0)))
        except Exception as e:
            print("poll err:", e)
        time.sleep(0.2)

    by_sec = defaultdict(list)
    for wall, psi, zone, fps, ob, wu in samples:
        by_sec[wall].append((psi, zone, fps, ob, wu))

    rows = []
    prev_zone = None
    for wall in sorted(by_sec):
        rows_in = by_sec[wall]
        zones = {r[1] for r in rows_in if r[1]}
        flipped = prev_zone is not None and prev_zone not in zones or len(zones) > 1
        nflips = 0
        pz = prev_zone
        for r in rows_in:
            if pz is not None and r[1] and r[1] != pz:
                nflips += 1
            if r[1]:
                pz = r[1]
        fps = max(r[2] for r in rows_in)
        ob = max(r[3] for r in rows_in)
        wu = max(r[4] for r in rows_in)
        rows.append((wall, fps, ob, wu, nflips, zones, prev_zone))
        prev_zone = rows_in[-1][1]

    all_fps = sorted(r[1] for r in rows)
    n = len(all_fps)
    print(f"== {len(samples)} samples, {len(rows)} seconds, spin ON ==")
    print(f"FPS: min={all_fps[0]} med={all_fps[n//2]} mean={sum(all_fps)/n:.1f} max={all_fps[-1]}")
    print(f"flip seconds: {sum(1 for r in rows if r[4] > 0)}, "
          f"non-flip: {sum(1 for r in rows if r[4] == 0)}")
    print(f"worst ob/s window: {max(r[2] for r in rows)}")

    worst = sorted(rows, key=lambda r: r[1])[:args.worst]
    print("\n-- worst seconds (fps | ob/s | worstRenderUs | flips | zones | prev) --")
    for wall, fps, ob, wu, nfl, zones, pz in worst:
        print(f"  t-{wall - rows[0][0]:4d}s fps={fps:2d} ob={ob:2d} worst={wu:6d}us "
              f"flips={nfl} zones={sorted(zones)} prev={pz}")

if __name__ == "__main__":
    main()
