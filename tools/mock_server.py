#!/usr/bin/env python3
"""Host mock for the Boost Gauge web control plane API."""

from __future__ import annotations

import argparse
import csv
import json
import math
import mimetypes
import time
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse


ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web"
STARTED_AT = time.time()

# Each theme now carries a `style` discriminator that selects a distinct gauge
# layout on both the physical panel and the web mirror, not just a recolor.
THEMES = [
    {
        "id": "dyno-cell",
        "name": "Dyno Cell",
        "style": "arc",
        "colors": {
            "face": "#090A0D",
            "track": "#20242C",
            "text": "#F5F7FA",
            "muted": "#8C95A3",
            "vacuum": "#4DD2FF",
            "boost": "#B8F35A",
            "overboost": "#FF4F6D",
            "zero": "#FFFFFF",
        },
        "brightnessDefaults": {"high": 92, "low": 18},
    },
    {
        "id": "vault-tec",
        "name": "Vault-Tec",
        "style": "vault",
        "colors": {
            "face": "#02100A",
            "track": "#0C3D24",
            "text": "#38F08A",
            "muted": "#1F7A4D",
            "vacuum": "#38F08A",
            "boost": "#38F08A",
            "overboost": "#EAFC50",
            "zero": "#38F08A",
        },
        "brightnessDefaults": {"high": 85, "low": 12},
    },
    {
        "id": "night-city",
        "name": "Night City",
        "style": "hud",
        "colors": {
            "face": "#080A08",
            "track": "#1A1C0A",
            "text": "#FCEE0A",
            "muted": "#5A7A0A",
            "vacuum": "#00E5FF",
            "boost": "#FCEE0A",
            "overboost": "#FF003C",
            "zero": "#00E5FF",
        },
        "brightnessDefaults": {"high": 90, "low": 14},
    },
    {
        "id": "big-digit",
        "name": "Big Digit",
        "style": "bigdigit",
        "colors": {
            "face": "#0B0C0E",
            "track": "#20242C",
            "text": "#FFFFFF",
            "muted": "#0B0C0E",
            "vacuum": "#4DD2FF",
            "boost": "#B8F35A",
            "overboost": "#FF4F6D",
            "zero": "#FFFFFF",
        },
        "brightnessDefaults": {"high": 90, "low": 16},
    },
]

CONFIG = {
    "brightnessHigh": 100,
    "brightnessLow": 12,
    "dimSchedule": {"enabled": True, "startMinutes": 21 * 60, "endMinutes": 7 * 60},
    "timezoneOffsetMinutes": 0,
    "activeThemeId": "dyno-cell",
    "psiMin": -15.0,
    "psiMax": 10.0,
    "psiOverboost": 8.0,
    "zeroAngle": 236.25,
}

MEDIA = {
    "present": False,
    "name": None,
    "sizeBytes": 0,
    "contentType": None,
    "uploadedAtEpochMs": None,
    "playbackEnabled": False,
    "playbackNote": "Upload/status/delete are implemented; on-device GIF playback is deferred.",
}

NETWORK = {
    "mode": "apsta",
    "staEnabled": True,
    "staConnected": True,
    "staSsid": "PitWall-5G",
    "staIp": "192.168.1.42",
    "rssi": -54,
    "apSsid": "BoostGauge-7F3A",
}

SCAN_NETWORKS = [
    {"ssid": "PitWall-5G", "rssi": -54, "auth": 3},
    {"ssid": "Paddock-Guest", "rssi": -67, "auth": 0},
    {"ssid": "Garage-IoT", "rssi": -72, "auth": 3},
]

LOGS: list[dict[str, float | int | str | bool]] = []
PEAK = 0.0
TIME_ANCHOR_MS = int(time.time() * 1000)


def current_psi() -> float:
    elapsed = time.time() - STARTED_AT
    phase = elapsed * ((2 * math.pi) / 7.5)
    envelope = 0.55 + 0.45 * math.sin(phase * 0.5 + 0.4)
    norm = (0.5 + 0.5 * math.sin(phase)) * envelope
    norm = norm * 0.92 + 0.08
    psi_min = float(CONFIG.get("psiMin", -15.0))
    psi_max = float(CONFIG.get("psiMax", 10.0))
    # Sweep the live face so the default 10 PSI dial is fully exercised.
    psi = psi_min + (psi_max - psi_min) * norm
    psi += 0.12 * math.sin(elapsed * 17.3) + 0.06 * math.sin(elapsed * 31.1)
    return round(max(psi_min, min(psi_max, psi)), 1)


def zone_for(psi: float) -> str:
    overboost = float(CONFIG.get("psiOverboost", 8.0))
    if psi >= overboost:
        return "OVER"
    if psi >= 0.35:
        return "BOOST"
    if psi > -0.35:
        return "ATMO"
    return "VAC"


def state_payload() -> dict[str, float | int | str | bool]:
    global PEAK
    psi = current_psi()
    PEAK = max(PEAK, psi)
    payload = {
        "psi": psi,
        "peakPsi": round(PEAK, 1),
        "zone": zone_for(psi),
        "demo": True,
        "brightness": CONFIG["brightnessHigh"],
        "firmwareVersion": "mock-v0.3.0-web",
        "uptimeMs": int((time.time() - STARTED_AT) * 1000),
        "epochMs": TIME_ANCHOR_MS + int((time.time() - STARTED_AT) * 1000),
        "timezoneOffsetMinutes": CONFIG["timezoneOffsetMinutes"],
        "activeThemeId": CONFIG["activeThemeId"],
    }
    LOGS.append({"ts": payload["epochMs"], "psi": psi, "zone": payload["zone"], "demo": True})
    del LOGS[:-3600]
    return payload


THEME_DEFAULTS = {
    t["id"]: dict(t["colors"]) for t in THEMES
}


def themes_payload() -> dict:
    """Mirror the firmware's /themes shape, including per-theme `customized`."""
    out = []
    for t in THEMES:
        item = dict(t)
        item["customized"] = any(
            t["colors"][k] != THEME_DEFAULTS[t["id"]][k]
            for k in ("vacuum", "boost", "overboost")
        )
        out.append(item)
    return {
        "activeThemeId": CONFIG["activeThemeId"],
        "bigDigitStaticBg": bool(CONFIG.get("bigDigitStaticBg", False)),
        "pixelShift": bool(CONFIG.get("pixelShift", True)),
        "bigDigitColorText": bool(CONFIG.get("bigDigitColorText", False)),
        "themes": out,
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "BoostGaugeMock/1.0"

    def log_message(self, fmt: str, *args: object) -> None:
        print(f"{self.address_string()} - {fmt % args}")

    def send_json(self, payload: object, status: HTTPStatus = HTTPStatus.OK) -> None:
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def read_json(self) -> dict:
        length = int(self.headers.get("Content-Length", "0"))
        if not length:
            return {}
        return json.loads(self.rfile.read(length).decode())

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/api/v1/state":
            self.send_json(state_payload())
        elif path == "/api/v1/config":
            self.send_json(CONFIG)
        elif path == "/api/v1/themes":
            self.send_json(themes_payload())
        elif path == "/api/v1/logs":
            limit = int(parse_qs(parsed.query).get("limit", ["120"])[0])
            self.send_json({"sessionStartedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(STARTED_AT)), "samples": LOGS[-limit:]})
        elif path == "/api/v1/logs.csv":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Disposition", "attachment; filename=boost-gauge-log.csv")
            self.end_headers()
            writer = csv.DictWriter(self.wfile_text(), fieldnames=["ts", "psi", "zone", "demo"])
            writer.writeheader()
            writer.writerows(LOGS)
        elif path == "/api/v1/media/status":
            self.send_json(MEDIA)
        elif path == "/api/v1/network":
            self.send_json(NETWORK)
        elif path == "/api/v1/network/scan":
            self.send_json({"networks": SCAN_NETWORKS})
        elif path == "/api/v1/events":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            try:
                while True:
                    payload = json.dumps(state_payload())
                    self.wfile.write(f"data: {payload}\n\n".encode())
                    self.wfile.flush()
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                return
        else:
            self.serve_static(path)

    def do_HEAD(self) -> None:
        parsed = urlparse(self.path)
        path = "/index.html" if parsed.path == "/" else parsed.path
        target = (WEB_ROOT / path.lstrip("/")).resolve()
        if WEB_ROOT not in target.parents and target != WEB_ROOT:
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not target.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mimetypes.guess_type(target.name)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(target.stat().st_size))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    def wfile_text(self):
        class Sink:
            def __init__(self, raw):
                self.raw = raw

            def write(self, text):
                self.raw.write(text.encode())

        return Sink(self.wfile)

    def do_PUT(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/config":
            payload = self.read_json()
            for key in ("brightnessHigh", "brightnessLow", "timezoneOffsetMinutes", "activeThemeId"):
                if key in payload:
                    CONFIG[key] = payload[key]
            if "dimSchedule" in payload:
                CONFIG["dimSchedule"] = {**CONFIG["dimSchedule"], **payload["dimSchedule"]}
            if any(key in payload for key in ("psiMin", "psiMax", "psiOverboost", "zeroAngle")):
                psi_min = float(payload.get("psiMin", CONFIG["psiMin"]))
                psi_max = float(payload.get("psiMax", CONFIG["psiMax"]))
                psi_overboost = float(payload.get("psiOverboost", CONFIG["psiOverboost"]))
                zero_angle = float(payload.get("zeroAngle", CONFIG["zeroAngle"]))
                valid = (
                    psi_min < 0
                    and -30.0 <= psi_min <= -1.0
                    and 5.0 <= psi_max <= 40.0
                    and 0.0 < psi_overboost < psi_max
                    and 180.0 <= zero_angle <= 315.0
                )
                if not valid:
                    self.send_json(
                        {
                            "error": "invalid gauge range",
                            "psiMin": psi_min,
                            "psiMax": psi_max,
                            "psiOverboost": psi_overboost,
                            "zeroAngle": zero_angle,
                        },
                        HTTPStatus.BAD_REQUEST,
                    )
                    return
                CONFIG["psiMin"] = psi_min
                CONFIG["psiMax"] = psi_max
                CONFIG["psiOverboost"] = psi_overboost
                CONFIG["zeroAngle"] = zero_angle
            self.send_json(CONFIG)
        elif parsed.path == "/api/v1/themes/active":
            payload = self.read_json()
            theme_ids = {theme["id"] for theme in THEMES}
            if payload.get("id") not in theme_ids:
                self.send_json({"error": "unknown theme"}, HTTPStatus.BAD_REQUEST)
                return
            CONFIG["activeThemeId"] = payload["id"]
            self.send_json(themes_payload())
        elif parsed.path == "/api/v1/themes/config":
            payload = self.read_json()
            if "bigDigitStaticBg" in payload:
                CONFIG["bigDigitStaticBg"] = bool(payload["bigDigitStaticBg"])
            if "pixelShift" in payload:
                CONFIG["pixelShift"] = bool(payload["pixelShift"])
            if "bigDigitColorText" in payload:
                CONFIG["bigDigitColorText"] = bool(payload["bigDigitColorText"])
            theme_id = payload.get("id")
            if theme_id:
                theme = next((t for t in THEMES if t["id"] == theme_id), None)
                if theme is None:
                    self.send_json({"error": "theme_not_found"}, HTTPStatus.NOT_FOUND)
                    return
                if payload.get("reset"):
                    for key in ("vacuum", "boost", "overboost"):
                        theme["colors"][key] = THEME_DEFAULTS[theme_id][key]
                else:
                    for key, value in (payload.get("colors") or {}).items():
                        if key in ("vacuum", "boost", "overboost"):
                            theme["colors"][key] = value
            self.send_json(themes_payload())
        elif parsed.path == "/api/v1/network":
            payload = self.read_json()
            if payload.get("mode") in ("ap", "apsta"):
                NETWORK["mode"] = payload["mode"]
            if "ssid" in payload and payload["ssid"]:
                NETWORK["staSsid"] = payload["ssid"]
            NETWORK["staEnabled"] = NETWORK["mode"] == "apsta"
            NETWORK["staConnected"] = NETWORK["staEnabled"]
            self.send_json(NETWORK)
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_POST(self) -> None:
        global TIME_ANCHOR_MS
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        if parsed.path == "/api/v1/time":
            payload = self.read_json()
            TIME_ANCHOR_MS = int(payload["epochMs"])
            CONFIG["timezoneOffsetMinutes"] = int(payload["timezoneOffsetMinutes"])
            self.send_json({"epochMs": TIME_ANCHOR_MS, "timezoneOffsetMinutes": CONFIG["timezoneOffsetMinutes"]})
        elif parsed.path == "/api/v1/media":
            body = self.rfile.read(length)
            if not body.startswith(b"GIF8"):
                self.send_json({"error": "expected GIF payload"}, HTTPStatus.BAD_REQUEST)
                return
            MEDIA.update(
                {
                    "present": True,
                    "name": unquote(self.headers.get("X-Filename", "active.gif")),
                    "sizeBytes": len(body),
                    "contentType": self.headers.get("Content-Type", "image/gif"),
                    "uploadedAtEpochMs": int(time.time() * 1000),
                    "playbackEnabled": False,
                }
            )
            self.send_json(MEDIA)
        elif parsed.path == "/api/v1/ota":
            body = self.rfile.read(length)
            if len(body) < 4096:
                self.send_json({"error": "binary too small to be a firmware image"}, HTTPStatus.BAD_REQUEST)
                return
            self.send_json({"status": "OTA image accepted by mock; reboot pending", "sizeBytes": len(body), "progress": 100})
        elif parsed.path == "/api/v1/network/reconnect":
            self.rfile.read(length)
            NETWORK["staConnected"] = NETWORK["mode"] == "apsta"
            self.send_json(NETWORK)
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def do_DELETE(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/logs":
            LOGS.clear()
            self.send_response(HTTPStatus.NO_CONTENT)
            self.end_headers()
        elif parsed.path == "/api/v1/media":
            MEDIA.update({"present": False, "name": None, "sizeBytes": 0, "contentType": None, "uploadedAtEpochMs": None})
            self.send_response(HTTPStatus.NO_CONTENT)
            self.end_headers()
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def serve_static(self, path: str) -> None:
        if path == "/":
            path = "/index.html"
        target = (WEB_ROOT / path.lstrip("/")).resolve()
        if WEB_ROOT not in target.parents and target != WEB_ROOT:
            self.send_error(HTTPStatus.FORBIDDEN)
            return
        if not target.is_file():
            self.send_error(HTTPStatus.NOT_FOUND)
            return
        body = target.read_bytes()
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", default=8080, type=int)
    args = parser.parse_args()
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"Serving Boost Gauge mock at http://{args.host}:{args.port}")
    server.serve_forever()


if __name__ == "__main__":
    main()
