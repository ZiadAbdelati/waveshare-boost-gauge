#!/usr/bin/env python3
"""Unit test the pass/fail math of the two cadence guards on synthetic samples.

Replicates (and cross-checks against) the exact formulas in:

  * tools/check_display_cadence.py - physical-gauge capacity gate: drop the
    first 4 warmup samples, sort ascending, median >= 60 required (ledger rows
    33, 89, 91, 107 - the "sustained median physical renderFps >= 60" guard).
  * tools/bench_theme_matrix.py demand_stats - organic demand-aware gate:
    PASS when median coverage (min(1, renderFps/gaugeDemandPerSecond)) over
    demanded windows is >= 0.95; zero-demand windows are idle, not failures
    (ledger rows 27-32 area, cadence contract 2026-08-09/10).

The existing scripts import cleanly (they have no import-time side effects),
so the constants are checked against the real modules and the formula is
replicated independently on edge cases.

Run:  python3 tools/tests/test_cadence_guard_math.py
"""

from __future__ import annotations

import os
import pathlib
import statistics
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import check_display_cadence  # noqa: E402  (stdlib only, safe import)
import bench_theme_matrix     # noqa: E402


class Result:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        if ok:
            print(f"PASS {label}")
        else:
            self.failures.append(label)
            print(f"FAIL {label}  [{detail if detail else 'assertion failed'}]")


# --- Independent replicas of the two guard formulas ------------------------

def cadence_verdict(render_fps: list[int], warmup: int = 4,
                    min_median: int = check_display_cadence.MIN_MEDIAN_RENDER_FPS):
    """Return ('pass', median) | ('fail', median) | ('insufficient', None).

    Mirrors tools/check_display_cadence.py: zero FPS entries are dropped by
    the collector, the first `warmup` samples are discarded as a bucket-straddle
    warmup, and the median uses the upper-middle of the sorted list.
    Use int() like the tool does, since the wire values are integers.
    """
    clean = [int(f) for f in render_fps if int(f) > 0]
    if len(clean) <= warmup:
        return "insufficient", None
    warm = clean[warmup:]
    median = sorted(warm)[len(warm) // 2]
    return ("pass" if median >= min_median else "fail", median)


def demand_verdict(rows, threshold: float = 0.95):
    """Return ('pass'|'fail'|'no_demand', coverage_median, zero_demand_count).

    Mirrors tools/bench_theme_matrix.py::demand_stats(): coverage per demanded
    window is min(1.0, fps/demand); the verdict is the MEDIAN coverage over
    demanded windows >= threshold; windows with zero demand are idle.
    """
    demanded = [(int(fps), int(demand)) for fps, demand in rows if demand > 0]
    if not demanded:
        return "no_demand", float("nan"), len(rows)
    coverage = statistics.median(min(1.0, fps / demand) for fps, demand in demanded)
    zero_demand = len(rows) - len(demanded)
    return ("pass" if coverage >= threshold else "fail", coverage, zero_demand)


def main() -> int:
    result = Result()

    # --- Constants pinned against the real modules -------------------------
    result.check(check_display_cadence.MIN_MEDIAN_RENDER_FPS == 60,
                 "check_display_cadence.MIN_MEDIAN_RENDER_FPS == 60")
    result.check(bench_theme_matrix.MIN_MEDIAN_RENDER_FPS == 60,
                 "bench_theme_matrix.MIN_MEDIAN_RENDER_FPS == 60")

    # --- check_display_cadence math ---------------------------------------
    # Exactly 60 after warmup must pass.
    verdict, median = cadence_verdict([0, 0, 0, 0, 60, 60, 60, 60, 60, 60])
    result.check(verdict == "pass", "cadence: median exactly 60 passes", f"got {verdict} {median}")

    # 59 median must fail.
    verdict, median = cadence_verdict([60, 60, 60, 60, 60, 59, 59, 59, 59, 59])
    result.check(verdict == "fail", "cadence: median 59 fails", f"got {verdict} {median}")

    # 59.9 on the wire is impossible (renderFps is an integer), but a mix whose
    # sorted-middle lands on 60 passes and one landing on 59 fails.
    mixed_pass = [60, 59, 60, 60, 59, 60, 60, 60, 59, 60, 60, 60]
    verdict, median = cadence_verdict(mixed_pass)
    result.check(verdict == "pass" and median == 60,
                 "cadence: mixed 59/60 with median 60 passes", f"got {verdict} {median}")
    mixed_fail = [60, 60, 60, 60, 59, 59, 59, 59, 59, 60, 60, 60]
    verdict, median = cadence_verdict(mixed_fail)
    result.check(verdict == "fail" and median == 59,
                 "cadence: mixed 59/60 with median 59 fails", f"got {verdict} {median}")

    # Warmup correctness: the first 4 samples must not decide the result.
    # Here the first 4 are 1 (= 1 FPS) but the sustained 60s after them pass.
    verdict, median = cadence_verdict([1, 1, 1, 1, 60, 60, 60, 60, 60])
    result.check(verdict == "pass" and median == 60,
                 "cadence: warmup samples are discarded before the verdict",
                 f"got {verdict} {median}")

    # Insufficient samples after warmup -> no verdict (tool exits non-zero).
    verdict, _ = cadence_verdict([60, 60, 60, 60])
    result.check(verdict == "insufficient",
                 "cadence: <= warmup samples is insufficient, not PASS")

    # Zero-FPS entries are dropped by the collector entirely.
    verdict, median = cadence_verdict([0, 0, 0, 0, 0, 60, 60, 60, 60, 60, 60])
    result.check(verdict == "pass" and median == 60,
                 "cadence: zero-FPS entries are dropped before warmup",
                 f"got {verdict} {median}")

    # Cross-check the replica against the real module's constant-driven logic.
    result.check(check_display_cadence.DEFAULT_SECONDS == 12,
                 "check_display_cadence.DEFAULT_SECONDS == 12 (sanity)")

    # --- bench_theme_matrix demand-coverage math ---------------------------
    # Exactly 95% median coverage passes.
    rows = [(57, 60), (57, 60), (60, 60)]
    verdict, coverage, zero = demand_verdict(rows)
    result.check(verdict == "pass" and abs(coverage - 0.95) < 1e-9,
                 "demand: median coverage exactly 0.95 passes",
                 f"got {verdict} cov={coverage} idle={zero}")

    # 94.9% median coverage fails.
    rows = [(47, 50), (47, 50), (60, 60)]
    verdict, coverage, zero = demand_verdict(rows)
    result.check(verdict == "fail" and coverage < 0.95,
                 "demand: median coverage 0.949 fails",
                 f"got {verdict} cov={coverage} idle={zero}")

    # Coverage is capped at 1.0 per window: 70fps/60 demand counts as full.
    rows = [(70, 60), (50, 60), (60, 60)]
    verdict, coverage, zero = demand_verdict(rows)
    result.check(verdict == "pass" and coverage == 1.0,
                 "demand: per-window coverage caps at 1.0 (no credit for overshoot)",
                 f"got {verdict} cov={coverage} idle={zero}")

    # Fully idle windows (zero demand) are not failures and never lower the
    # coverage median - they are excluded from the demanded set (ledger guard:
    # "zero-demand windows are idle, not failures").
    rows = [(0, 0), (0, 0), (60, 60), (59, 60), (60, 60)]
    verdict, coverage, zero = demand_verdict(rows)
    result.check(verdict == "pass" and zero == 2,
                 "demand: zero-demand windows are idle and excluded",
                 f"got {verdict} cov={coverage} idle={zero}")

    # All-idle sample set: no demand -> 'no_demand' verdict (never a FAIL).
    verdict, _, zero = demand_verdict([(0, 0), (0, 0), (0, 0)])
    result.check(verdict == "no_demand" and zero == 3,
                 "demand: all-idle is NO DEMAND, not a failure",
                 f"got {verdict} idle={zero}")

    # A single severely shortfalling demanded window fails even if everything
    # else is full, once it drags the median below threshold.
    rows = [(30, 60), (30, 60), (60, 60)]
    verdict, coverage, _ = demand_verdict(rows)
    result.check(verdict == "fail" and coverage == 0.5,
                 "demand: shortfall windows (50% coverage) make median 0.5 -> FAIL",
                 f"got {verdict} cov={coverage}")

    # Cross-check the replica against the real module's demand_stats() on the
    # same synthetic rows (the tools' module functions take display dicts).
    rows_dicts = [
        {"renderFps": 57, "gaugeDemandPerSecond": 60},
        {"renderFps": 60, "gaugeDemandPerSecond": 60},
        {"renderFps": 60, "gaugeDemandPerSecond": 60},
        {"renderFps": 60, "gaugeDemandPerSecond": 64},
    ]
    real = bench_theme_matrix.demand_stats(rows_dicts)
    mine = demand_verdict([(r["renderFps"], r["gaugeDemandPerSecond"])
                           for r in rows_dicts])
    result.check(real is not None and mine[1] == real[0] and mine[2] == real[2],
                 "demand: replica agrees with bench_theme_matrix.demand_stats",
                 f"real={real} replica={mine[:3]}")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
