#!/usr/bin/env python3
"""Interactive browser control panel for the BoostGauge host simulator.

Spawns ``sim/build/boost_gauge_sim --stream`` and serves the live gauge plus a
control surface over HTTP, so the REAL firmware renderer can be watched and
poked from a browser (no SDL2, no display needed).

Endpoints
    GET  /        control panel page
    GET  /stream  multipart/x-mixed-replace stream of PNG frames
    POST /cmd     body is one command line, forwarded to the sim's stdin
    GET  /shot    write the current frame to <shot-dir>/<theme>-<unit>-<psi>.png
    GET  /state   JSON status (theme/unit/psi/demo/page/sim health)

Only the standard library and Pillow (already required by sim/raw_to_png.py)
are used.  The sim's command grammar and BGFR frame protocol are documented in
sim/README.md and sim/main.c.
"""

from __future__ import annotations

import argparse
import io
import json
import os
import re
import signal
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from PIL import Image

MAGIC = b"BGFR"
FRAME_HEADER = struct.Struct("<4sIII")  # magic, width, height, seq
STATUS_RE = re.compile(rb"^\[PANEL\] psi=(-?\d+(?:\.\d+)?) demo=([01])")

THEMES = [
    ("dyno-cell", "Dyno Cell"),
    ("vault-tec", "Vault-Tec"),
    ("night-city", "Night City"),
    ("big-digit", "Big Digit"),
    ("neon", "Neon"),
]
UNITS = [("psi", "psi"), ("bar", "bar"), ("kPa", "kPa")]
UNITS_CANON = {"psi": "psi", "bar": "bar", "kpa": "kPa"}
LAYOUTS = [("tube", "tube"), ("segments", "segments"), ("marquee", "marquee")]
FONTS = [("0", "SF Alien"), ("1", "Doto")]
PRESETS = [("0", "Violet"), ("1", "Miami"), ("2", "Toxic"), ("3", "Bloodmoon")]
SCENARIOS = [("normal", "normal"), ("stale", "stale"), ("disconnected", "disconnected")]

DEFAULT_PSI = 5.0


class PanelState:
    """The state the panel believes the sim is in (the sim has no query path).

    Updated from the commands the panel itself sends, plus the ``[PANEL]``
    status lines the sim writes to stderr for the actual reading."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.theme = "dyno-cell"
        self.unit = "psi"
        self.page = "boost"
        # Count of frames the sim had emitted when the last command was sent.
        # /shot waits for frames strictly after this so it never captures a
        # frame rendered before the command took effect.
        self.cmd_marker = 0

    def reset(self) -> None:
        with self.lock:
            self.theme = "dyno-cell"
            self.unit = "psi"
            self.page = "boost"
            self.cmd_marker = 0

    def apply_command(self, cmd: str) -> None:
        """Track only values the sim will actually accept (it silently ignores
        an unknown id/unit/page, and the panel must not then report a state the
        gauge is not rendering)."""
        parts = cmd.split()
        if not parts:
            return
        with self.lock:
            if parts[0] == "theme" and len(parts) > 1:
                if parts[1] in {v for v, _ in THEMES}:
                    self.theme = parts[1]
            elif parts[0] == "unit" and len(parts) > 1:
                if parts[1].lower() in UNITS_CANON:
                    self.unit = UNITS_CANON[parts[1].lower()]
            elif parts[0] == "page" and len(parts) > 1:
                if parts[1] in ("boost", "tpms"):
                    self.page = parts[1]


class SimProcess:
    """Owns the ``--stream`` subprocess and its two reader threads.

    stdout carries only BGFR frames; stderr carries the firmware's logs plus
    the ``[PANEL]`` status lines.  The supervisor restarts the sim whenever it
    dies so the panel page survives a crash."""

    def __init__(self, binary: Path, state: PanelState, cwd: Path) -> None:
        self.binary = binary
        self.state = state
        self.cwd = cwd
        self.lock = threading.Lock()
        self.cond = threading.Condition(self.lock)
        self.proc: subprocess.Popen | None = None
        self.count = 0
        self.latest: tuple[int, int, int, bytes] | None = None
        self.psi: float | None = DEFAULT_PSI
        self.demo = False
        self.restarts = 0
        self.rebuilds = 0
        self.shutdown = False
        self.png_cache: tuple[int, bytes] | None = None
        self.encode_lock = threading.Lock()
        self._supervisor = threading.Thread(
            target=self._supervise, name="sim-supervisor", daemon=True
        )
        self._supervisor.start()

    # ---- lifecycle -------------------------------------------------------
    def binary_mtime(self) -> float | None:
        try:
            return self.binary.stat().st_mtime
        except OSError:
            return None

    def _spawn(self) -> subprocess.Popen:
        return subprocess.Popen(
            [str(self.binary), "--stream"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=str(self.cwd),
            bufsize=0,
        )

    def _wait(self, proc: subprocess.Popen) -> tuple[int | None, bool]:
        """Block until the sim exits or the binary on disk is rebuilt.

        A rebuilt binary MUST be picked up. Serving frames from a sim started
        before the last ``cmake --build`` is this harness's worst failure mode:
        the panel exists to show the CURRENT firmware, and a stale render is
        indistinguishable from a fix that did not work. Returns
        ``(returncode, rebuilt)``; ``rebuilt`` means the child was replaced, not
        that it crashed."""
        mtime = self.binary_mtime()
        while not self.shutdown:
            try:
                return proc.wait(timeout=0.5), False
            except subprocess.TimeoutExpired:
                pass
            now = self.binary_mtime()
            if now is not None and mtime is not None and now != mtime:
                mtime = now
                try:
                    proc.terminate()
                    proc.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    try:
                        proc.kill()
                        proc.wait(timeout=3)
                    except (subprocess.TimeoutExpired, OSError):
                        pass
                except OSError:
                    pass
                return None, True
        return None, False

    def _supervise(self) -> None:
        first = True
        rebuild_restart = False
        while not self.shutdown:
            # Back off only between CRASH restarts. A rebuild restart must come
            # back immediately: the page should reflect the new binary as soon
            # as it exists, and the 0.5 s crash-loop delay would leave the panel
            # holding a stale frame for half a second after every build.
            if not first and not rebuild_restart:
                time.sleep(0.5)
            first = False
            rebuild_restart = False
            try:
                proc = self._spawn()
            except OSError as exc:
                print(f"[panel] cannot start sim: {exc}; retrying", file=sys.stderr, flush=True)
                time.sleep(1.0)
                continue
            with self.lock:
                self.proc = proc
                # Frame count stays monotonic across restarts: a stream client
                # waiting on count > last must not stall until the new process
                # has emitted as many frames as the old one.
                self.latest = None
                self.png_cache = None
                self.psi = DEFAULT_PSI
                self.demo = False
                self.cond.notify_all()
            if self.shutdown:
                # close() raced with the spawn; do not leave an orphan sim.
                try:
                    proc.terminate()
                    proc.wait(timeout=3)
                except (subprocess.TimeoutExpired, OSError):
                    try:
                        proc.kill()
                        proc.wait(timeout=3)
                    except (subprocess.TimeoutExpired, OSError):
                        pass
                break
            self.state.reset()
            readers = [
                threading.Thread(target=self._read_frames, args=(proc,), daemon=True),
                threading.Thread(target=self._read_logs, args=(proc,), daemon=True),
            ]
            for r in readers:
                r.start()
            rc, rebuilt = self._wait(proc)
            for r in readers:
                r.join(timeout=2)
            if self.shutdown:
                break
            if rebuilt:
                rebuild_restart = True
                self.rebuilds += 1
                print(
                    f"[panel] {self.binary.name} was rebuilt; restarting the sim "
                    f"(#{self.rebuilds})",
                    file=sys.stderr,
                    flush=True,
                )
            else:
                self.restarts += 1
                print(
                    f"[panel] sim exited (rc={rc}); restarting (#{self.restarts})",
                    file=sys.stderr,
                    flush=True,
                )

    def close(self) -> None:
        self.shutdown = True
        with self.cond:
            self.cond.notify_all()
        proc = self.proc
        if proc is not None and proc.poll() is None:
            try:
                proc.terminate()
                proc.wait(timeout=3)
            except (subprocess.TimeoutExpired, OSError):
                try:
                    proc.kill()
                    proc.wait(timeout=3)
                except (subprocess.TimeoutExpired, OSError):
                    pass
        self._supervisor.join(timeout=3)
        # The supervisor may have been mid-spawn when shutdown was set; it
        # cleans up its own child, but verify nothing is left either way.
        proc = self.proc
        if proc is not None and proc.poll() is None:
            try:
                proc.kill()
                proc.wait(timeout=3)
            except (subprocess.TimeoutExpired, OSError):
                pass

    # ---- readers ---------------------------------------------------------
    def _read_frames(self, proc: subprocess.Popen) -> None:
        buf = b""
        fd = proc.stdout.fileno()
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
                magic, w, h, _seq = FRAME_HEADER.unpack_from(buf, 0)
                if magic != MAGIC or w == 0 or h == 0 or w > 4096 or h > 4096:
                    # Resync: drop up to the next magic (keep 3 bytes in case
                    # it straddles this read).
                    idx = buf.find(MAGIC)
                    buf = buf[idx:] if idx >= 0 else buf[-3:]
                    continue
                need = FRAME_HEADER.size + w * h * 4
                if len(buf) < need:
                    break
                rgba = bytes(buf[FRAME_HEADER.size:need])
                buf = buf[need:]
                with self.cond:
                    self.count += 1
                    self.latest = (self.count, w, h, rgba)
                    self.cond.notify_all()

    def _read_logs(self, proc: subprocess.Popen) -> None:
        buf = b""
        fd = proc.stderr.fileno()
        while True:
            try:
                chunk = os.read(fd, 1 << 16)
            except OSError:
                break
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                m = STATUS_RE.match(line)
                if m:
                    with self.lock:
                        self.psi = float(m.group(1))
                        self.demo = m.group(2) == b"1"

    # ---- frames ----------------------------------------------------------
    def get_frame(self, after: int, timeout: float) -> tuple[int, int, int, bytes] | None:
        """Latest frame whose sequence count is > ``after``, or None on timeout."""
        deadline = time.monotonic() + timeout
        with self.cond:
            while True:
                if self.latest is not None and self.latest[0] > after:
                    return self.latest
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return None
                self.cond.wait(remaining)

    def png_for(self, frame: tuple[int, int, int, bytes]) -> bytes:
        count, w, h, rgba = frame
        with self.lock:
            if self.png_cache is not None and self.png_cache[0] == count:
                return self.png_cache[1]
        with self.encode_lock:
            with self.lock:
                if self.png_cache is not None and self.png_cache[0] == count:
                    return self.png_cache[1]
            # snapshot_take()'s ARGB8888 is B,G,R,A in memory (see sim/raw_to_png.py).
            img = Image.frombytes("RGBA", (w, h), rgba, "raw", "BGRA").convert("RGB")
            out = io.BytesIO()
            img.save(out, format="PNG", optimize=False)
            data = out.getvalue()
            with self.lock:
                self.png_cache = (count, data)
            return data

    # ---- commands --------------------------------------------------------
    def send(self, cmd: str) -> bool:
        proc = self.proc
        if proc is None or proc.poll() is not None:
            return False
        try:
            proc.stdin.write((cmd.strip() + "\n").encode("utf-8"))
            proc.stdin.flush()
            return True
        except (BrokenPipeError, OSError, ValueError):
            return False

    def running(self) -> bool:
        proc = self.proc
        return proc is not None and proc.poll() is None


STATE = PanelState()
SIM: SimProcess | None = None
SHOT_DIR = Path("preview/panel")
VERBOSE = False


class PanelHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"
    server_version = "boost-sim-panel/1.0"

    def log_message(self, fmt: str, *args) -> None:
        if VERBOSE:
            super().log_message(fmt, *args)

    # ---- helpers ---------------------------------------------------------
    def _send(self, code: int, ctype: str, body: bytes) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def _send_json(self, code: int, payload: dict) -> None:
        self._send(code, "application/json", json.dumps(payload).encode("utf-8"))

    # ---- routes ----------------------------------------------------------
    def do_GET(self) -> None:  # noqa: N802 (http.server API)
        path = urllib.parse.urlsplit(self.path).path
        if path == "/":
            self._send(200, "text/html; charset=utf-8", PAGE_HTML.encode("utf-8"))
        elif path == "/stream":
            self._stream()
        elif path == "/shot":
            self._shot()
        elif path == "/state":
            self._send_json(200, self._state())
        elif path == "/favicon.ico":
            self._send(204, "image/x-icon", b"")
        else:
            self._send(404, "text/plain; charset=utf-8", b"not found\n")

    def do_POST(self) -> None:  # noqa: N802 (http.server API)
        path = urllib.parse.urlsplit(self.path).path
        if path != "/cmd":
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0 or length > 8192:
            self._send_json(400, {"ok": False, "error": "missing or oversized body"})
            return
        cmd = self.rfile.read(length).decode("utf-8", "replace").strip()
        if not cmd:
            self._send_json(400, {"ok": False, "error": "empty command"})
            return
        assert SIM is not None
        with STATE.lock:
            STATE.cmd_marker = SIM.count
        ok = SIM.send(cmd)
        if ok:
            STATE.apply_command(cmd)
        self._send_json(200 if ok else 503, {"ok": ok, "cmd": cmd})

    # ---- streaming -------------------------------------------------------
    def _stream(self) -> None:
        assert SIM is not None
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        last = 0
        while not SIM.shutdown:
            frame = SIM.get_frame(last, timeout=5.0)
            if frame is None:
                continue
            last = frame[0]
            png = SIM.png_for(frame)
            head = (
                b"--frame\r\nContent-Type: image/png\r\nContent-Length: "
                + str(len(png)).encode("ascii")
                + b"\r\n\r\n"
            )
            try:
                self.wfile.write(head)
                self.wfile.write(png)
                self.wfile.write(b"\r\n")
                self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                break

    def _shot(self) -> None:
        assert SIM is not None
        with STATE.lock:
            marker = STATE.cmd_marker
            theme = STATE.theme
            unit = STATE.unit
        # +1 so a frame that was already in flight when the command was sent is
        # not mistaken for the post-command state.
        frame = SIM.get_frame(marker + 1, timeout=5.0)
        if frame is None:
            self._send_json(503, {"ok": False, "error": "no fresh frame after command"})
            return
        png = SIM.png_for(frame)
        psi = SIM.psi
        psi_text = f"{psi:.1f}" if psi is not None else "na"
        name = re.sub(r"[^A-Za-z0-9._-]", "_", f"{theme}-{unit}-{psi_text}.png")
        out = SHOT_DIR / name
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(png)
        self._send_json(
            200,
            {
                "ok": True,
                "path": str(out),
                "abs": str(out.resolve()),
                "bytes": len(png),
                "frame": frame[0],
                "theme": theme,
                "unit": unit,
                "psi": psi,
            },
        )

    def _state(self) -> dict:
        assert SIM is not None
        with STATE.lock:
            theme, unit, page = STATE.theme, STATE.unit, STATE.page
        with SIM.lock:
            psi, demo, count = SIM.psi, SIM.demo, SIM.count
        return {
            "theme": theme,
            "unit": unit,
            "page": page,
            "psi": psi,
            "demo": demo,
            "frames": count,
            "running": SIM.running(),
            "restarts": SIM.restarts,
            "rebuilds": SIM.rebuilds,
            "simBuiltAt": _fmt_mtime(SIM.binary_mtime()),
        }


def _fmt_mtime(m: float | None) -> str | None:
    if m is None:
        return None
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(m))


def _options(*pairs: tuple[str, str]) -> str:
    return "".join(f'<option value="{v}">{label}</option>' for v, label in pairs)


PAGE_HTML = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BoostGauge sim panel</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 16px; background: #101116; color: #e6e8ee;
         font: 14px/1.4 -apple-system, Segoe UI, Roboto, sans-serif; }
  h1 { font-size: 16px; margin: 0 0 12px; letter-spacing: .04em;
       text-transform: uppercase; color: #9aa4b8; }
  .wrap { display: flex; gap: 20px; flex-wrap: wrap; align-items: flex-start; }
  .gauge { flex: 0 0 auto; }
  .gauge img { width: 466px; height: 466px; image-rendering: pixelated;
               background: #000; border: 1px solid #2a3040; border-radius: 8px; }
  .controls { flex: 1 1 320px; min-width: 320px; max-width: 460px; }
  fieldset { border: 1px solid #2a3040; border-radius: 8px; margin: 0 0 12px;
             padding: 10px 12px 12px; }
  legend { color: #9aa4b8; font-size: 12px; text-transform: uppercase;
           letter-spacing: .06em; padding: 0 4px; }
  .row { display: flex; align-items: center; gap: 8px; margin: 6px 0; }
  .row > label { flex: 0 0 84px; color: #b9c2d4; }
  select, input[type=number] { flex: 1 1 auto; background: #191e2a; color: #e6e8ee;
       border: 1px solid #333c50; border-radius: 6px; padding: 5px 6px; }
  input[type=range] { flex: 1 1 auto; }
  button { background: #22304a; color: #e6e8ee; border: 1px solid #3a4c70;
           border-radius: 6px; padding: 6px 12px; cursor: pointer; }
  button:hover { background: #2c3d5e; }
  button.primary { background: #2f6b3f; border-color: #46a35d; }
  .readout { font-family: ui-monospace, Menlo, monospace; font-size: 13px;
             background: #12161f; border: 1px solid #2a3040; border-radius: 8px;
             padding: 8px 10px; margin-bottom: 12px; white-space: pre-wrap; }
  .shot { margin-top: 8px; font-family: ui-monospace, Menlo, monospace;
          font-size: 12px; color: #8fe08f; min-height: 1.2em; word-break: break-all; }
  .hint { color: #78849c; font-size: 12px; margin-top: 4px; }
</style>
</head>
<body>
<h1>BoostGauge sim panel</h1>
<div class="wrap">
  <div class="gauge"><img id="live" src="/stream" alt="live gauge frame"></div>
  <div class="controls">
    <div class="readout" id="readout">connecting&hellip;</div>

    <fieldset><legend>Face</legend>
      <div class="row"><label for="theme">theme</label>
        <select id="theme">__THEMES__</select></div>
      <div class="row"><label>unit</label>
        __UNITS__</div>
    </fieldset>

    <fieldset><legend>Neon</legend>
      <div class="row"><label for="layout">layout</label>
        <select id="layout">__LAYOUTS__</select></div>
      <div class="row"><label for="font">font</label>
        <select id="font">__FONTS__</select></div>
      <div class="row"><label for="preset">preset</label>
        <select id="preset">__PRESETS__</select></div>
    </fieldset>

    <fieldset><legend>Vault-Tec</legend>
      <div class="row"><label for="needle">needle</label>
        <select id="needle"><option value="green">green</option><option value="red">red</option></select></div>
      <div class="row"><label for="tail">tail</label>
        <select id="tail"><option value="off">off</option><option value="on">on</option></select></div>
    </fieldset>

    <fieldset><legend>Dyno Cell</legend>
      <div class="row"><label for="dynoblack">background</label>
        <select id="dynoblack"><option value="off">face</option><option value="on">true black</option></select></div>
    </fieldset>

    <fieldset><legend>Pressure</legend>
      <div class="row"><label for="follow">source</label>
        <input type="checkbox" id="follow"> <span>follow the demo waveform</span></div>
      <div class="row"><label for="psiRange">psi</label>
        <input type="range" id="psiRange" min="-15" max="30" step="0.1" value="5"></div>
      <div class="row"><label for="psiNum">value</label>
        <input type="number" id="psiNum" min="-15" max="30" step="0.1" value="5"></div>
      <div class="row"><label>sweep</label>
        <button id="sweepOrganic">organic</button>
        <button id="sweepFast">fast 9.789 psi/s</button></div>
    </fieldset>

    <fieldset><legend>Page</legend>
      <div class="row"><label for="page">page</label>
        <select id="page"><option value="boost">boost</option><option value="tpms">TPMS</option></select></div>
      <div class="row"><label for="tpms">TPMS</label>
        <select id="tpms">__SCENARIOS__</select></div>
    </fieldset>

    <button class="primary" id="shot">Save screenshot</button>
    <div class="shot" id="shotPath"></div>
    <div class="hint">Files land in __SHOTDIR__/ named
      &lt;theme&gt;-&lt;unit&gt;-&lt;psi&gt;.png. Sliders and controls drive the real
      firmware entry points through <code>--stream</code>.</div>
  </div>
</div>
<script>
const $ = (id) => document.getElementById(id);
async function cmd(line) {
  try {
    const r = await fetch('/cmd', { method: 'POST', body: line });
    if (!r.ok) return false;
    return true;
  } catch (e) { return false; }
}
function on(id, ev, fn) { $(id).addEventListener(ev, (e) => fn(e.target)); }

on('theme', 'change', (t) => cmd('theme ' + t.value));
on('layout', 'change', (t) => cmd('layout ' + t.value));
on('font', 'change', (t) => cmd('neonfont ' + t.value));
on('preset', 'change', (t) => cmd('preset ' + t.value));
on('needle', 'change', (t) => cmd('needle ' + t.value));
on('tail', 'change', (t) => cmd('tail ' + t.value));
on('dynoblack', 'change', (t) => cmd('dynoblack ' + t.value));
on('page', 'change', (t) => cmd('page ' + t.value));
on('tpms', 'change', (t) => cmd('tpms ' + t.value));

document.querySelectorAll('input[name="unit"]').forEach((r) => {
  r.addEventListener('change', () => { if (r.checked) cmd('unit ' + r.value); });
});

function setPsi(v, send) {
  $('psiRange').value = v; $('psiNum').value = v;
  if (send) cmd('psi ' + v);
}
on('psiRange', 'input', (t) => { $('follow').checked = false; cmd('demo off'); setPsi(t.value, true); });
on('psiNum', 'input', (t) => { $('follow').checked = false; cmd('demo off'); setPsi(t.value, true); });
on('follow', 'change', (t) => cmd('demo ' + (t.checked ? 'on' : 'off')));
on('sweepOrganic', 'click', () => { $('follow').checked = true; cmd('sweep organic'); cmd('demo on'); });
on('sweepFast', 'click', () => { $('follow').checked = true; cmd('sweep fast'); cmd('demo on'); });

on('shot', 'click', async () => {
  $('shotPath').textContent = 'saving\u2026';
  try {
    const r = await fetch('/shot');
    const j = await r.json();
    $('shotPath').textContent = j.ok ? ('saved ' + j.path + ' (' + j.bytes + ' bytes)')
                                     : ('shot failed: ' + j.error);
  } catch (e) { $('shotPath').textContent = 'shot failed: ' + e; }
});

let initialised = false;
async function poll() {
  try {
    const s = await (await fetch('/state')).json();
    const psi = (s.psi === null || s.psi === undefined) ? '?' : Number(s.psi).toFixed(2);
    let text = `theme ${s.theme}   unit ${s.unit}   psi ${psi}   `
             + `source ${s.demo ? 'demo waveform' : 'fixed'}   page ${s.page}\n`
             + `sim ${s.running ? 'running' : 'RESTARTING'}   frames ${s.frames}`
             + (s.simBuiltAt ? `   built ${s.simBuiltAt}` : '   built ?')
             + (s.restarts ? `   restarts ${s.restarts}` : '')
             + (s.rebuilds ? `   auto-restarted ${s.rebuilds}x after a rebuild` : '');
    $('readout').textContent = text;
    if (!initialised) {
      initialised = true;
      $('theme').value = s.theme;
      document.querySelectorAll('input[name="unit"]').forEach((r) => {
        r.checked = (r.value === s.unit);
      });
      $('page').value = s.page;
      $('follow').checked = !!s.demo;
      if (s.psi !== null && s.psi !== undefined) setPsi(Number(s.psi).toFixed(1), false);
    } else if (s.demo && s.psi !== null && s.psi !== undefined) {
      // While following the waveform, track the live reading on the slider.
      $('psiRange').value = s.psi; $('psiNum').value = Number(s.psi).toFixed(1);
    }
  } catch (e) { /* server briefly unavailable */ }
}
poll();
setInterval(poll, 500);
</script>
</body>
</html>
"""


def build_page() -> str:
    units = "".join(
        f'<label><input type="radio" name="unit" value="{v}"> {label}</label>'
        for v, label in UNITS
    )
    return (
        PAGE_HTML.replace("__THEMES__", _options(*THEMES))
        .replace("__UNITS__", units)
        .replace("__LAYOUTS__", _options(*LAYOUTS))
        .replace("__FONTS__", _options(*FONTS))
        .replace("__PRESETS__", _options(*PRESETS))
        .replace("__SCENARIOS__", _options(*SCENARIOS))
        .replace("__SHOTDIR__", str(SHOT_DIR))
    )


def parse_args(argv: list[str]) -> argparse.Namespace:
    ap = argparse.ArgumentParser(
        description="Interactive browser control panel for the BoostGauge host simulator."
    )
    ap.add_argument("--sim", default="sim/build/boost_gauge_sim",
                    help="path to the simulator binary (default: %(default)s)")
    ap.add_argument("--host", default="127.0.0.1", help="bind address (default: %(default)s)")
    ap.add_argument("--port", type=int, default=8787, help="HTTP port (default: %(default)s)")
    ap.add_argument("--shot-dir", default="preview/panel",
                    help="directory for /shot PNGs (default: %(default)s)")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every HTTP request")
    return ap.parse_args(argv)


def main(argv: list[str]) -> int:
    global SIM, SHOT_DIR, VERBOSE

    args = parse_args(argv)
    SHOT_DIR = Path(args.shot_dir)
    VERBOSE = args.verbose

    binary = Path(args.sim)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print(
            f"[panel] simulator not found or not executable: {binary}\n"
            f"        build it with: cmake -S sim -B sim/build && "
            f"cmake --build sim/build -j4",
            file=sys.stderr,
        )
        return 2

    global PAGE_HTML
    PAGE_HTML = build_page()

    SIM = SimProcess(binary, STATE, Path.cwd())
    server = ThreadingHTTPServer((args.host, args.port), PanelHandler)
    server.daemon_threads = True

    print(
        f"[panel] sim   : {binary}\n"
        f"[panel] panel : http://{args.host}:{args.port}/\n"
        f"[panel] shots : {SHOT_DIR}",
        file=sys.stderr,
        flush=True,
    )

    def _term(_signum, _frame):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, _term)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[panel] shutting down", file=sys.stderr, flush=True)
    finally:
        server.server_close()
        SIM.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
