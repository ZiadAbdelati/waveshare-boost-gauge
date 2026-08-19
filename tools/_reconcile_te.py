#!/usr/bin/env python3
"""Reconcile renderFps vs teWaits/teSkips per metric window on the live board.

Polls /api/v1/state rapidly and groups samples by the 1 s metric window index
(derived from uptimeMs). Each window's counters are reset once per second, so
the max value observed for a counter inside a window is that window's total.

Usage: python tools/_reconcile_te.py --seconds 40
"""
from __future__ import annotations

import argparse
import json
import time
import urllib.request
from collections import defaultdict


def get(url: str) -> dict:
    with urllib.request.urlopen(url, timeout=5) as r:
        return json.load(r)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", default="http://192.168.1.100")
    ap.add_argument("--seconds", type=float, default=40.0)
    ap.add_argument("--pause", type=float, default=0.12)
    args = ap.parse_args()

    base = args.url.rstrip("/")
    windows: dict[int, dict] = defaultdict(lambda: {"fps": 0, "waits": 0, "skips": 0,
                                                    "to": 0, "flushes": 0, "pps": 0,
                                                    "ob": 0, "worst": 0, "psi": 0.0})
    end = time.monotonic() + args.seconds
    n = 0
    while time.monotonic() < end:
        try:
            st = get(base + "/api/v1/state")
        except Exception as e:
            print("poll err:", e)
            time.sleep(0.2)
            continue
        d = st.get("display", {})
        w = int(st.get("uptimeMs", 0) / 1000)
        c = windows[w]
        c["fps"] = max(c["fps"], d.get("renderFps", 0))
        c["waits"] = max(c["waits"], d.get("teWaits", 0))
        c["skips"] = max(c["skips"], d.get("teSkips", 0))
        c["to"] = max(c["to"], d.get("teTimeouts", 0))
        c["flushes"] = max(c["flushes"], d.get("flushesPerSecond", 0))
        c["pps"] = max(c["pps"], d.get("pixelsPerSecond", 0))
        c["ob"] = max(c["ob"], d.get("framesOverBudget", 0))
        c["worst"] = max(c["worst"], d.get("worstRenderUs", 0))
        c["psi"] = st.get("psi", 0.0)
        n += 1
        time.sleep(args.pause)

    print(f"{n} polls, {len(windows)} distinct windows")
    print(f"{'win':>5} {'fps':>4} {'waits':>5} {'skips':>5} {'w+s':>5} {'to':>2} "
          f"{'flsh':>5} {'pps':>8} {'ob':>3} {'worst':>6} {'psi':>6}")
    total_w = total_s = total_f = 0
    for w in sorted(windows):
        c = windows[w]
        total_w += c["waits"]; total_s += c["skips"]; total_f += c["fps"]
        print(f"{w:5d} {c['fps']:4d} {c['waits']:5d} {c['skips']:5d} {c['waits']+c['skips']:5d} "
              f"{c['to']:2d} {c['flushes']:5d} {c['pps']:8d} {c['ob']:3d} {c['worst']:6d} {c['psi']:6.2f}")
    nw = len(windows)
    # Accounting (2026-08-09): teWaits is incremented UNCONDITIONALLY on every
    # te_wait_for_region_spans() call, i.e. one per writeback/RENDER_READY-with-
    # data pass, so teWaits == renderFps per window by construction. teSkips is
    # only incremented on the subset of those calls that skipped the wait, so
    # skips is INSIDE waits - "waits+skips" double-counts and must never be
    # reported as the TE-event rate. The TE-event rate is teWaits alone.
    skip_pct = 100.0 * total_s / max(1, total_w)
    print(f"\nsums: fps={total_f} ({total_f/nw:.1f}/s)  teWaits={total_w} "
          f"({total_w/nw:.1f}/s)  teSkips={total_s} ({skip_pct:.1f}% of waits) "
          f"teTimeouts={sum(c['to'] for c in windows.values())}  "
          f"waits/fps={total_w/max(1,total_f):.3f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
