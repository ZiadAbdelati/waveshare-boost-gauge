#!/usr/bin/env python3
"""Unified host regression-test entry point for the Boost Gauge.

Runs every host-side test and prints one summary table (test name, pass/fail,
seconds), exiting non-zero if anything fails.

The suite is deliberately a thin runner around self-contained tests:

  * the EXISTING standalone scripts under tools/ (committed harnesses that
    must stay importable/runable one at a time) are invoked as subprocesses:
      - tools/test_mock_api.py        (HTTP mock end-to-end)
      - tools/test_ble_sim.py         (BLE simulator source conformance)
      - tools/test_map_conversion.py  (GM 12223861 MAP transfer function)
      - tools/test_rtc_epoch.py       (DS3231 civil->epoch conversion)
  * the NEW host tests under tools/tests/ (ledger-derived contracts, stdlib
    only, each with its own __main__) are also invoked as subprocesses.

Usage:
    python3 tools/test_suite.py                # full run
    python3 tools/test_suite.py --verbose      # stream each test's output
    python3 tools/test_suite.py --filter gatt  # only tests whose name matches
    python3 tools/test_suite.py --quick        # skip slow-flagged tests

--quick exists to skip benches marked slow in the registry below. No current
host test is marked slow (the slow benches are the hardware gates in
tools/check_hardware_gates.py), so --quick is currently a no-op kept for
future host-side benches; the plumbing is exercised and documented in
tools/tests/README.md.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = REPO_ROOT / "tools"


class TestSpec:
    def __init__(self, name: str, path: pathlib.Path, slow: bool = False) -> None:
        self.name = name
        self.path = path
        self.slow = slow


def build_registry() -> list[TestSpec]:
    """Order: existing committed scripts first, then the ledger tests."""
    existing = [
        "test_mock_api.py",
        "test_ble_sim.py",
        "test_map_conversion.py",
        "test_rtc_epoch.py",
    ]
    new = [
        "test_web_api_contract.py",
        "test_gatt_contract.py",
        "test_theme_store_invariants.py",
        "test_nvs_key_inventory.py",
        "test_network_semantics.py",
        "test_gesture_constants.py",
        "test_cadence_guard_math.py",
    ]
    specs = []
    for fname in existing:
        path = TOOLS / fname
        if path.is_file():
            specs.append(TestSpec(fname, path))
        else:
            specs.append(TestSpec(fname + " [MISSING]", TOOLS / fname))
    for fname in new:
        path = TOOLS / "tests" / fname
        if path.is_file():
            specs.append(TestSpec(fname, path))
        else:
            specs.append(TestSpec(fname + " [MISSING]", TOOLS / "tests" / fname))
    return specs


def run_test(spec: TestSpec, verbose: bool) -> tuple[bool, float, str]:
    start = time.monotonic()
    if not spec.path.is_file():
        return False, 0.0, f"missing file: {spec.path}"
    proc = subprocess.run(
        [sys.executable, str(spec.path)],
        cwd=str(REPO_ROOT),
        capture_output=not verbose,
        text=True,
        timeout=None,
    )
    elapsed = time.monotonic() - start
    ok = proc.returncode == 0
    detail = ""
    if ok:
        if verbose:
            detail = proc.stdout or ""
    else:
        detail = (proc.stdout or "") + (proc.stderr or "")
        if len(detail) > 4000:
            detail = detail[:4000] + "\n... (truncated)"
    return ok, elapsed, detail


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true",
                        help="stream each test's output instead of suppressing it")
    parser.add_argument("--filter", default=None, metavar="REGEX",
                        help="only run tests whose basename matches the regex")
    parser.add_argument("--quick", action="store_true",
                        help="skip tests flagged slow in the registry")
    parser.add_argument("--list", action="store_true", dest="list_only",
                        help="print the test registry and exit")
    args = parser.parse_args(argv)

    specs = build_registry()
    if args.filter:
        try:
            pattern = re.compile(args.filter, re.IGNORECASE)
        except re.error as error:
            print(f"--filter is not a valid regex: {error}", file=sys.stderr)
            return 2
        specs = [s for s in specs if pattern.search(s.name)]
    if args.quick:
        skipped = [s.name for s in specs if s.slow]
        specs = [s for s in specs if not s.slow]
        if skipped:
            print(f"[quick] skipping slow tests: {', '.join(skipped)}")
    if not specs:
        print("no tests matched", file=sys.stderr)
        return 2
    if args.list_only:
        for spec in specs:
            print(f"{spec.name:<46} {'slow' if spec.slow else 'fast'}")
        return 0

    print(f"Boost Gauge host regression suite - {len(specs)} test(s)\n")
    failures: list[tuple[str, str]] = []
    rows: list[tuple[str, bool, float]] = []
    for spec in specs:
        ok, elapsed, detail = run_test(spec, args.verbose)
        rows.append((spec.name, ok, elapsed))
        status = "PASS" if ok else "FAIL"
        if args.verbose and detail:
            print(detail.rstrip())
        elif not ok and detail:
            print(f"--- {spec.name} output (first 4 KB) ---")
            print(detail.rstrip())
            print(f"--- end {spec.name} output ---")
        if not ok:
            failures.append((spec.name, detail[:200]))
        print(f"{status:>4} {spec.name}  ({elapsed:.2f}s)")
        print()
    if not args.verbose:
        for name, _detail in failures:
            print(f"  FAILED: {name}")

    print("=" * 78)
    print(f"{'test':<46}{'result':>6}{'seconds':>10}")
    print("-" * 78)
    for name, ok, elapsed in rows:
        print(f"{name:<46}{('PASS' if ok else 'FAIL'):>6}{elapsed:>10.2f}")
    total = sum(el for _, _, el in rows)
    passed = sum(1 for _, ok, _ in rows if ok)
    print("-" * 78)
    print(f"{'TOTAL':<46}{f'{passed}/{len(rows)}':>6}{total:>10.2f}\n")
    if failures:
        print(f"FAILED: {len(failures)} test(s) failed; see rows above for details.")
        return 1
    print("All host tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
