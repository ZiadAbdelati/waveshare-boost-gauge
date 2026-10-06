#!/usr/bin/env python3
"""OBD2 BLE central lifecycle conformance (host only, no radio).

Builds and runs ``tools/test_obd_ble_lifecycle``, the fake-NimBLE harness that
drives the REAL ``main/boost_obd_ble.c``, and requires it to PASS. The harness
is the authority; this wrapper exists so the guard runs in the default host
suite with a clear SKIP on a source-only checkout.

Why this guard exists (2026-10-05, hardware report): toggling the OBD2 BLE
switch OFF then ON from the settings overlay left the central permanently
unable to reconnect until a reboot. NimBLE keeps a connection whose
``BLE_GAP_EVENT_CONNECT`` the application ignores, and it fails every later
``ble_gap_connect()`` to that peer with ``BLE_HS_EDONE`` (a connection already
exists) - which never clears by itself. The driver recorded the handle only
when enabled and tore down only the handle it remembered, so a link that landed
in the disabled window became an unfindable phantom and the retry loop spun on
EDONE for the rest of the drive. The harness reproduces exactly that
(invariant-2 FAIL, invariant-3 FAIL, a ``connect failed: status=0x000e``
storm) against the unfixed source, and invariant-4 ("BLE_HS_EDONE on a live
link is adopted, not retried") covers the half of the fix that rescues the
on-glass ordering -- reverting that half alone fails invariant-4 while the
other three still pass.

Run:  python3 tools/tests/test_obd_ble_lifecycle.py
      python3 tools/tests/test_obd_ble_lifecycle.py --bin <harness> --no-build
                                 (doctoring: point it at an old/other build)
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
TARGET = "test_obd_ble_lifecycle"
BINARY = REPO_ROOT / "sim" / "build" / TARGET
BUILD_DIR = REPO_ROOT / "sim" / "build"
RUN_TIMEOUT_S = 300
BUILD_TIMEOUT_S = 600

# The harness prints these. PASS alone is not enough: a binary that exits 0
# without running anything must not satisfy the guard.
PASS_LINE = "obd-ble-lifecycle: PASS"
FAIL_LINE = "obd-ble-lifecycle: FAIL"
MIN_OK_LINES = 5  # precondition + invariant-1..4


def find_cmake() -> str | None:
    found = shutil.which("cmake")
    if found:
        return found
    for cand in ("/usr/local/opt/cmake/bin/cmake", "/opt/homebrew/bin/cmake"):
        if pathlib.Path(cand).is_file():
            return cand
    return None


def skip(reason: str) -> int:
    print(f"SKIP: {reason}")
    print("      (build first: cmake -S sim -B sim/build && "
          f"cmake --build sim/build --target {TARGET})")
    return 0


def build(cmake: str) -> bool:
    if not (BUILD_DIR / "CMakeCache.txt").is_file():
        return False
    proc = subprocess.run(
        [cmake, "--build", str(BUILD_DIR), "--target", TARGET],
        cwd=str(REPO_ROOT), capture_output=True, text=True,
        timeout=BUILD_TIMEOUT_S,
    )
    if proc.returncode != 0:
        print("build failed:")
        print((proc.stdout or "")[-3000:] + (proc.stderr or "")[-3000:])
        return False
    return True


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=str(BINARY),
                    help="harness binary to run (default: the sim build output)")
    ap.add_argument("--no-build", action="store_true",
                    help="skip the build step (doctoring / external binary)")
    args = ap.parse_args(argv)

    binary = pathlib.Path(args.bin)
    if not args.no_build:
        cmake = find_cmake()
        if cmake is None:
            return skip("cmake not found")
        if not build(cmake):
            return skip("the sim tree is not configured (managed components absent?)")
    if not binary.is_file():
        return skip(f"{binary} not built")

    try:
        proc = subprocess.run([str(binary)], cwd=str(REPO_ROOT),
                              capture_output=True, text=True,
                              timeout=RUN_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print(f"FAIL: {binary.name} did not terminate within {RUN_TIMEOUT_S}s")
        return 1

    out = (proc.stdout or "") + (proc.stderr or "")
    tail = "\n".join(out.strip().splitlines()[-18:])
    ok_lines = out.count(": OK")
    problems: list[str] = []
    if proc.returncode != 0:
        problems.append(f"exit status {proc.returncode}")
    if FAIL_LINE in out or ": FAIL" in out:
        problems.append("the harness reported a failed invariant")
    if PASS_LINE not in out:
        problems.append(f"missing '{PASS_LINE}'")
    if ok_lines < MIN_OK_LINES:
        problems.append(f"only {ok_lines} ': OK' assertions (< {MIN_OK_LINES})")

    print(f"{binary.name}: {ok_lines} assertions OK" if not problems
          else f"{binary.name}: FAILED ({'; '.join(problems)})")
    if problems:
        print(tail)
        return 1
    print(f"PASS ({ok_lines} assertions; the real driver reached READY through "
          f"a disable/enable and left no phantom link)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
