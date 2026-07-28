#!/usr/bin/env python3
"""Interleaved A/B harness for the regionDBuf worst-case-latency fix.

Reboots the board between every run (via POST /api/v1/restart), waits for it
to come back on HTTP, sets the requested regionDBuf state, lets vault-tec/demo
settle, then samples /api/v1/state at ~4 Hz for the sample window and reports
renderFps / renderGapP50Us / worstRenderUs / framesOverBudget / teWaits /
teTimeouts / teSkips.

Committed so the numbers it prints are reproducible, per the project's own
method rule (see the "measurement nobody can re-run is not evidence" ledger
row): run this yourself against the board rather than trusting a pasted
number. Assumes the board is already on vault-tec with demo mode and teSync
on (this only toggles regionDBuf); set those first via the dashboard or
PUT /api/v1/themes/config if starting from a different state.
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
from urllib.request import urlopen, Request


def api_get(base: str, path: str) -> dict:
    with urlopen(f"{base}{path}", timeout=5) as r:
        return json.load(r)


def api_put(base: str, path: str, body: dict) -> dict:
    data = json.dumps(body).encode()
    req = Request(f"{base}{path}", data=data, method="PUT",
                   headers={"Content-Type": "application/json"})
    with urlopen(req, timeout=5) as r:
        return json.load(r)


def api_post(base: str, path: str) -> None:
    req = Request(f"{base}{path}", data=b"", method="POST")
    try:
        urlopen(req, timeout=3)
    except Exception:
        pass  # device reboots before it can answer; expected


def wait_online(base: str, timeout_s: float = 30.0) -> None:
    deadline = time.monotonic() + timeout_s
    last_err = None
    while time.monotonic() < deadline:
        try:
            api_get(base, "/api/v1/state")
            return
        except Exception as e:  # noqa: BLE001
            last_err = e
            time.sleep(0.3)
    raise SystemExit(f"device did not come back online: {last_err}")


def run_once(base: str, region_dbuf: bool, settle_s: float, sample_s: float,
             pixel_shift: bool = False) -> dict:
    print("  rebooting...", file=sys.stderr)
    api_post(base, "/api/v1/restart")
    time.sleep(2.0)  # give the device time to actually drop off before polling
    wait_online(base)
    print(f"  online, setting regionDBuf={region_dbuf} pixelShift={pixel_shift}", file=sys.stderr)
    # pixelShift forces an occasional full-panel repaint (anti-burn-in) that is
    # unrelated to the needle-tearing question this harness targets and would
    # otherwise show up as an unpredictable worstRenderUs spike in either arm.
    # Controlled off by default so the comparison isolates regionDBuf.
    api_put(base, "/api/v1/themes/config", {"regionDBuf": region_dbuf, "pixelShift": pixel_shift})
    print(f"  settling {settle_s}s...", file=sys.stderr)
    time.sleep(settle_s)

    # /api/v1/state's display.* fields are counts/values for the LAST
    # COMPLETED 1-second window (display_metrics_event_cb resets them every
    # 1,000,000 us). Polling faster than 1 Hz re-reads the same window
    # multiple times, which is fine for *_median (duplicates don't move a
    # median) but silently inflates any *sum* of samples by the duplication
    # factor - caught by a first version of this harness reporting an
    # arithmetically impossible ~75 teSkips/s against a ~56 renderFps. Poll at
    # slightly over 1 Hz so almost every sample lands in a fresh window, and
    # dedupe consecutive identical (renderFps, worstRenderUs) pairs before
    # summing as a second line of defence.
    samples = []
    deadline = time.monotonic() + sample_s
    while time.monotonic() < deadline:
        st = api_get(base, "/api/v1/state")
        d = st["display"]
        if d.get("renderFps"):
            samples.append(d)
        time.sleep(1.05)

    if not samples:
        raise SystemExit("no samples collected")

    deduped = []
    prev_key = None
    for s in samples:
        key = (s["renderFps"], s["worstRenderUs"], s["teWaits"], s["teSkips"])
        if key != prev_key:
            deduped.append(s)
        prev_key = key

    def col(rows, key):
        return [r[key] for r in rows]

    render_fps = col(samples, "renderFps")
    gap_p50 = col(samples, "renderGapP50Us")
    worst = col(samples, "worstRenderUs")
    over_budget_dedup = col(deduped, "framesOverBudget")
    te_waits_dedup = col(deduped, "teWaits")
    te_timeouts_dedup = col(deduped, "teTimeouts")
    te_skips_dedup = col(deduped, "teSkips")

    return {
        "n": len(samples),
        "n_distinct_windows": len(deduped),
        "renderFps_min": min(render_fps),
        "renderFps_median": statistics.median(render_fps),
        "gapP50Us_median": statistics.median(gap_p50),
        "worstRenderUs_median": statistics.median(worst),
        "worstRenderUs_max": max(worst),
        "framesOverBudget_median": statistics.median(over_budget_dedup),
        "framesOverBudget_sum_deduped": sum(over_budget_dedup),
        "teWaits_sum_deduped": sum(te_waits_dedup),
        "teTimeouts_sum_deduped": sum(te_timeouts_dedup),
        "teSkips_sum_deduped": sum(te_skips_dedup),
        "raw_worstRenderUs": worst,
        "raw_framesOverBudget_deduped": over_budget_dedup,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--url", default="http://192.168.50.102")
    ap.add_argument("--settle", type=float, default=14.0, help="seconds to let the face settle before sampling")
    ap.add_argument("--sample", type=float, default=20.0, help="seconds to sample /state for")
    ap.add_argument("--rounds", type=int, default=3, help="A/B rounds, interleaved OFF/ON/OFF/ON/...")
    ap.add_argument("--pixel-shift", action="store_true",
                     help="leave pixelShift on during the run instead of controlling it off")
    ap.add_argument("--out", default="ab_region_dbuf_results.json")
    args = ap.parse_args()

    base = args.url.rstrip("/")
    results = {"OFF": [], "ON": []}
    for rnd in range(args.rounds):
        for label, val in (("OFF", False), ("ON", True)):
            print(f"=== round {rnd + 1}/{args.rounds}: regionDBuf {label} ===", file=sys.stderr)
            r = run_once(base, val, args.settle, args.sample, pixel_shift=args.pixel_shift)
            results[label].append(r)
            print(json.dumps(r, indent=2))

    print("\n=== SUMMARY ===")
    header = (f"{'':>5} {'renderFps':>10} {'gapP50':>10} {'worstMed':>10} "
              f"{'worstMax':>10} {'ovrBudget':>10} {'teWaits':>8} {'teTO':>6} {'teSkips':>8}")
    print(header)
    for label in ("OFF", "ON"):
        for r in results[label]:
            print(f"{label:>5} {r['renderFps_median']:>10} {r['gapP50Us_median'] / 1000:>9.1f}m "
                  f"{r['worstRenderUs_median'] / 1000:>9.1f}m {r['worstRenderUs_max'] / 1000:>9.1f}m "
                  f"{r['framesOverBudget_median']:>10} {r['teWaits_sum_deduped']:>8} "
                  f"{r['teTimeouts_sum_deduped']:>6} {r['teSkips_sum_deduped']:>8}")

    with open(args.out, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nRaw results written to {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
