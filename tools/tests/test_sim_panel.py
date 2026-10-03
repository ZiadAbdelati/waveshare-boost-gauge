#!/usr/bin/env python3
"""Sim control-panel harness - live-stream test (host only, no device).

Guards the contract between ``sim/main.c --stream`` and the browser panel
(``tools/sim_panel.py``):

  * stdout carries ONLY the BGFR frame stream: ``"BGFR" | u32 w | u32 h |
    u32 seq | RGBA``, with every firmware log line redirected to stderr, so a
    single ``printf`` can never desync the parser. The reader here treats any
    byte before the magic as a failure, which is exactly what a leaked log
    line would produce.
  * Frames are DISP_SIZE x DISP_SIZE (466x466) with w*h*4 pixel bytes.
  * The command grammar reaches the real firmware entry points: a ``unit``
    change must alter the rendered pixels (unit marks and tick numerals are
    baked at scene build), and a later change back must alter them again.
  * ``quit`` exits cleanly (rc 0) so the panel's supervisor can restart or
    shut down without killing the process.

The simulator must be built first (``cmake -S sim -B sim/build &&
cmake --build sim/build -j4``); when it is absent the test reports SKIP
instead of failing a source-only checkout.

Stdlib only. Run:  python3 tools/tests/test_sim_panel.py
"""

from __future__ import annotations

import os
import pathlib
import struct
import subprocess
import sys
import threading
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
SIM_BINARY = REPO_ROOT / "sim" / "build" / "boost_gauge_sim"

DISP_SIZE = 466
FRAME_HEADER = struct.Struct("<4sIII")
MAGIC = b"BGFR"


class Result:
    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        print(f"  [{'ok' if ok else 'FAIL'}] {label}")
        if not ok:
            self.failures.append(f"{label}: {detail}" if detail else label)


class FrameReader:
    """Drains stdout continuously (the sim blocks when the pipe fills) and
    keeps the newest frame plus a desync counter."""

    def __init__(self, proc: subprocess.Popen) -> None:
        self.proc = proc
        self.lock = threading.Lock()
        self.latest: tuple[int, int, int, bytes] | None = None
        self.frames = 0
        self.desyncs = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def _run(self) -> None:
        buf = b""
        fd = self.proc.stdout.fileno()
        while True:
            try:
                chunk = os.read(fd, 1 << 20)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while True:
                if len(buf) < FRAME_HEADER.size:
                    break
                magic, w, h, seq = FRAME_HEADER.unpack_from(buf, 0)
                if magic != MAGIC:
                    with self.lock:
                        self.desyncs += 1
                    idx = buf.find(MAGIC)
                    buf = buf[idx:] if idx >= 0 else buf[-3:]
                    continue
                need = FRAME_HEADER.size + w * h * 4
                if len(buf) < need:
                    break
                rgba = bytes(buf[FRAME_HEADER.size:need])
                buf = buf[need:]
                with self.lock:
                    self.frames += 1
                    self.latest = (seq, w, h, rgba)

    def wait_frame(self, after: int, timeout: float) -> tuple[int, int, int, bytes] | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self.lock:
                if self.latest is not None and self.latest[0] > after:
                    return self.latest
            time.sleep(0.01)
        return None

    def snapshot(self) -> tuple[int, int, int, bytes] | None:
        with self.lock:
            return self.latest


def main() -> int:
    print("sim_panel --stream harness")
    if not SIM_BINARY.is_file() or not os.access(SIM_BINARY, os.X_OK):
        print(f"  SKIP: {SIM_BINARY} not built "
              f"(cmake -S sim -B sim/build && cmake --build sim/build -j4)")
        return 0

    result = Result()
    proc = subprocess.Popen(
        [str(SIM_BINARY), "--stream"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        cwd=str(REPO_ROOT),
        bufsize=0,
    )
    reader = FrameReader(proc)
    try:
        first = reader.wait_frame(0, timeout=20.0)
        result.check(first is not None, "first BGFR frame arrives within 20 s",
                     "no frame on stdout")
        if first is None:
            return 1

        seq, w, h, rgba = first
        result.check((w, h) == (DISP_SIZE, DISP_SIZE),
                     f"frame is {DISP_SIZE}x{DISP_SIZE}", f"got {w}x{h}")
        result.check(len(rgba) == w * h * 4, "frame carries w*h*4 pixel bytes",
                     f"got {len(rgba)}")

        # A unit change rebuilds the face, so the pixels must change; the
        # second change proves it is the unit and not just animation.
        before = first
        changed = []
        for unit in ("bar", "kPa", "psi"):
            proc.stdin.write(f"unit {unit}\n".encode())
            proc.stdin.flush()
            nxt = reader.wait_frame(before[0] + 1, timeout=10.0)
            result.check(nxt is not None, f"fresh frame after 'unit {unit}'",
                         "no post-command frame")
            if nxt is None:
                return 1
            changed.append(nxt[3] != before[3])
            before = nxt
        result.check(all(changed), "every unit change alters the rendered pixels",
                     f"unchanged at: {[u for u, c in zip(('bar', 'kPa', 'psi'), changed) if not c]}")

        result.check(reader.desyncs == 0,
                     "stdout is pure BGFR (no log line leaked into the stream)",
                     f"{reader.desyncs} desync(s)")

        proc.stdin.write(b"quit\n")
        proc.stdin.flush()
        try:
            rc = proc.wait(timeout=10.0)
        except subprocess.TimeoutExpired:
            rc = None
        result.check(rc == 0, "quit exits the sim with rc 0", f"rc={rc}")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)

    print()
    if result.failures:
        print(f"{len(result.failures)} failure(s) of {result.checks} checks")
        for f in result.failures:
            print(f"  - {f}")
        return 1
    print(f"all {result.checks} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
