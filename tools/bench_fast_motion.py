#!/usr/bin/env python3
"""Isolate and report display cadence specifically during FAST needle motion
on vault-tec, as opposed to the demo sweep's aggregate (which mixes fast and
slow segments and hides exactly the regime this measures).

Two independent methods, both against the live board:

1. `sweep` (primary): flips the firmware into `demoFastSweep` mode
   (main/boost_sim.c) - a constant-slew triangle wave sustained across the
   whole PSI_MIN..PSI_MAX range at 9.789 psi/s, the SAME peak trend slew rate
   the organic sine-envelope demo produces at its own zero-crossings (derived
   below in `trend_peak_slew_psi_per_s`, not invented). Because the whole
   sampling window is then "fast", the normal 1 Hz display.* aggregation
   IS the fast-motion measurement - no binning needed.

2. `organic` (cross-check): leaves the normal demo waveform running, polls
   /api/v1/state as fast as the board answers, and empirically measures
   |dpsi/dt| from consecutive samples (device-clock deltas, smoothed over a
   window to reject the sim's own high-frequency flutter terms), then bins
   the once-a-second display.* readings by the mean sweep speed observed
   during that same wall-clock second. This validates that `sweep`'s chosen
   velocity is representative of what the organic demo actually produces at
   its fastest, not a faster synthetic case.

Usage:
    python tools/bench_fast_motion.py sweep    --url http://192.168.50.102
    python tools/bench_fast_motion.py organic  --url http://192.168.50.102
    python tools/bench_fast_motion.py constants   # just print the derived numbers

Committed so these numbers are reproducible - see the project's own method
rule ("a measurement nobody can re-run is not evidence").
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import time
from pathlib import Path
from bench_common import DEFAULT_URL, api_get, api_put, api_post, wait_online

# ---------------------------------------------------------------------------
# Constants mirrored from main/boost_sim.c and main/boost_gauge.c. Keep these
# in sync by hand; there is no shared source of truth between firmware and
# this script (see the ledger's own "documented a setting that was never in
# effect" row - the fix there was making the firmware log/report the active
# value, which is why this script reads demoFastSweep back from the device
# rather than assuming the PUT took effect).
# ---------------------------------------------------------------------------
PSI_MIN = -14.5
PSI_MAX = 9.6

# Vault face geometry (VAULT_A0/A1, DEFAULT_ZERO_ANGLE/PSI_MIN/PSI_MAX,
# ARC_START/RANGE) from main/boost_gauge.c, used only to convert measured
# |dpsi/dt| into an approximate needle angular velocity for readability.
VAULT_A0 = 152.0
VAULT_A1 = 388.0
DEFAULT_ZERO_ANGLE = 236.25
ARC_START = 135.0
ARC_RANGE = 270.0
DEFAULT_PSI_MIN = -15.0
DEFAULT_PSI_MAX = 10.0
_ZERO_AT = VAULT_A0 + ((DEFAULT_ZERO_ANGLE - ARC_START) / ARC_RANGE) * (VAULT_A1 - VAULT_A0)
VAULT_VAC_SLOPE_DEG_PER_PSI = (_ZERO_AT - VAULT_A0) / (0.0 - DEFAULT_PSI_MIN)   # ~5.9
VAULT_BOOST_SLOPE_DEG_PER_PSI = (VAULT_A1 - _ZERO_AT) / DEFAULT_PSI_MAX          # ~14.75


def psi_trend(t: float) -> float:
    """Exact port of boost_sim_tick()'s non-fast-sweep branch, noise terms
    excluded (those are ~2.75 Hz / ~4.95 Hz flutter that doesn't move the
    needle visibly and would dominate a naive point derivative)."""
    phase = t * (2.0 * math.pi / 7.5)
    envelope = 0.55 + 0.45 * math.sin(phase * 0.5 + 0.4)
    norm = 0.5 + 0.5 * math.sin(phase)
    norm = norm * envelope
    norm = norm * 0.92 + 0.08
    return PSI_MIN + (PSI_MAX - PSI_MIN) * norm


def trend_peak_slew_psi_per_s(cycles: float = 3.0, dt: float = 1e-4) -> tuple[float, float]:
    """Numerically differentiate psi_trend over `cycles` full envelope
    periods (envelope period is 2*7.5=15s) and return (peak |dpsi/dt|, the
    psi value at which it occurs)."""
    period = 15.0
    n = int(cycles * period / dt)
    best_v = 0.0
    best_p = 0.0
    for i in range(n):
        t = i * dt
        v = (psi_trend(t + dt) - psi_trend(t - dt)) / (2 * dt)
        if abs(v) > abs(best_v):
            best_v = v
            best_p = psi_trend(t)
    return best_v, best_p


def angle_slope_deg_per_psi(psi: float) -> float:
    return VAULT_BOOST_SLOPE_DEG_PER_PSI if psi >= 0 else VAULT_VAC_SLOPE_DEG_PER_PSI


def cmd_constants(_args) -> int:
    peak_v, at_psi = trend_peak_slew_psi_per_s()
    period = 2.0 * (PSI_MAX - PSI_MIN) / abs(peak_v)
    print(f"organic demo peak TREND |dpsi/dt|: {abs(peak_v):.3f} psi/s (near psi={at_psi:.3f})")
    print(f"  -> vault angular velocity there (vacuum slope {VAULT_VAC_SLOPE_DEG_PER_PSI:.2f} deg/psi): "
          f"{abs(peak_v) * VAULT_VAC_SLOPE_DEG_PER_PSI:.1f} deg/s")
    print(f"  -> vault angular velocity there (boost slope {VAULT_BOOST_SLOPE_DEG_PER_PSI:.2f} deg/psi): "
          f"{abs(peak_v) * VAULT_BOOST_SLOPE_DEG_PER_PSI:.1f} deg/s")
    print(f"fast-sweep triangle period for this slew: {period:.4f} s "
          f"(FAST_SWEEP_PERIOD_S in main/boost_sim.c should match)")
    return 0


def cmd_sweep(args) -> int:
    base = args.url.rstrip("/")
    print("rebooting...", file=sys.stderr)
    api_post(base, "/api/v1/restart")
    time.sleep(2.0)
    wait_online(base)
    print(f"online, setting demoMode=true demoFastSweep=true regionDBuf={args.region_dbuf} "
          f"teSync=true teScanline={args.te_scanline} pixelShift=false", file=sys.stderr)
    theme = args.theme
    if theme:
        api_put(base, "/api/v1/themes/active", {"id": theme})
    if args.layout is not None:
        lcfg = api_put(base, "/api/v1/themes/config", {"neonLayout": args.layout})
        assert lcfg.get("neonLayout") == args.layout, f"neonLayout did not take effect: {lcfg}"
    cfg = api_put(base, "/api/v1/themes/config", {
        "demoMode": True,
        "demoFastSweep": True,
        "regionDBuf": args.region_dbuf,
        "teSync": True,
        "teScanline": args.te_scanline,
        "pixelShift": False,
    })
    assert cfg.get("demoFastSweep") is True, f"demoFastSweep did not take effect: {cfg}"
    assert cfg.get("regionDBuf") == args.region_dbuf, f"regionDBuf did not take effect: {cfg}"
    assert cfg.get("teScanline") == args.te_scanline, f"teScanline did not take effect: {cfg}"
    print(f"settling {args.settle}s...", file=sys.stderr)
    time.sleep(args.settle)

    samples = []
    deadline = time.monotonic() + args.sample
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
        key = (s["renderFps"], s.get("gaugeDemandPerSecond"),
               s["worstRenderUs"], s["teWaits"], s["teSkips"])
        if key != prev_key:
            deduped.append(s)
        prev_key = key

    def col(rows, key):
        return [r[key] for r in rows]

    result = {
        "n": len(samples),
        "n_distinct_windows": len(deduped),
        "regionDBuf": args.region_dbuf,
        "teScanline": args.te_scanline,
        "theme": args.theme,
        "layout": args.layout,
        "renderFps_min": min(col(samples, "renderFps")),
        "renderFps_median": statistics.median(col(samples, "renderFps")),
        "renderGapP50Us_median": statistics.median(col(samples, "renderGapP50Us")),
        "renderGapMaxUs_max": max(col(samples, "renderGapMaxUs")),
        "worstRenderUs_median": statistics.median(col(samples, "worstRenderUs")),
        "worstRenderUs_max": max(col(samples, "worstRenderUs")),
        "framesOverBudget_median": statistics.median(col(deduped, "framesOverBudget")),
        "framesOverBudget_sum_deduped": sum(col(deduped, "framesOverBudget")),
        "teSkips_sum_deduped": sum(col(deduped, "teSkips")),
        "teWaits_sum_deduped": sum(col(deduped, "teWaits")),
        "teTimeouts_sum_deduped": sum(col(deduped, "teTimeouts")),
        "teScanlineWaits_sum_deduped": sum(r.get("teScanlineWaits", 0) for r in deduped),
        "pixelsPerSecond_median": statistics.median(col(samples, "pixelsPerSecond")),
        "flushesPerSecond_median": statistics.median(col(samples, "flushesPerSecond")),
        "tePeriodUs": samples[-1].get("tePeriodUs"),
        "raw": samples,
    }
    demand_rows = [(r["renderFps"], r["gaugeDemandPerSecond"])
                   for r in samples if r.get("gaugeDemandPerSecond", 0) > 0]
    if demand_rows:
        result["gaugeDemandPerSecond_median"] = statistics.median(
            demand for _, demand in demand_rows)
        result["demandCoverage_median"] = statistics.median(
            min(1.0, fps / demand) for fps, demand in demand_rows)
        result["demandShortfallPerSecond_median"] = statistics.median(
            max(0, demand - fps) for fps, demand in demand_rows)
    print(json.dumps({k: v for k, v in result.items() if k != "raw"}, indent=2))

    # Leave demoFastSweep off afterwards - it is diagnostic-only and would
    # otherwise survive until the next reboot.
    api_put(base, "/api/v1/themes/config", {"demoFastSweep": False})

    with open(args.out, "w") as f:
        json.dump(result, f, indent=2)
    print(f"\nRaw results written to {args.out}")
    return 0


def display_window_key(display: dict) -> tuple:
    """Identify one published display-metric window without a firmware ID."""
    return tuple(display.get(k) for k in (
        "renderFps", "renderGapP50Us", "renderGapMaxUs", "worstRenderUs",
        "framesOverBudget", "gaugeDemandPerSecond", "teWaits", "teSkips", "teTimeouts",
        "teScanlineWaits", "pixelsPerSecond", "flushesPerSecond",
    ))


def analyze_crossings(rows: list[dict], threshold: float,
                      crossings_per_direction: int) -> dict:
    """Pair each threshold crossing with the first subsequently published
    display window.  This avoids repeatedly counting the rolling 1 Hz metrics
    while the HTTP poller is faster than their producer."""
    pending: list[dict] = []
    events: list[dict] = []
    accepted = {"boost_to_overboost": 0, "overboost_to_boost": 0}
    previous = None

    for row in rows:
        psi = float(row["psi"])
        uptime_ms = int(row["uptimeMs"])
        display = row["display"]
        window_key = display_window_key(display)

        still_pending = []
        for crossing in pending:
            if window_key == crossing["window_key"]:
                still_pending.append(crossing)
                continue
            direction = crossing["direction"]
            if accepted[direction] < crossings_per_direction:
                event = {k: v for k, v in crossing.items() if k != "window_key"}
                event["metricUptimeMs"] = uptime_ms
                event["metricLatencyMs"] = uptime_ms - crossing["crossingUptimeMs"]
                event["display"] = display
                events.append(event)
                accepted[direction] += 1
        pending = still_pending

        if previous is not None:
            previous_psi = float(previous["psi"])
            direction = None
            if previous_psi < threshold <= psi:
                direction = "boost_to_overboost"
            elif previous_psi >= threshold > psi:
                direction = "overboost_to_boost"
            if direction and accepted[direction] < crossings_per_direction and not any(
                    p["direction"] == direction for p in pending):
                pending.append({
                    "direction": direction,
                    "crossingUptimeMs": uptime_ms,
                    "crossingPsi": psi,
                    "previousPsi": previous_psi,
                    "window_key": window_key,
                })
        previous = row

    summaries = {}
    metric_names = (
        "renderFps", "renderGapP50Us", "renderGapMaxUs", "worstRenderUs",
        "framesOverBudget", "teWaits", "teSkips", "teTimeouts",
        "teScanlineWaits", "pixelsPerSecond", "flushesPerSecond",
    )
    for direction in accepted:
        group = [event for event in events if event["direction"] == direction]
        summary = {"n": len(group)}
        if group:
            summary["metricLatencyMs_median"] = statistics.median(
                event["metricLatencyMs"] for event in group)
            for name in metric_names:
                values = [event["display"].get(name, 0) for event in group]
                summary[f"{name}_min"] = min(values)
                summary[f"{name}_median"] = statistics.median(values)
                summary[f"{name}_max"] = max(values)
        summaries[direction] = summary
    return {
        "thresholdPsi": threshold,
        "requestedPerDirection": crossings_per_direction,
        "complete": all(accepted[d] >= crossings_per_direction for d in accepted),
        "summaries": summaries,
        "events": events,
    }


def cmd_crossings(args) -> int:
    if args.count < 1:
        raise SystemExit("--count must be at least 1")
    if args.poll <= 0 or args.timeout <= 0 or args.settle < 0:
        raise SystemExit("--poll/--timeout must be positive and --settle non-negative")
    if args.input:
        capture = json.loads(args.input.read_text(encoding="utf-8-sig"))
        rows = capture["rows"] if isinstance(capture, dict) else capture
        threshold = args.threshold if args.threshold is not None else capture.get("thresholdPsi")
        if threshold is None:
            raise SystemExit("offline analysis needs --threshold or thresholdPsi in the input")
        result = analyze_crossings(rows, float(threshold), args.count)
        print(json.dumps(result, indent=2))
        return 0 if result["complete"] else 2

    base = args.url.rstrip("/")
    initial = api_get(base, "/api/v1/themes")
    config = api_get(base, "/api/v1/config")
    threshold = float(args.threshold if args.threshold is not None else config["psiOverboost"])
    restore = {key: initial[key] for key in (
        "demoMode", "demoFastSweep", "regionDBuf", "teSync", "teScanline",
        "pixelShift", "neonLayout",
    )}
    rows = []
    try:
        if not args.no_reboot:
            print("rebooting...", file=sys.stderr)
            api_post(base, "/api/v1/restart")
            time.sleep(2.0)
            wait_online(base)
        if args.theme:
            api_put(base, "/api/v1/themes/active", {"id": args.theme})
        if args.layout is not None:
            layout_cfg = api_put(base, "/api/v1/themes/config", {"neonLayout": args.layout})
            assert layout_cfg.get("neonLayout") == args.layout, layout_cfg
        configured = api_put(base, "/api/v1/themes/config", {
            "demoMode": True,
            "demoFastSweep": True,
            "regionDBuf": args.region_dbuf,
            "teSync": True,
            "teScanline": args.te_scanline,
            "pixelShift": False,
        })
        for key, expected in (("demoMode", True), ("demoFastSweep", True),
                              ("regionDBuf", args.region_dbuf), ("teSync", True),
                              ("teScanline", args.te_scanline), ("pixelShift", False)):
            assert configured.get(key) == expected, f"{key} did not take effect: {configured}"
        print(f"settling {args.settle}s, then collecting {args.count} crossings/direction "
              f"at {threshold:.3f} psi (timeout {args.timeout}s)...", file=sys.stderr)
        time.sleep(args.settle)
        deadline = time.monotonic() + args.timeout
        result = analyze_crossings(rows, threshold, args.count)
        while time.monotonic() < deadline and not result["complete"]:
            state = api_get(base, "/api/v1/state")
            rows.append({
                "uptimeMs": state["uptimeMs"],
                "psi": state["psi"],
                "display": state["display"],
            })
            result = analyze_crossings(rows, threshold, args.count)
            time.sleep(args.poll)
    finally:
        print("restoring initial board configuration", file=sys.stderr)
        wait_online(base)
        api_put(base, "/api/v1/themes/config", restore)
        api_put(base, "/api/v1/themes/active", {"id": initial["activeThemeId"]})

    output = {
        "thresholdPsi": threshold,
        "theme": args.theme,
        "layout": args.layout,
        "regionDBuf": args.region_dbuf,
        "teScanline": args.te_scanline,
        "rows": rows,
        "analysis": result,
    }
    args.out.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    print(f"raw capture and analysis written to {args.out}", file=sys.stderr)
    return 0 if result["complete"] else 2


def cmd_organic(args) -> int:
    base = args.url.rstrip("/")
    # pixelShift fires a full-screen repaint every pixelShiftSec (default 90s)
    # unrelated to the needle-tearing/fast-motion question this measures, and
    # a 90s-class run will almost certainly catch one mid-window if left on -
    # exactly the confound the project's own method rules call out ("I
    # personally reported a bogus 32ms vs 18ms result... because that repaint
    # landed inside some measurement stages and not others"). Controlled off
    # explicitly rather than trusted to already be off.
    cfg = api_put(base, "/api/v1/themes/config", {"pixelShift": False})
    assert cfg.get("pixelShift") is False, f"pixelShift did not turn off: {cfg}"
    print(f"polling {base}/api/v1/state for {args.duration}s "
          f"(organic demo waveform, no firmware changes, pixelShift forced off)...", file=sys.stderr)
    rows = []
    deadline = time.monotonic() + args.duration
    while time.monotonic() < deadline:
        try:
            st = api_get(base, "/api/v1/state")
        except Exception as e:  # noqa: BLE001
            print(f"  poll failed: {e}", file=sys.stderr)
            continue
        rows.append((time.monotonic(), st["uptimeMs"], st["psi"], st["display"]))
        # No sleep: poll as fast as the board answers over HTTP. Typical LAN
        # round trip here is tens of ms, giving a natural ~10-30 Hz sample rate.

    if len(rows) < 10:
        raise SystemExit("too few samples collected")

    # Smoothed derivative: compare samples ~WINDOW_S apart (not adjacent
    # polls) to average out the sim's 17.3/31.1 rad/s flutter terms
    # (periods ~0.36s / ~0.20s) while still resolving the envelope's
    # multi-second-scale speed changes.
    WINDOW_S = 0.35
    velocity_by_wall_sec: dict[int, list[float]] = {}
    j = 0
    for i in range(len(rows)):
        t_i, up_i, psi_i, _ = rows[i]
        while j < len(rows) and rows[j][1] - up_i < WINDOW_S * 1000:
            j += 1
        if j >= len(rows):
            break
        t_j, up_j, psi_j, _ = rows[j]
        dt_s = (up_j - up_i) / 1000.0
        if dt_s < WINDOW_S * 0.5:
            continue
        dpsi_dt = (psi_j - psi_i) / dt_s
        bucket = int(t_i)
        velocity_by_wall_sec.setdefault(bucket, []).append(abs(dpsi_dt))

    # Bucket the display.* readings (deduped by value change, like the
    # sweep/region-dbuf harnesses) by wall-clock second and pair with the
    # mean |dpsi/dt| observed in that same second.
    display_by_sec: dict[int, dict] = {}
    prev_key = None
    for t, up, psi, d in rows:
        key = (d["renderFps"], d.get("gaugeDemandPerSecond"),
               d["worstRenderUs"], d["teWaits"])
        if key == prev_key:
            continue
        prev_key = key
        display_by_sec[int(t)] = d

    paired = []
    for sec, d in display_by_sec.items():
        vs = velocity_by_wall_sec.get(sec)
        if not vs:
            continue
        mean_v = statistics.mean(vs)
        paired.append((mean_v, d))

    if len(paired) < 8:
        raise SystemExit(f"only {len(paired)} paired (velocity, display) samples; "
                          "run longer with --duration")

    paired.sort(key=lambda x: x[0])
    n = len(paired)
    slow = paired[: n // 4] or paired[:1]
    fast = paired[-(n // 4):] or paired[-1:]

    def summarize(label, group):
        fps = [d["renderFps"] for _, d in group]
        gap = [d["renderGapP50Us"] for _, d in group]
        worst = [d["worstRenderUs"] for _, d in group]
        over = [d["framesOverBudget"] for _, d in group]
        vs = [v for v, _ in group]
        print(f"\n=== {label} (n={len(group)}, mean|dpsi/dt| {statistics.mean(vs):.2f} psi/s, "
              f"range {min(vs):.2f}-{max(vs):.2f}) ===")
        print(f"  renderFps: min={min(fps)} median={statistics.median(fps)}")
        print(f"  renderGapP50Us: median={statistics.median(gap):.0f}")
        print(f"  worstRenderUs: median={statistics.median(worst):.0f} max={max(worst)}")
        print(f"  framesOverBudget: median={statistics.median(over):.1f}")
        demand = [(d["renderFps"], d["gaugeDemandPerSecond"])
                  for _, d in group if d.get("gaugeDemandPerSecond", 0) > 0]
        if demand:
            coverage = [min(1.0, fps / requested) for fps, requested in demand]
            shortfall = [max(0, requested - fps) for fps, requested in demand]
            print(f"  gaugeDemandPerSecond: median={statistics.median(d for _, d in demand):.1f}")
            print(f"  demandCoverage: median={statistics.median(coverage) * 100:.1f}%")
            print(f"  demandShortfallPerSecond: median={statistics.median(shortfall):.1f}")
        else:
            print("  demandCoverage: n/a (firmware does not expose gauge demand)")

    print(f"\ntotal polls: {len(rows)}, distinct display windows: {len(display_by_sec)}, "
          f"paired (velocity,window) seconds: {len(paired)}")
    summarize("SLOWEST quartile of seconds", slow)
    summarize("FASTEST quartile of seconds", fast)

    with open(args.out, "w") as f:
        json.dump({
            "paired": [(v, d) for v, d in paired],
        }, f, indent=2)
    print(f"\nRaw results written to {args.out}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_const = sub.add_parser("constants", help="print the derived fast-sweep constants and exit")
    p_const.set_defaults(func=cmd_constants)

    p_sweep = sub.add_parser("sweep", help="controlled fast-sweep measurement (reboots the board)")
    p_sweep.add_argument("--url", default=DEFAULT_URL)
    p_sweep.add_argument("--settle", type=float, default=14.0)
    p_sweep.add_argument("--sample", type=float, default=20.0)
    p_sweep.add_argument("--region-dbuf", type=lambda s: s.lower() == "true", default=True,
                          help="true/false")
    p_sweep.add_argument("--te-scanline", type=lambda s: s.lower() == "true", default=False,
                          help="true/false: enable the dynamic CO5300 set_tear_scanline "
                               "writeback (region-dbuf bursts wait for the scan to clear the "
                               "band instead of the next V-blank)")
    p_sweep.add_argument("--theme", default=None,
                          help="theme id to PUT as active after reboot (e.g. vault-tec or neon)")
    p_sweep.add_argument("--layout", type=int, default=None,
                          help="neonLayout 0=tube/1=segments/2=marquee to PUT when "
                               "--theme neon is used")
    p_sweep.add_argument("--out", default="fast_motion_sweep_results.json")
    p_sweep.set_defaults(func=cmd_sweep)

    p_cross = sub.add_parser(
        "crossings",
        help="bounded boost<->overboost crossing capture, or offline re-analysis")
    p_cross.add_argument("--url", default=DEFAULT_URL)
    p_cross.add_argument("--theme", default="neon")
    p_cross.add_argument("--layout", type=int, default=1,
                         help="neon layout (default: segments); use with --theme neon")
    p_cross.add_argument("--count", type=int, default=4,
                         help="completed crossings required in each direction")
    p_cross.add_argument("--threshold", type=float, default=None,
                         help="overboost PSI; live default comes from current config")
    p_cross.add_argument("--settle", type=float, default=4.0)
    p_cross.add_argument("--timeout", type=float, default=35.0)
    p_cross.add_argument("--poll", type=float, default=0.04)
    p_cross.add_argument("--region-dbuf", type=lambda s: s.lower() == "true", default=True,
                         help="true/false")
    p_cross.add_argument("--te-scanline", type=lambda s: s.lower() == "true", default=False,
                         help="true/false")
    p_cross.add_argument("--no-reboot", action="store_true")
    p_cross.add_argument("--input", type=Path,
                         help="analyze a prior JSON capture without contacting a board")
    p_cross.add_argument("--out", type=Path, default=Path("fast_motion_crossings.json"))
    p_cross.set_defaults(func=cmd_crossings)

    p_org = sub.add_parser("organic", help="cross-check against the normal demo waveform (no reboot)")
    p_org.add_argument("--url", default=DEFAULT_URL)
    p_org.add_argument("--duration", type=float, default=90.0)
    p_org.add_argument("--out", default="fast_motion_organic_results.json")
    p_org.set_defaults(func=cmd_organic)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
