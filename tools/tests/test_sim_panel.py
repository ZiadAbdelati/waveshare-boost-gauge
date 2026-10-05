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
  * The physical settings overlay is drivable and observable: ``overlay
    show|hide|page <0..2>|next|prev`` reach the firmware page hooks, ``ref
    rel|abs`` and ``atmosphere <kpa>`` reach the pressure reference, ``/state``
    reports the overlay page and reference from the sim's ``[PANEL]`` line, and
    the three overlay pages render distinct frames with the active page dot
    moving. A restart (crash or rebuild) is a fresh process, so the panel
    re-sends the tracked reference and atmosphere; the tests prove both survive.

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
PANEL_SCRIPT = REPO_ROOT / "tools" / "sim_panel.py"

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


def _free_port() -> int:
    import socket

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def _state_on(port: int) -> dict | None:
    import json
    import urllib.request

    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/state", timeout=3) as r:
            return json.load(r)
    except Exception:
        return None


def _post_cmd(port: int, cmd: str) -> dict | None:
    import json
    import urllib.request

    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/cmd", data=cmd.encode("utf-8"), method="POST"
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.load(r)
    except Exception:
        return None


def _shot_png(port: int) -> bytes | None:
    """Capture the panel's current frame through /shot and return the PNG
    bytes. /shot waits for a frame strictly after the last command, so this is
    the post-command render."""
    import json
    import urllib.request

    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/shot", timeout=15) as r:
            info = json.load(r)
        return pathlib.Path(info["path"]).read_bytes()
    except Exception:
        return None


def _wait_state(port: int, timeout: float, pred) -> dict | None:
    """Poll /state until ``pred`` holds; returns the last state seen (None if
    the panel never answered). Callers assert on the returned state so a
    timeout reports the actual value."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        last = _state_on(port)
        if last is not None and pred(last):
            return last
        time.sleep(0.1)
    return last


def decode_png(data: bytes) -> tuple[int, int, bytes]:
    """Minimal stdlib decoder for the panel's PNGs (8-bit RGB, non-interlaced,
    which is what Pillow emits here with ``optimize=False``): returns
    ``(width, height, RGB rows)``. Keeps the harness's no-Pillow promise while
    letting the tests compare actual pixels instead of trusting that two
    different PNGs mean two different frames."""
    import zlib

    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos = 8
    w = h = bit_depth = color_type = interlace = None
    idat = bytearray()
    while pos + 8 <= len(data):
        (ln,) = struct.unpack_from(">I", data, pos)
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bit_depth, color_type, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
    if bit_depth != 8 or color_type != 2 or interlace != 0:
        raise ValueError(
            f"unsupported PNG: depth={bit_depth} color={color_type} interlace={interlace}"
        )
    raw = zlib.decompress(bytes(idat))
    stride = w * 3
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        filt = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if filt == 1:      # Sub
            for i in range(3, stride):
                line[i] = (line[i] + line[i - 3]) & 0xFF
        elif filt == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif filt == 3:    # Average
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif filt == 4:    # Paeth
            for i in range(stride):
                a = line[i - 3] if i >= 3 else 0
                b = prev[i]
                c = prev[i - 3] if i >= 3 else 0
                pa = abs(b - c)
                pb = abs(a - c)
                pc = abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif filt != 0:
            raise ValueError(f"bad PNG filter {filt}")
        out += line
        prev = line
    return w, h, bytes(out)


def _dot_centroid_x(w: int, h: int, rgb: bytes, y0: int = 6, y1: int = 38) -> float | None:
    """x centroid of the lit page-indicator dots in the overlay's top strip.

    That strip holds ONLY the dots: the QR code starts lower and the page
    titles start at y=40. A bright-pixel centroid therefore tracks which dot
    is lit (white/accent) as opposed to the dark inactive dots."""
    xs = []
    for y in range(max(0, y0), min(h, y1)):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            r, g, b = rgb[o], rgb[o + 1], rgb[o + 2]
            if max(r, g, b) >= 200 and r + g + b >= 450:
                xs.append(x)
    if not xs:
        return None
    return sum(xs) / len(xs)


def check_panel_picks_up_a_rebuild(result: Result) -> None:
    """The panel must serve the binary that is on disk NOW.

    A panel whose sim child predates the last ``cmake --build`` keeps streaming
    the OLD firmware, which is indistinguishable from a change that did not work
    - it is exactly how a rebuilt layout came to look "exactly the same". The
    supervisor must notice the binary's mtime change and respawn, so this drives
    a real panel, bumps the mtime of a COPY of the binary, and requires the
    restart. The copy matters: touching the real artifact would make the panel's
    reported build time a lie.
    """
    if PANEL_SCRIPT is None:
        return
    import shutil
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        probe = pathlib.Path(tmp) / SIM_BINARY.name
        shutil.copy2(SIM_BINARY, probe)
        probe.chmod(0o755)
        port = _free_port()
        panel = subprocess.Popen(
            [sys.executable, str(PANEL_SCRIPT), "--port", str(port), "--sim", str(probe),
             "--shot-dir", str(REPO_ROOT / "preview" / "panel-test")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=str(REPO_ROOT),
        )
        try:
            _check_rebuild_pickup(result, panel, port, probe)
        finally:
            panel.terminate()
            try:
                panel.wait(timeout=10)
            except subprocess.TimeoutExpired:
                panel.kill()
                panel.wait(timeout=5)


def _check_rebuild_pickup(
    result: Result,
    panel: subprocess.Popen,
    port: int,
    probe: pathlib.Path,
) -> None:
    deadline = time.time() + 20.0
    state = None
    while time.time() < deadline:
        state = _state_on(port)
        if state is not None and state.get("running"):
            break
        time.sleep(0.25)
    result.check(state is not None and bool(state.get("running")),
                 "panel serves a running sim", f"state={state}")
    if state is None or not state.get("running"):
        return
    result.check(bool(state.get("simBuiltAt")),
                 "panel reports the sim build time",
                 "no simBuiltAt in /state")

    before_frames = int(state.get("frames") or 0)
    os.utime(probe, None)   # simulate `cmake --build`
    restarted = False
    deadline = time.time() + 15.0
    while time.time() < deadline:
        after = _state_on(port)
        if after and int(after.get("rebuilds") or 0) >= 1:
            restarted = True
            break
        time.sleep(0.25)
    result.check(restarted,
                 "panel respawns the sim when the binary is rebuilt",
                 "no rebuild observed within 15 s")
    if not restarted:
        return
    # The restart is a respawn, so `running` is briefly false while the new
    # child comes up; require it to come BACK, not that it never left.
    live = None
    deadline = time.time() + 15.0
    while time.time() < deadline:
        live = _state_on(port)
        if live and live.get("running"):
            break
        time.sleep(0.25)
    result.check(bool(live and live.get("running")),
                 "panel serves a fresh sim after the rebuild restart",
                 f"state={live}")
    after = live or {}
    result.check(int(after.get("frames") or 0) >= before_frames,
                 "frame counter stays monotonic across a rebuild restart",
                 f"{before_frames} -> {after.get('frames')}")


def check_settings_overlay(result: Result) -> None:
    """Drive a REAL panel and assert the settings-overlay contract end to end.

    Everything is observed through the panel's own surface: ``/cmd`` reaches the
    firmware entry points, ``/state`` reports what the sim's ``[PANEL]`` status
    line says (frames carry pixels, not widget state), and ``/shot`` captures
    the rendered frame. PNG bytes are compared directly for the change/restore
    checks (Pillow's encoder is lossless and writes no timestamp, so byte
    equality is pixel equality); the three overlay pages are decoded and
    compared pixel-for-pixel so the page indicator's moving dot can be located.
    """
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        port = _free_port()
        panel = subprocess.Popen(
            [sys.executable, str(PANEL_SCRIPT), "--port", str(port), "--sim", str(SIM_BINARY),
             "--shot-dir", str(pathlib.Path(tmp) / "shots")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=str(REPO_ROOT),
        )
        try:
            _check_overlay(result, port)
        finally:
            panel.terminate()
            try:
                panel.wait(timeout=10)
            except subprocess.TimeoutExpired:
                panel.kill()
                panel.wait(timeout=5)


def _check_overlay(result: Result, port: int) -> None:
    state = _wait_state(port, 20.0, lambda s: bool(s.get("running")))
    result.check(bool(state and state.get("running")),
                 "panel serves a running sim for the overlay checks", f"state={state}")
    if not state or not state.get("running"):
        return

    # Freeze the reading and start closed so later frames differ only by what
    # this function commands.
    _post_cmd(port, "psi 5")
    _post_cmd(port, "demo off")
    _post_cmd(port, "ref rel")
    _post_cmd(port, "overlay hide")
    _wait_state(port, 5.0, lambda s: s.get("overlay") == -1)
    closed = _shot_png(port)

    _post_cmd(port, "overlay show")
    st = _wait_state(port, 5.0, lambda s: s.get("overlay") == 0)
    result.check(st is not None and st.get("overlay") == 0,
                 "overlay show opens page 0 and /state reports it", f"state={st}")
    page0 = _shot_png(port)
    if closed is None or page0 is None:
        result.check(False, "overlay frames captured via /shot", "no PNG from /shot")
        return
    result.check(closed != page0,
                 "opening the overlay changes the rendered frame",
                 "closed and open frames encode identically")

    # Capture and decode each page; the sim reports which one is open.
    frames: dict[int, bytes] = {}
    for n in (1, 2):
        _post_cmd(port, f"overlay page {n}")
        st = _wait_state(port, 5.0, lambda s, n=n: s.get("overlay") == n)
        result.check(st is not None and st.get("overlay") == n,
                     f"overlay page {n} opens page {n} and /state reports it", f"state={st}")
        png = _shot_png(port)
        if png is None:
            result.check(False, f"page {n} frame captured via /shot", "no PNG from /shot")
            return
        frames[n] = png

    w, h, px0 = decode_png(page0)
    px1 = decode_png(frames[1])[2]
    px2 = decode_png(frames[2])[2]
    result.check(px0 != px1 and px1 != px2 and px0 != px2,
                 "each of the 3 overlay pages renders a different frame",
                 "two overlay pages decoded to identical pixels")

    # The page indicator's active dot moves with the page. The top strip holds
    # only the dots (QR lower, titles at y=40), so the bright-pixel centroid
    # is the lit dot's x.
    cx = {0: _dot_centroid_x(w, h, px0), 1: _dot_centroid_x(w, h, px1),
          2: _dot_centroid_x(w, h, px2)}
    result.check(all(v is not None for v in cx.values()),
                 "a lit page-indicator dot is found on every page", f"centroids={cx}")
    if all(v is not None for v in cx.values()):
        result.check(abs(cx[0] - cx[1]) >= 6 and abs(cx[1] - cx[2]) >= 6
                     and abs(cx[0] - cx[2]) >= 6,
                     "the page-indicator active dot moves between pages",
                     f"centroids={cx}")

    # next/prev follow the firmware's cycle with wraparound: swipe_left is
    # forward (0->1->2->0), swipe_right is backward (0->2->1->0).
    _post_cmd(port, "overlay page 0")
    _wait_state(port, 5.0, lambda s: s.get("overlay") == 0)
    _post_cmd(port, "overlay next")
    st = _wait_state(port, 5.0, lambda s: s.get("overlay") == 1)
    result.check(st is not None and st.get("overlay") == 1,
                 "overlay next advances to page 1", f"state={st}")

    _post_cmd(port, "overlay page 0")
    _wait_state(port, 5.0, lambda s: s.get("overlay") == 0)
    _post_cmd(port, "overlay prev")
    st = _wait_state(port, 5.0, lambda s: s.get("overlay") == 2)
    result.check(st is not None and st.get("overlay") == 2,
                 "overlay prev wraps back to page 2", f"state={st}")

    _post_cmd(port, "overlay page 3")
    st = _wait_state(port, 2.0, lambda s: s.get("overlay") == 2)
    result.check(st is not None and st.get("overlay") == 2,
                 "an out-of-range overlay page is rejected", f"state={st}")

    _post_cmd(port, "overlay hide")
    st = _wait_state(port, 5.0, lambda s: s.get("overlay") == -1)
    result.check(st is not None and st.get("overlay") == -1,
                 "overlay hide closes the overlay and /state reports it", f"state={st}")

    # Reference: absolute changes the numerals for a fixed reading; relative
    # is an exact passthrough and restores the frame byte-for-byte.
    _post_cmd(port, "psi 5")
    _post_cmd(port, "demo off")
    _post_cmd(port, "ref rel")
    st = _wait_state(port, 5.0, lambda s: s.get("ref") == "rel")
    result.check(st is not None and st.get("ref") == "rel",
                 "ref rel is reported by /state", f"state={st}")
    rel1 = _shot_png(port)

    _post_cmd(port, "ref abs")
    st = _wait_state(port, 5.0, lambda s: s.get("ref") == "abs")
    result.check(st is not None and st.get("ref") == "abs",
                 "ref abs is reported by /state", f"state={st}")
    abs1 = _shot_png(port)
    result.check(rel1 is not None and abs1 is not None and rel1 != abs1,
                 "ref abs changes the rendered frame at a fixed psi",
                 "relative and absolute frames encode identically")

    _post_cmd(port, "ref rel")
    _wait_state(port, 5.0, lambda s: s.get("ref") == "rel")
    rel2 = _shot_png(port)
    result.check(rel1 is not None and rel2 is not None and rel1 == rel2,
                 "ref rel restores the relative frame",
                 "relative frame changed after a round trip")

    # Atmosphere: in absolute mode the reference value is the atmosphere, so
    # 80 kPa must render differently from the 101.325 kPa default.
    _post_cmd(port, "ref abs")
    _wait_state(port, 5.0, lambda s: s.get("ref") == "abs")
    _post_cmd(port, "atmosphere 101.325")
    _wait_state(port, 5.0, lambda s: abs(float(s.get("atmosphere") or 0) - 101.325) < 0.01)
    a1 = _shot_png(port)
    _post_cmd(port, "atmosphere 80")
    st = _wait_state(port, 5.0, lambda s: abs(float(s.get("atmosphere") or 0) - 80.0) < 0.01)
    result.check(st is not None and abs(float(st.get("atmosphere") or 0) - 80.0) < 0.01,
                 "atmosphere is tracked by the panel", f"state={st}")
    a2 = _shot_png(port)
    result.check(a1 is not None and a2 is not None and a1 != a2,
                 "atmosphere changes the absolute rendering",
                 "80 kPa and 101.325 kPa frames encode identically")

    _post_cmd(port, "atmosphere 200")
    time.sleep(0.2)
    a3 = _shot_png(port)
    result.check(a2 is not None and a3 is not None and a2 == a3,
                 "an out-of-range atmosphere is rejected (render unchanged)",
                 "render moved after a rejected atmosphere")

    # A reference change must ALSO repaint an already-open overlay page: the
    # page was built before the mode flipped, so without an in-place rebuild
    # its REL/ABS square would keep the old state while the device is in ABS.
    _post_cmd(port, "overlay show")
    _post_cmd(port, "overlay page 2")
    _wait_state(port, 5.0, lambda s: s.get("overlay") == 2)
    _post_cmd(port, "ref rel")
    _wait_state(port, 5.0, lambda s: s.get("ref") == "rel")
    o1 = _shot_png(port)
    _post_cmd(port, "ref abs")
    st = _wait_state(port, 5.0, lambda s: s.get("ref") == "abs")
    o2 = _shot_png(port)
    result.check(st is not None and st.get("ref") == "abs"
                 and o1 is not None and o2 is not None and o1 != o2,
                 "ref abs repaints an already-open overlay page",
                 "open overlay frame unchanged by the reference")
    _post_cmd(port, "ref rel")
    _wait_state(port, 5.0, lambda s: s.get("ref") == "rel")
    o3 = _shot_png(port)
    result.check(o1 is not None and o3 is not None and o1 == o3,
                 "ref rel restores the open overlay page",
                 "open overlay frame did not return after ref rel")


def check_restart_reasserts_settings(result: Result) -> None:
    """A restart is a NEW process: the atmosphere override reverts to 101.325
    and the store may revert to relative, so the panel must re-send what the
    user asked for instead of leaving the page showing a state the fresh sim is
    not in. Uses a COPY of the binary so the real artifact stays untouched."""
    import shutil
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        probe = pathlib.Path(tmp) / SIM_BINARY.name
        shutil.copy2(SIM_BINARY, probe)
        probe.chmod(0o755)
        port = _free_port()
        panel = subprocess.Popen(
            [sys.executable, str(PANEL_SCRIPT), "--port", str(port), "--sim", str(probe),
             "--shot-dir", str(pathlib.Path(tmp) / "shots")],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            cwd=str(REPO_ROOT),
        )
        try:
            _check_reassert(result, port, probe)
        finally:
            panel.terminate()
            try:
                panel.wait(timeout=10)
            except subprocess.TimeoutExpired:
                panel.kill()
                panel.wait(timeout=5)


def _check_reassert(result: Result, port: int, probe: pathlib.Path) -> None:
    state = _wait_state(port, 20.0, lambda s: bool(s.get("running")))
    result.check(bool(state and state.get("running")),
                 "panel serves a running sim for the re-assert check", f"state={state}")
    if not state or not state.get("running"):
        return

    # Freeze the reading, then pick a distinctive reference/atmosphere.
    _post_cmd(port, "psi 5")
    _post_cmd(port, "demo off")
    _post_cmd(port, "ref abs")
    _post_cmd(port, "atmosphere 60")
    _wait_state(port, 5.0, lambda s: s.get("ref") == "abs")
    before = _shot_png(port)

    os.utime(probe, None)   # simulate `cmake --build`
    state = _wait_state(port, 25.0,
                        lambda s: int(s.get("rebuilds") or 0) >= 1 and s.get("running"))
    result.check(state is not None and int(state.get("rebuilds") or 0) >= 1
                 and bool(state.get("running")),
                 "panel restarts the sim for the re-assert check", f"state={state}")
    if state is None or not state.get("running"):
        return

    # The reference is report-driven, so it is the observable proof the panel
    # re-sent it: a fresh sim would report rel until the re-assert lands.
    state = _wait_state(port, 10.0, lambda s: s.get("ref") == "abs")
    result.check(state is not None and state.get("ref") == "abs",
                 "panel re-asserts the reference after a restart", f"state={state}")

    # Atmosphere is not on the wire: prove it through the render. Same frozen
    # psi + reference; if the atmosphere reverted to 101.325 the frame differs.
    _post_cmd(port, "psi 5")
    _post_cmd(port, "demo off")
    after = _shot_png(port)
    result.check(before is not None and after is not None and before == after,
                 "panel re-asserts the atmosphere after a restart",
                 "frame changed across a restart with a non-default atmosphere")


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

        # Dyno Cell's true-black face is a per-theme option like the Vault
        # needle or the neon preset, so the panel has to be able to reach it
        # through the same stream - and the flag has to change pixels AND
        # change them back, not latch.
        proc.stdin.write(b"theme dyno-cell\n")
        proc.stdin.flush()
        on_theme = reader.wait_frame(before[0] + 1, timeout=10.0)
        result.check(on_theme is not None, "fresh frame after 'theme dyno-cell'",
                     "no post-command frame")
        if on_theme is None:
            return 1
        before = on_theme
        for state in ("on", "off"):
            proc.stdin.write(f"dynoblack {state}\n".encode())
            proc.stdin.flush()
            nxt = reader.wait_frame(before[0] + 1, timeout=10.0)
            result.check(nxt is not None, f"fresh frame after 'dynoblack {state}'",
                         "no post-command frame")
            if nxt is None:
                return 1
            result.check(nxt[3] != before[3],
                         f"'dynoblack {state}' alters the rendered face",
                         "frame identical to the previous one")
            before = nxt

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

    check_panel_picks_up_a_rebuild(result)
    check_settings_overlay(result)
    check_restart_reasserts_settings(result)

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
