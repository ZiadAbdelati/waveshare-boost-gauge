#!/usr/bin/env python3
"""Hardware release gates for the live ESP32-S3 Boost Gauge board.

Run these before every release / flash-to-car. The board must be on the LAN
(http://192.168.50.102 by default) with the dashboard's API reachable; the
physical gauge is the thing under test for the cadence gates. The serial port
is optional and is NEVER held open: `--serial` is only read (no DTR/RTS poke),
and if another session owns the port the gate reports SKIP instead of failing.

Usage:
    python3 tools/check_hardware_gates.py
    python3 tools/check_hardware_gates.py --url http://192.168.50.102 --seconds 30
    python3 tools/check_hardware_gates.py --skip ap --skip ble   # no WiFi/AP or
                                                                 # Bluetooth gates
    python3 tools/check_hardware_gates.py --json                 # machine output

Gates (each labeled with the regression-ledger rows it guards):
  1. boot-health    - /state 200, firmwareVersion present, API healthy
  2. cadence        - tools/check_display_cadence.py 30 s soak, median >= 60
  3. per-theme fast-slew - bench_fast_motion.py sweep per theme, median >= 60
  4. ws-pool        - exactly 3 WebSocket clients; 4th rejected/closed; 1-3 keep
                      receiving (fourth-client invariant)
  5. http-latency   - p50/p90/max of 40 GET /api/v1/state (informational; PASS
                      when p50 < 400 ms against the 147 ms advertising baseline)
  6. ap-join        - join BoostGauge-* SoftAP (password boost1234), DHCP, GET
                      /state; ALWAYS restores the Mac's prior WiFi association
                      (requires --skip ap to omit; it disassociates this Mac)
  7. ble-advertise  - BoostGauge BLE advertisement visible (skips if blueutil /
                      system Bluetooth data unavailable)
  8. media-smoke    - tiny GIF upload -> commit -> delete -> empty (dual-slot)
  9. ota-state      - /state sane; bad-magic OTA body rejected 400 (no real OTA)
 10. logs-ring      - /logs?limit=5 strictly increasing t_ms; CSV header exact;
                      DELETE then empty; repeated DELETE harmless

Exit code is non-zero if any gate FAILs (SKIP is allowed).
"""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io as _io
import json
import os
import pathlib
import re
import socket
import statistics
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = REPO_ROOT / "tools"
DEFAULT_URL = "http://192.168.50.102"
WS_PATH = "/api/v1/state/ws"
WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

THEME_ORDER = ["dyno-cell", "vault-tec", "night-city", "big-digit", "neon"]
MIN_MEDIAN_RENDER_FPS = 60
LEDGER_MAP = {
    "boot-health": "ledger rows 11, 41, 51, 52 (HTTP API ready; OTA boot; persistence)",
    "cadence": "ledger rows 33, 89, 91, 107 + cadence contract 2026-08-09/10",
    "fast-slew": "ledger rows 27, 30, 44, 107 (constant-slew median>=60 per theme)",
    "ws-pool": "ledger rows 36-39, 81-85 (3-client pool; fourth rejected without disturbing 1-3)",
    "http-latency": "ledger row 7 (BLE loop web-latency baseline: p95 191->118, max 291->169)",
    "ap-join": "ledger rows 16, 20/36, 70 (SoftAP starvation; AP-join QR; scan never drops LAN)",
    "ble-advertise": "companion-app GATT contract docs/bluetooth-gatt.md (appBle toggle, advertising)",
    "media-smoke": "ledger rows 34, 35 (raw dual-slot store; abort/overlap/repeat-delete)",
    "ota-state": "ledger rows 51, 52 (OTA boot partition; bad magic rejected)",
    "logs-ring": "ledger rows 22, 38, 82 (5 Hz ring, strictly increasing t_ms, CSV header)",
}


class GateResult:
    def __init__(self, name: str) -> None:
        self.name = name
        self.status = "PASS"   # PASS | FAIL | SKIP
        self.detail: list[str] = []
        self.seconds = 0.0

    def line(self, text: str) -> None:
        self.detail.append(text)

    def finish(self, ok: bool | None) -> None:
        if ok is None:
            self.status = "SKIP"
        elif ok:
            self.status = "PASS"
        else:
            self.status = "FAIL"


def http_get(base: str, path: str, timeout: float = 5.0):
    with urllib.request.urlopen(f"{base}{path}", timeout=timeout) as response:
        return response.status, dict(response.headers), response.read()


def http_put(base: str, path: str, body: dict, timeout: float = 8.0):
    req = urllib.request.Request(
        f"{base}{path}", data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"}, method="PUT")
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.status, dict(response.headers), response.read()


def http_post_raw(base: str, path: str, body: bytes, timeout: float = 30.0):
    req = urllib.request.Request(f"{base}{path}", data=body, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def http_delete(base: str, path: str, body: dict | None = None, timeout: float = 8.0):
    data = json.dumps(body).encode() if body is not None else b""
    req = urllib.request.Request(f"{base}{path}", data=data, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def get_json(base: str, path: str, timeout: float = 5.0):
    status, _h, body = http_get(base, path, timeout=timeout)
    return status, json.loads(body) if body else {}


def api_json(base: str, path: str, method: str, body=None, timeout: float = 8.0):
    """HTTP helper returning (status, dict-or-raw)."""
    if method == "PUT":
        status, _h, raw = http_put(base, path, body, timeout=timeout)
    elif method == "DELETE":
        status, _h, raw = http_delete(base, path, body, timeout=timeout)
    else:
        status, _h, raw = http_get(base, path, timeout=timeout)
    try:
        return status, json.loads(raw) if raw else {}
    except ValueError:
        return status, raw


# --------------------------------------------------------------------------
# WebSocket client (stdlib only) for the ws-pool gate.
# --------------------------------------------------------------------------

class WsClient:
    def __init__(self, host: str, port: int) -> None:
        self.host = host
        self.port = port
        self.sock: socket.socket | None = None
        self.leftover = b""
        self.text_frames = 0
        self.opened_ok = False
        self.close_reason = ""

    def connect(self, path: str = WS_PATH, timeout: float = 6.0) -> bool:
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {self.host}:{self.port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        sock = socket.create_connection((self.host, self.port), timeout=timeout)
        sock.settimeout(timeout)
        try:
            sock.sendall(request.encode())
        except OSError as err:
            self.close_reason = f"send failed: {err}"
            sock.close()
            return False
        buf = b""
        try:
            while b"\r\n\r\n" not in buf:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                buf += chunk
        except socket.timeout:
            self.close_reason = "handshake timeout"
            sock.close()
            return False
        head, _, rest = buf.partition(b"\r\n\r\n")
        text = head.decode(errors="replace")
        status_line = text.split("\r\n", 1)[0] if text else ""
        status = status_line.split(" ", 2)[1] if len(status_line.split(" ", 2)) > 1 else ""
        accept = ""
        for line in text.split("\r\n")[1:]:
            if line.lower().startswith("sec-websocket-accept:"):
                accept = line.split(":", 1)[1].strip()
        expected = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        if status != "101" or accept != expected:
            self.close_reason = f"handshake not accepted (status={status!r})"
            sock.close()
            return False
        self.sock = sock
        self.leftover = rest
        self.opened_ok = True
        return True

    def _recv_exact(self, n: int) -> bytes:
        assert self.sock is not None
        data = b""
        while len(data) < n:
            chunk = self.sock.recv(n - len(data))
            if not chunk:
                raise ConnectionError("socket closed by peer")
            data += chunk
        return data

    def read_one(self) -> tuple[int, bytes]:
        """Return (opcode, payload). Raises ConnectionError on close/timeout."""
        while len(self.leftover) < 2:
            assert self.sock is not None
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("socket closed by peer")
            self.leftover += chunk
        b1, b2 = self.leftover[0], self.leftover[1]
        opcode = b1 & 0x0F
        length = b2 & 0x7F
        offset = 2
        if length == 126:
            while len(self.leftover) < 4:
                self.leftover += self._recv_exact(1)
            length = struct.unpack(">H", self.leftover[2:4])[0]
            offset = 4
        elif length == 127:
            while len(self.leftover) < 10:
                self.leftover += self._recv_exact(1)
            length = struct.unpack(">Q", self.leftover[2:10])[0]
            offset = 10
        masked = b2 & 0x80
        mask = b""
        if masked:
            while len(self.leftover) < offset + 4:
                self.leftover += self._recv_exact(1)
            mask = self.leftover[offset:offset + 4]
            offset += 4
        while len(self.leftover) < offset + length:
            self.leftover += self._recv_exact(1)
        payload = self.leftover[offset:offset + length]
        self.leftover = self.leftover[offset + length:]
        if masked:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def wait_text_frames(self, count: int, timeout: float = 8.0) -> int:
        """Read until `count` text frames observed or timeout; returns total."""
        deadline = time.monotonic() + timeout
        while self.text_frames < count and time.monotonic() < deadline:
            try:
                opcode, payload = self.read_one()
            except (ConnectionError, OSError, socket.timeout):
                self.close_reason = "read failed before enough frames"
                return self.text_frames
            if opcode == 1:
                self.text_frames += 1
            elif opcode == 8:
                self.close_reason = "peer sent CLOSE"
                return self.text_frames
        return self.text_frames

    def send_close(self) -> None:
        if self.sock is None:
            return
        try:
            mask = os.urandom(4)
            payload = b"\x03\xe8"
            header = bytes([0x88, 0x80 | len(payload)]) + mask + payload
            self.sock.sendall(header)
        except OSError:
            pass
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass
        self.sock = None


def ws_connect(base: str, timeout: float = 6.0) -> WsClient:
    from urllib.parse import urlparse
    parsed = urlparse(base)
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    return WsClient(host, port)


# --------------------------------------------------------------------------
# Serial capture (optional, never holds the port aggressively).
# --------------------------------------------------------------------------

def serial_reader(path: str, results: dict, duration: float):
    """Best-effort serial capture; never pokes DTR/RTS. If the port is busy
    (e.g. another session owns it) the gate is SKIPped, not failed."""
    try:
        import serial  # type: ignore
    except ImportError:
        results["serial"] = {"status": "SKIP", "detail": "pyserial not installed"}
        return
    try:
        ser = serial.Serial(path, 115200, timeout=0.5,
                            dsrdtr=False, rtscts=False, xonxoff=False,
                            dtr=False, rts=False)
    except (OSError, serial.SerialException) as err:
        results["serial"] = {
            "status": "SKIP",
            "detail": f"could not open {path} (port busy by another session?): {err}",
        }
        return
    buf = b""
    deadline = time.monotonic() + duration
    while time.monotonic() < deadline:
        try:
            chunk = ser.read(4096)
        except OSError:
            break
        if chunk:
            buf += chunk
    ser.close()
    text = buf.decode(errors="replace")
    errors = []
    for pattern in (r"Guru Meditation", r"ESP_ERR_NO_MEM", r"send color data failed",
                    r"panic", r"assert failed", r"abort\(\)"):
        if re.search(pattern, text, re.I):
            errors.append(pattern)
    results["serial"] = {
        "status": "PASS" if not errors else "FAIL",
        "detail": f"captured {len(text)} chars; error patterns: {errors or 'none'}",
    }


# --------------------------------------------------------------------------
# Gates
# --------------------------------------------------------------------------

def gate_boot_health(base: str) -> GateResult:
    gate = GateResult("boot-health")
    try:
        status, state = get_json(base, "/api/v1/state")
        ok = status == 200 and isinstance(state.get("firmwareVersion"), str) \
            and state["firmwareVersion"]
        gate.line(f"/state {status}, firmwareVersion={state.get('firmwareVersion')!r}")
        for path in ("/api/v1/config", "/api/v1/themes"):
            st, _ = get_json(base, path)
            ok = ok and st == 200
            gate.line(f"GET {path} -> {st}")
        gate.finish(ok)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_cadence(base: str, seconds: float) -> GateResult:
    gate = GateResult("cadence")
    try:
        proc = subprocess.run(
            [sys.executable, str(TOOLS / "check_display_cadence.py"),
             "--url", base, "--seconds", str(seconds)],
            capture_output=True, text=True, timeout=seconds + 60)
        out = (proc.stdout or "") + (proc.stderr or "")
        m = re.search(r"physical render FPS: min=(\d+) median=(\d+) samples=(\d+)", out)
        if m is None:
            gate.line(out[-500:])
            gate.finish(False)
            return gate
        median = int(m.group(2))
        gate.line(f"min={m.group(1)} median={median} samples={m.group(3)} "
                  f"(gate: median >= {MIN_MEDIAN_RENDER_FPS})")
        gate.finish(median >= MIN_MEDIAN_RENDER_FPS and proc.returncode == 0)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_fast_slew(base: str, settle: float, sample: float) -> GateResult:
    gate = GateResult("fast-slew")
    try:
        status, initial = get_json(base, "/api/v1/themes")
        restore = {
            "activeThemeId": initial.get("activeThemeId"),
            "neonLayout": initial.get("neonLayout"),
            "neonMarqueeSpin": initial.get("neonMarqueeSpin"),
            "demoMode": initial.get("demoMode"),
            "demoFastSweep": initial.get("demoFastSweep"),
            "pixelShift": initial.get("pixelShift"),
            "regionDBuf": initial.get("regionDBuf"),
            "teSync": initial.get("teSync"),
            "teScanline": initial.get("teScanline"),
        }
        gate.line(f"restore target: {json.dumps(restore)}")
    except Exception as err:  # noqa: BLE001
        gate.line(f"could not read initial theme state: {err}")
        gate.finish(False)
        return gate

    any_fail = False
    try:
        for theme_id in THEME_ORDER:
            out_json = pathlib.Path(os.environ.get("TMPDIR", "/tmp")) / \
                f"gate_fast_slew_{theme_id}.json"
            cmd = [sys.executable, str(TOOLS / "bench_fast_motion.py"), "sweep",
                   "--url", base, "--theme", theme_id,
                   "--settle", str(settle), "--sample", str(sample),
                   "--out", str(out_json)]
            gate.line(f"running sweep for {theme_id} ...")
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=240)
            if proc.returncode != 0:
                gate.line(f"  {theme_id}: bench_fast_motion failed rc={proc.returncode}: "
                          f"{(proc.stderr or '')[-300:]}")
                any_fail = True
                continue
            try:
                data = json.loads(out_json.read_text(encoding="utf-8"))
            except (OSError, ValueError) as err:
                gate.line(f"  {theme_id}: cannot parse bench output: {err}")
                any_fail = True
                continue
            median = data.get("renderFps_median")
            min_fps = data.get("renderFps_min")
            passed = isinstance(median, (int, float)) and median >= MIN_MEDIAN_RENDER_FPS
            if not passed:
                any_fail = True
            gate.line(f"  {theme_id}: min={min_fps} median={median} "
                      f"n={data.get('n')} -> {'PASS' if passed else 'FAIL'}")
    finally:
        # Restore the pre-gate theme + config state (never leave the board on
        # the sweep or a different theme).
        try:
            api_json(base, "/api/v1/themes/active", "PUT", {"id": restore["activeThemeId"]})
            cfg = {k: v for k, v in restore.items() if k != "activeThemeId" and v is not None}
            api_json(base, "/api/v1/themes/config", "PUT", cfg)
            gate.line(f"restored theme={restore['activeThemeId']} config={json.dumps(cfg)}")
        except Exception as err:  # noqa: BLE001
            any_fail = True
            gate.line(f"restore failed: {err}")
    gate.finish(not any_fail)
    return gate


def gate_ws_pool(base: str) -> GateResult:
    gate = GateResult("ws-pool")
    clients: list[WsClient] = []
    try:
        for i in range(3):
            client = ws_connect(base)
            if not client.connect():
                gate.line(f"client {i + 1}: handshake rejected ({client.close_reason})")
                gate.finish(False)
                return gate
            clients.append(client)
        # All three must receive frames at the 62.5 Hz push cadence.
        frame_requirement = 3
        got = [c.wait_text_frames(frame_requirement, timeout=6.0) for c in clients]
        ok = all(g >= frame_requirement for g in got)
        gate.line(f"3 clients each waiting: frames={got} (need {frame_requirement})")
        if not ok:
            gate.finish(False)
            return gate

        # Fourth client: must be rejected/closed without disturbing 1-3.
        fourth = ws_connect(base)
        fourth_opened = fourth.connect(timeout=4.0)
        texts = fourth.wait_text_frames(1, timeout=2.0)
        fourth_rejected = (not fourth_opened) or (texts == 0) \
            or ("CLOSE" in fourth.close_reason)
        gate.line(f"4th client: opened={fourth_opened} frames={texts} "
                  f"reason={fourth.close_reason!r} -> "
                  f"{'rejected/closed' if fourth_rejected else 'still open!'}")
        fourth.send_close()

        # Original three must keep receiving after the fourth attempt.
        before = [c.text_frames for c in clients]
        got_again = [c.wait_text_frames(c.text_frames + 1, timeout=4.0) for c in clients]
        kept = all(after > before[i] for i, after in enumerate(got_again))
        gate.line(f"after 4th: before={before} after={got_again} -> "
                  f"{'still receiving' if kept else 'disturbed!'}")
        gate.finish(ok and fourth_rejected and kept)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    finally:
        for client in clients:
            client.send_close()
        time.sleep(1.0)  # let httpd process the CLOSE frames and release slots
    return gate


def gate_http_latency(base: str, samples: int = 40) -> GateResult:
    gate = GateResult("http-latency")
    times: list[float] = []
    try:
        for _ in range(samples):
            start = time.monotonic()
            status, _ = get_json(base, "/api/v1/state")
            elapsed_ms = (time.monotonic() - start) * 1000.0
            if status == 200:
                times.append(elapsed_ms)
        if len(times) < samples // 2:
            gate.line(f"only {len(times)}/{samples} samples succeeded")
            gate.finish(False)
            return gate
        s = sorted(times)
        p50 = s[len(s) // 2]
        p90 = s[int(len(s) * 0.9)]
        gate.line(f"p50={p50:.1f}ms p90={p90:.1f}ms max={s[-1]:.1f}ms "
                  f"n={len(times)} (informational; reference 147 ms advertising baseline)")
        gate.finish(p50 < 400.0)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_ap_join() -> GateResult:
    """Join the BoostGauge-* SoftAP, GET /state over it, restore prior WiFi."""
    gate = GateResult("ap-join")
    router = "/System/Library/PrivateFrameworks/Apple80211.framework/Versions/Current/Resources/airport"
    try:
        hw = subprocess.run(["networksetup", "-listallhardwareports"],
                            capture_output=True, text=True, check=True).stdout
        m = re.search(r"Hardware Port: Wi-Fi\s*\n\s*Device: (en\d+)", hw)
        if not m:
            gate.line("no Wi-Fi hardware port found on this Mac")
            gate.finish(False)
            return gate
        device = m.group(1)
        # HeliPort/itlwm machines: CoreWLAN sees no interface, CLI join is
        # impossible, and even after a GUI join the data path may be dead
        # (hardware-verified 2026-08-23). Accept a manual GUI join: if the
        # interface already holds a 192.168.4.x lease, skip scan+join and
        # verify HTTP directly.
        probe = subprocess.run(["ifconfig", device], capture_output=True, text=True)
        if re.search(r"inet 192\.168\.4\.\d+", probe.stdout):
            gate.line(f"{device} already on the gauge AP (manual/HeliPort join); verifying HTTP")
            ap_ip = "192.168.4.1"
            ok = False
            for _ in range(3):
                r = subprocess.run(["curl", "-s", "--interface", device,
                                    "--connect-timeout", "5",
                                    f"http://{ap_ip}/api/v1/state"],
                                   capture_output=True, text=True, timeout=20)
                if '"psi"' in (r.stdout or ""):
                    gate.line("GET /state over the AP: 200 with psi - AP data path OK")
                    ok = True
                    break
                time.sleep(2)
            gate.finish(ok)
            return gate
        prior_ssid = ""
        try:
            prior_ssid = subprocess.run(
                ["networksetup", "-getairportnetwork", device],
                capture_output=True, text=True, check=True).stdout
            m2 = re.search(r"Current Wi-Fi Network: (.+)", prior_ssid)
            prior_ssid = m2.group(1).strip() if m2 else ""
        except subprocess.CalledProcessError:
            prior_ssid = ""
        gate.line(f"Wi-Fi device={device} prior_ssid={prior_ssid!r}")

        # Scan for the SoftAP SSID.
        target = None
        for _ in range(3):
            if os.path.exists(router):
                scan = subprocess.run([router, "-s"], capture_output=True, text=True,
                                      timeout=30)
                for line in (scan.stdout or "").splitlines():
                    if line.strip().startswith("BoostGauge-"):
                        target = line.split()[0]
                        break
            else:
                prof = subprocess.run(["system_profiler", "SPAirPortDataType"],
                                      capture_output=True, text=True, timeout=60)
                for line in (prof.stdout or "").splitlines():
                    if "BoostGauge-" in line:
                        m3 = re.search(r"BoostGauge-[0-9A-F]{4}", line)
                        if m3:
                            target = m3.group(0)
                            break
            if target:
                break
            time.sleep(2)
        if not target:
            gate.line("SoftAP BoostGauge-* not visible in scan")
            gate.finish(False)
            return gate
        gate.line(f"found SoftAP {target}")

        associated = False
        got_ip = ""
        try:
            for attempt in range(10):
                subprocess.run(["networksetup", "-setairportnetwork", device,
                                target, "boost1234"],
                               capture_output=True, text=True, timeout=30)
                time.sleep(2)
                check = subprocess.run(["networksetup", "-getairportnetwork", device],
                                       capture_output=True, text=True)
                if target in (check.stdout or ""):
                    associated = True
                    break
            if associated:
                ip = subprocess.run(["ipconfig", "getifaddr", device],
                                    capture_output=True, text=True, timeout=10)
                got_ip = (ip.stdout or "").strip()
            gate.line(f"associated={associated} ap_ip={got_ip!r}")
            if associated and got_ip:
                status, state = get_json(f"http://{got_ip}", "/api/v1/state", timeout=8)
                gate.line(f"GET http://{got_ip}/api/v1/state -> {status} "
                          f"firmware={state.get('firmwareVersion')!r}")
                gate.finish(status == 200)
            else:
                gate.finish(False)
        finally:
            # ALWAYS restore the original association (field-failure repro gate).
            if prior_ssid:
                subprocess.run(["networksetup", "-setairportnetwork", device, prior_ssid],
                               capture_output=True, text=True, timeout=30)
                gate.line(f"restored prior WiFi {prior_ssid!r}")
            else:
                if os.path.exists(router):
                    subprocess.run([router, "-z"], capture_output=True, text=True)
                gate.line("no prior SSID; disassociated (restore via user's WiFi)")
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_ble_advertise() -> GateResult:
    gate = GateResult("ble-advertise")
    try:
        if subprocess.run(["which", "blueutil"], capture_output=True, text=True).returncode != 0:
            gate.line("blueutil not installed - SKIP. Install with "
                      "'brew install blueutil' and re-run, or use a phone BLE "
                      "scanner to verify a device named 'BoostGauge' advertising "
                      "b6a00000-0000-4000-8000-00000000b6a0.")
            gate.finish(None)
            return gate
        found = False
        for _ in range(5):  # ~10 s of observation
            prof = subprocess.run(["system_profiler", "SPBluetoothDataType"],
                                  capture_output=True, text=True, timeout=60)
            if "BoostGauge" in (prof.stdout or ""):
                found = True
                gate.line("BoostGauge visible in system Bluetooth data")
                break
            time.sleep(2)
        gate.finish(found)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def tiny_gif_bytes() -> bytes:
    w = h = 2
    lsd = struct.pack("<HHB", w, h, 0x80) + b"\x00\x00"
    gct = b"\xff\xff\xff\x00\x00\x00"
    image = b"\x2c" + struct.pack("<HHHH", 0, 0, w, h) + b"\x00"
    lzw = b"\x02\x02\x44\x01\x00"
    return b"GIF89a" + lsd + gct + image + lzw + b"\x3b"


def gate_media_smoke(base: str) -> GateResult:
    gate = GateResult("media-smoke")
    try:
        status, media = get_json(base, "/api/v1/media/status")
        gate.line(f"initial /media/status -> {status} present={media.get('present')}")
        gif = tiny_gif_bytes()
        status, head, raw = http_post_raw(base, "/api/v1/media", gif)
        body = json.loads(raw) if raw else {}
        ok = status == 200 and body.get("present") is True
        gate.line(f"POST tiny GIF ({len(gif)} B) -> {status} present={body.get('present')}")
        status, media = get_json(base, "/api/v1/media/status")
        ok = ok and status == 200 and media.get("present") is True \
            and media.get("size") == len(gif)
        gate.line(f"GET /media/status after commit -> present={media.get('present')} "
                  f"size={media.get('size')}")
        status, _h, raw = http_delete(base, "/api/v1/media")
        body2 = json.loads(raw) if raw else {}
        ok = ok and status == 200 and body2.get("present") is False
        gate.line(f"DELETE media -> {status} present={body2.get('present')}")
        status, _h, raw = http_delete(base, "/api/v1/media")
        body3 = json.loads(raw) if raw else {}
        ok = ok and status == 200 and body3.get("present") is False
        gate.line(f"repeated DELETE media -> {status} present={body3.get('present')} "
                  f"(repeated deletes harmless)")
        gate.finish(ok)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_ota_state(base: str) -> GateResult:
    gate = GateResult("ota-state")
    try:
        status, state = get_json(base, "/api/v1/state")
        ok = status == 200 and isinstance(state.get("firmwareVersion"), str) \
            and state["firmwareVersion"]
        gate.line(f"/state -> {status} firmwareVersion={state.get('firmwareVersion')!r}")
        if "ota" in state:
            ota = state["ota"]
            gate.line(f"ota block present: {json.dumps(ota)[:200]}")
            ok = ok and isinstance(ota, dict) and "status" in ota
        else:
            gate.line("ota block: absent from /state on this branch; sanity via firmwareVersion only")

        # Bad-magic body: MUST be rejected 400 (no real OTA image is posted).
        status, _h, raw = http_post_raw(base, "/api/v1/ota",
                                        b"THIS_IS_NOT_AN_OTA_IMAGE" * 4, timeout=30)
        gate.line(f"POST /ota bad magic -> {status} (expect 400; body={raw[:60]!r})")
        ok = ok and status == 400
        gate.finish(ok)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


def gate_logs_ring(base: str) -> GateResult:
    gate = GateResult("logs-ring")
    try:
        status, logs = get_json(base, "/api/v1/logs?limit=5")
        samples = logs.get("samples", [])
        ok_inc = all(samples[i]["tMs"] < samples[i + 1]["tMs"]
                     for i in range(len(samples) - 1))
        gate.line(f"GET /logs?limit=5 -> {status} rows={len(samples)} strictly_increasing={ok_inc}")
        ok = status == 200 and ok_inc
        max_before = max((s["tMs"] for s in samples), default=0)

        status, headers, raw = http_get(base, "/api/v1/logs.csv")
        text = raw.decode("utf-8")
        header = text.splitlines()[0] if text else ""
        expected = "timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo"
        gate.line(f"GET /logs.csv -> {status} header_match={header == expected}")
        ok = ok and status == 200 and header == expected
        rows = list(csv.reader(_io.StringIO(text)))
        if len(rows) > 1:
            ok = ok and len(rows[1]) == 8
            gate.line(f"CSV data rows={len(rows) - 1} cols={len(rows[1]) if len(rows) > 1 else 0}")

        status, _h, raw = http_delete(base, "/api/v1/logs")
        gate.line(f"DELETE /logs -> {status}")
        ok = ok and status == 200
        status, logs = get_json(base, "/api/v1/logs?limit=5")
        after = logs.get("samples", [])
        # The 5 Hz background logger refills the ring within ~100 ms on the live
        # board, so strict emptiness only holds on the host mock. The live
        # invariant is: the pre-delete ring is gone and the returned rows are a
        # fresh-session rebuild (count small, every tMs newer than the old ring).
        max_after = max((s["tMs"] for s in after), default=0)
        cleared = (not after) or (len(after) < len(samples) and max_after > max_before)
        gate.line(f"GET /logs after DELETE -> {status} rows={len(after)} "
                  f"max_tMs_newer_than_old_ring={max_after > max_before} "
                  f"({'empty' if not after else 'fresh rebuild'})")
        ok = ok and status == 200 and cleared
        status, _h, raw = http_delete(base, "/api/v1/logs")
        gate.line(f"repeated DELETE /logs -> {status} (harmless)")
        ok = ok and status == 200
        gate.finish(ok)
    except Exception as err:  # noqa: BLE001
        gate.line(f"exception: {err}")
        gate.finish(False)
    return gate


# --------------------------------------------------------------------------
# Runner
# --------------------------------------------------------------------------

def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default=DEFAULT_URL)
    parser.add_argument("--serial", default=None, metavar="PORT",
                        help="optional serial device to capture during gates "
                             "(read-only; never held open or poked)")
    parser.add_argument("--seconds", type=float, default=30.0,
                        help="soak length for the cadence gate (default 30)")
    parser.add_argument("--skip", action="append", choices=["ap", "ble"], default=[],
                        help="skip a gate flagged risky for the local machine "
                             "(repeatable)")
    parser.add_argument("--bench-settle", type=float, default=10.0,
                        help="per-theme fast-slew settle seconds (default 10)")
    parser.add_argument("--bench-sample", type=float, default=18.0,
                        help="per-theme fast-slew sample seconds (default 18; sized "
                             "so occasional heavy-window drops cannot flip the median)")
    parser.add_argument("--json", action="store_true",
                        help="emit a machine-readable JSON report on stdout "
                             "(human output goes to stderr)")
    args = parser.parse_args(argv)
    base = args.url.rstrip("/")
    skips = set(args.skip)

    out = sys.stderr if args.json else sys.stdout
    def log(text: str) -> None:
        print(text, file=out, flush=True)

    log(f"Boost Gauge hardware gates - {base}")
    log(f"skips: {sorted(skips) or 'none'}; cadence soak: {args.seconds:.0f}s; "
        f"fast-slew settle/sample: {args.bench_settle:.0f}/{args.bench_sample:.0f}s")
    log("")

    # Optional serial capture across the whole run. Read-only (DTR/RTS off);
    # a busy port (another session owns it) reports SKIP and never fails gates.
    serial_result: dict = {"status": "SKIP", "detail": "--serial not given"}
    capture_thread = None
    if args.serial:
        import threading
        serial_result = {}
        capture_thread = threading.Thread(
            target=serial_reader,
            args=(args.serial, serial_result, max(30.0, args.seconds + 60.0)),
            daemon=True)
        capture_thread.start()
        log(f"serial capture on {args.serial} started (read-only; DTR/RTS off)")

    gates: list[GateResult] = []

    def run(name: str, fn, *fn_args) -> None:
        if any(name == s or name.startswith(s + "-") for s in skips):
            gate = GateResult(name)
            gate.line("skipped via --skip")
            gate.finish(None)
            gates.append(gate)
            return
        start = time.monotonic()
        try:
            gate = fn(*fn_args)
        except Exception as err:  # noqa: BLE001
            gate = GateResult(name)
            gate.line(f"gate raised: {err}")
            gate.finish(False)
        gate.seconds = time.monotonic() - start
        gates.append(gate)
        label = LEDGER_MAP.get(name, "")
        log(f"[{gate.status}] {name} ({gate.seconds:.1f}s)  {label}")
        for line in gate.detail:
            log(f"    {line}")
        log("")

    run("boot-health", gate_boot_health, base)
    run("cadence", gate_cadence, base, args.seconds)
    run("fast-slew", gate_fast_slew, base, args.bench_settle, args.bench_sample)
    run("ws-pool", gate_ws_pool, base)
    run("http-latency", gate_http_latency, base)
    run("ap-join", gate_ap_join)
    run("ble-advertise", gate_ble_advertise)
    run("media-smoke", gate_media_smoke, base)
    run("ota-state", gate_ota_state, base)
    run("logs-ring", gate_logs_ring, base)

    # Summary table.
    log("=" * 78)
    log(f"{'gate':<20}{'result':>6}{'seconds':>10}")
    log("-" * 78)
    for gate in gates:
        log(f"{gate.name:<20}{gate.status:>6}{gate.seconds:>10.1f}")
    failed = [g.name for g in gates if g.status == "FAIL"]
    log("-" * 78)
    log(f"TOTAL                                      {sum(1 for g in gates) - len(failed)}/"
        f"{len(gates)} passed, {len(failed)} failed")
    log("")

    if capture_thread is not None:
        capture_thread.join(timeout=15.0)
        ser = serial_result.get("serial", {})
        log(f"serial capture: {ser.get('status', 'SKIP')} - {ser.get('detail', '')}")

    if args.json:
        report = {
            "url": base,
            "skips": sorted(skips),
            "seconds": args.seconds,
            "gates": [
                {
                    "name": g.name,
                    "status": g.status,
                    "seconds": round(g.seconds, 2),
                    "ledger": LEDGER_MAP.get(g.name, ""),
                    "detail": g.detail,
                }
                for g in gates
            ],
            "serial": serial_result if args.serial else None,
            "exit_code": 1 if failed else 0,
        }
        print(json.dumps(report, indent=2))
    if failed:
        for name in failed:
            print(f"FAILED gate: {name}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
