#!/usr/bin/env python3
"""Host mock for the Boost Gauge web control plane API."""

from __future__ import annotations

import argparse
import csv
import io
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
    },
    {
        "id": "vault-tec",
        "name": "Vault-Tec",
        "style": "vault",
        "colors": {
            "face": "#05281A",
            "track": "#0C3D24",
            "text": "#38F08A",
            "muted": "#1F7A4D",
            "vacuum": "#38F08A",
            "boost": "#38F08A",
            "overboost": "#EAFC50",
            "zero": "#38F08A",
        },
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
    },
    {
        "id": "neon",
        "name": "Neon",
        "style": "neon",
        "colors": {
            "face": "#000000", "track": "#241038", "text": "#FFFFFF",
            "muted": "#5A3A7A", "vacuum": "#8B3DFF", "boost": "#FF2BD6",
            "overboost": "#FF6A00", "zero": "#FFFFFF",
        },
    },
]

NEON_PRESETS = [
    {"track": "#241038", "muted": "#5A3A7A", "vacuum": "#8B3DFF", "boost": "#FF2BD6", "overboost": "#FF6A00"},
    {"track": "#10222E", "muted": "#3F6E80", "vacuum": "#00E5FF", "boost": "#FF2BD6", "overboost": "#FF2A00"},
    {"track": "#12300A", "muted": "#4C7A2E", "vacuum": "#39FF14", "boost": "#FFF000", "overboost": "#FF00A0"},
]

# Keep in step with BOOST_PXSHIFT_SEC_* in main/boost_theme.h.
PXSHIFT_SEC_MIN = 30
PXSHIFT_SEC_MAX = 3600
PXSHIFT_SEC_DEFAULT = 90

CONFIG = {
    "brightnessHigh": 100,
    "brightnessLow": 12,
    "dimSchedule": {"enabled": True, "startMinutes": 21 * 60, "endMinutes": 7 * 60},
    "timezoneOffsetMinutes": 0,
    "activeThemeId": "dyno-cell",
    "vaultNeedleRed": False,
    "vaultNeedleTail": False,
    "neonLayout": 1,
    "neonPreset": 0,
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

# ---------------------------------------------------------------- sensors ---
# Mirrors main/boost_sensors.c. The GM 12223861 curve is defined at 5.00 V and
# the sensor is ratiometric, so the configured supply is normalized out before
# the transfer function is applied:
#     normalized = map_volts * 5.00 / supply_volts
#     nominal    = 62.8721124 * normalized + 1.08216242
#     corrected  = nominal + offset_kpa
MAP_KPA_PER_VOLT = 62.8721124
MAP_KPA_INTERCEPT = 1.08216242
MAP_SUPPLY_MIN = 4.50
MAP_SUPPLY_MAX = 5.50
MAP_CAL_MAX_KPA = 10.0
KPA_TO_PSI = 0.145037738

SENSORS = {
    "supplyVolts": 5.20,
    # version 0 == never calibrated; the GET reports the rest as 0 in that case.
    "cal": {
        "version": 0,
        "samples": 0,
        "offsetKpa": 0.0,
        "supplyVolts": 5.20,
        "refMapVolts": 0.0,
        "refNominalKpa": 0.0,
        "refBmpKpa": 0.0,
        "epochMs": 0,
    },
    "bmpUpdates": 4000,
}

# Mock-only fault injection so the dashboard's error rendering can actually be
# exercised without hardware. Drive it with:
#   PUT /api/v1/mock/sensors  {"calFail": "no_bmp"}
#   POST /api/v1/sensors/calibration?fail=unstable_reading   (one-shot)
# `mapAgeMs`/`bmpAgeMs` accept -1 to exercise the "never read" rendering.
#
# `scanFail` and `stateFail` exist for the shared-#errorBox lifetime rules in
# web/app.js, which need the two producers to fail independently:
#   {"scanFail": "scan busy"}  -> GET /network/scan 503, a user-sourced error
#                                 that must survive the 4 Hz poll loop.
#   {"stateFail": true}        -> GET /state 503, a live-sourced error that must
#                                 clear itself once it is set back to false.
MOCK = {
    "calFail": None,
    "adsPresent": True,
    "bmpPresent": True,
    "ambientIsFallback": False,
    "fault": False,
    "mapAgeMs": None,
    "bmpAgeMs": None,
    "calDelaySec": 2.0,
    "scanFail": None,
    "stateFail": False,
}

CAL_ERROR_STATUS = {
    "no_ads": HTTPStatus.CONFLICT,
    "no_bmp": HTTPStatus.CONFLICT,
    "stale_reading": HTTPStatus.CONFLICT,
    "unstable_reading": HTTPStatus.CONFLICT,
    "implausible_pressure": HTTPStatus.CONFLICT,
    "correction_out_of_range": HTTPStatus.CONFLICT,
    "persist_failed": HTTPStatus.INTERNAL_SERVER_ERROR,
    "busy": HTTPStatus.CONFLICT,
}


def nominal_kpa(map_volts: float, supply_volts: float) -> float:
    normalized = map_volts * 5.0 / supply_volts
    return MAP_KPA_PER_VOLT * normalized + MAP_KPA_INTERCEPT


def volts_for_nominal(kpa: float, supply_volts: float) -> float:
    normalized = (kpa - MAP_KPA_INTERCEPT) / MAP_KPA_PER_VOLT
    return normalized * supply_volts / 5.0


def true_bmp_kpa() -> float:
    """Slowly drifting atmosphere so the live readouts visibly move."""
    return 98.58 + 0.04 * math.sin((time.time() - STARTED_AT) / 37.0)


def live_sensors() -> dict:
    """Live block of GET /api/v1/sensors/calibration.

    The simulated MAP sensor reads ~2.37 kPa low at atmosphere, which is what a
    one-point calibration is there to remove.
    """
    supply = float(SENSORS["supplyVolts"])
    bmp = true_bmp_kpa()
    elapsed = time.time() - STARTED_AT
    map_volts = volts_for_nominal(bmp - 2.37, supply) + 0.0004 * math.sin(elapsed * 3.1)
    nominal = nominal_kpa(map_volts, supply)
    offset = float(SENSORS["cal"]["offsetKpa"]) if SENSORS["cal"]["version"] else 0.0

    map_age = MOCK["mapAgeMs"] if MOCK["mapAgeMs"] is not None else int(elapsed * 1000) % 17
    bmp_age = MOCK["bmpAgeMs"] if MOCK["bmpAgeMs"] is not None else int(elapsed * 1000) % 160
    return {
        "adsPresent": bool(MOCK["adsPresent"]),
        "bmpPresent": bool(MOCK["bmpPresent"]),
        "fault": bool(MOCK["fault"]),
        "mapVolts": round(map_volts, 4),
        "mapAgeMs": int(map_age),
        "nominalKpa": round(nominal, 2),
        "correctedKpa": round(nominal + offset, 2),
        "bmpKpa": round(bmp, 2),
        "bmpAgeMs": int(bmp_age),
        "bmpUpdates": int(SENSORS["bmpUpdates"] + elapsed * 5),
        "ambientIsFallback": bool(MOCK["ambientIsFallback"]),
    }


def calibration_payload() -> dict:
    """Body of GET /api/v1/sensors/calibration, and of every successful write."""
    cal = SENSORS["cal"]
    valid = int(cal["version"]) != 0
    return {
        "supplyVolts": round(float(SENSORS["supplyVolts"]), 2),
        "live": live_sensors(),
        "calibration": {
            "valid": valid,
            "version": int(cal["version"]),
            "offsetKpa": round(float(cal["offsetKpa"]), 2) if valid else 0.0,
            "offsetPsi": round(float(cal["offsetKpa"]) * KPA_TO_PSI, 3) if valid else 0.0,
            "supplyVolts": round(float(cal["supplyVolts"]), 2) if valid else 0.0,
            "refMapVolts": round(float(cal["refMapVolts"]), 4) if valid else 0.0,
            "refNominalKpa": round(float(cal["refNominalKpa"]), 2) if valid else 0.0,
            "refBmpKpa": round(float(cal["refBmpKpa"]), 2) if valid else 0.0,
            "samples": int(cal["samples"]) if valid else 0,
            "epochMs": int(cal["epochMs"]) if valid else 0,
        },
    }


def run_calibration() -> tuple[dict, HTTPStatus]:
    """Validate, persist, activate — or fail and change nothing."""
    forced = MOCK["calFail"]
    if forced:
        return {"error": forced}, CAL_ERROR_STATUS.get(forced, HTTPStatus.CONFLICT)

    live = live_sensors()
    # The same gates the firmware applies, so the mock cannot "succeed" while
    # showing a state that would be rejected on hardware.
    if not live["adsPresent"]:
        return {"error": "no_ads"}, HTTPStatus.CONFLICT
    if not live["bmpPresent"] or live["ambientIsFallback"]:
        return {"error": "no_bmp"}, HTTPStatus.CONFLICT
    if live["mapAgeMs"] < 0 or live["bmpAgeMs"] < 0 or live["bmpAgeMs"] > 2000:
        return {"error": "stale_reading"}, HTTPStatus.CONFLICT

    supply = float(SENSORS["supplyVolts"])
    offset = live["bmpKpa"] - live["nominalKpa"]
    if abs(offset) > MAP_CAL_MAX_KPA:
        return {"error": "correction_out_of_range"}, HTTPStatus.CONFLICT

    SENSORS["cal"] = {
        "version": 1,
        "samples": 40,
        "offsetKpa": offset,
        "supplyVolts": supply,
        "refMapVolts": live["mapVolts"],
        "refNominalKpa": live["nominalKpa"],
        "refBmpKpa": live["bmpKpa"],
        "epochMs": TIME_ANCHOR_MS + int((time.time() - STARTED_AT) * 1000),
    }
    return calibration_payload(), HTTPStatus.OK


def set_supply_volts(value: object) -> tuple[dict, HTTPStatus]:
    try:
        volts = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return {"error": "invalid_supply"}, HTTPStatus.BAD_REQUEST
    if not math.isfinite(volts) or not MAP_SUPPLY_MIN <= volts <= MAP_SUPPLY_MAX:
        return {"error": "invalid_supply"}, HTTPStatus.BAD_REQUEST
    if MOCK["calFail"] == "persist_failed":
        return {"error": "persist_failed"}, HTTPStatus.INTERNAL_SERVER_ERROR

    SENSORS["supplyVolts"] = volts
    cal = SENSORS["cal"]
    if int(cal["version"]):
        # Recompute the offset from the stored reference under the new
        # normalization, preserving the same atmospheric reference rather than
        # carrying an offset derived under the old supply.
        cal["refNominalKpa"] = nominal_kpa(float(cal["refMapVolts"]), volts)
        cal["offsetKpa"] = float(cal["refBmpKpa"]) - float(cal["refNominalKpa"])
        cal["supplyVolts"] = volts
    return calibration_payload(), HTTPStatus.OK


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
    demo = bool(CONFIG.get("demoMode", False))
    # Mirror the firmware: in real mode the reading derives from a MAP sensor
    # against a BMP280 ambient. The mock fakes plausible raw values so the
    # dashboard's sensors panel has something to show.
    ambient_kpa = 101.3
    map_abs_kpa = ambient_kpa + psi / 0.145037738
    map_volts = round(map_abs_kpa * 0.0059 + 0.6, 4)
    payload = {
        "psi": psi,
        "peakPsi": round(PEAK, 1),
        "zone": zone_for(psi),
        "demo": demo,
        "brightness": CONFIG["brightnessHigh"],
        "firmwareVersion": "mock-v0.3.0-web",
        "uptimeMs": int((time.time() - STARTED_AT) * 1000),
        "epochMs": TIME_ANCHOR_MS + int((time.time() - STARTED_AT) * 1000),
        "timezoneOffsetMinutes": CONFIG["timezoneOffsetMinutes"],
        "activeThemeId": CONFIG["activeThemeId"],
        "sensors": {
            "adsPresent": not demo,
            "bmpPresent": not demo,
            "fault": False,
            "mapVolts": map_volts,
            "mapAbsKpa": round(map_abs_kpa, 2),
            "ambientKpa": ambient_kpa,
        },
    }
    # Mirror the firmware /state OBD section. With the BLE link enabled the
    # mock fakes a connected adapter and live PID readings so the dashboard's
    # OBD readout can be exercised without hardware.
    if bool(CONFIG.get("tpmsBle", False)):
        payload["obd"] = {
            "state": 3,
            "lastError": 0,
            "peer": "vlinker fd+",
            "peerAddr": "11:22:33:44:55:66",
            "uptimeMs": int((time.time() - STARTED_AT) * 1000),
            "ageMs": 120,
            "valid": True,
            "rpm": round(900 + 600 * abs(psi), 1),
            "speedKph": 0.0,
            "coolantC": 88.0,
            "mapKpa": 33.0 + 6.9 * psi,
            "iatC": 31.0,
            "throttlePct": 18.0,
            "mafGps": 5.2,
            "fuelPct": 62.0,
            "batteryV": 12.4,
        }
    else:
        payload["obd"] = {
            "state": 0,
            "lastError": 0,
            "peer": "",
            "peerAddr": "",
            "uptimeMs": 0,
            "ageMs": 0,
            "valid": False,
            "rpm": 0.0,
            "speedKph": 0.0,
            "coolantC": 0.0,
            "mapKpa": 0.0,
            "iatC": 0.0,
            "throttlePct": 0.0,
            "mafGps": 0.0,
            "fuelPct": 0.0,
            "batteryV": 0.0,
        }
    LOGS.append({"ts": payload["epochMs"], "psi": psi, "zone": payload["zone"], "demo": demo})
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
        if t["id"] == "neon":
            item["colors"] = {**t["colors"], **NEON_PRESETS[int(CONFIG.get("neonPreset", 0))]}
        item["customized"] = any(
            t["colors"][k] != THEME_DEFAULTS[t["id"]][k]
            for k in ("vacuum", "boost", "overboost")
        )
        out.append(item)
    return {
        "activeThemeId": CONFIG["activeThemeId"],
        "bigDigitStaticBg": bool(CONFIG.get("bigDigitStaticBg", False)),
        "pixelShift": bool(CONFIG.get("pixelShift", True)),
        "pixelShiftSec": int(CONFIG.get("pixelShiftSec", PXSHIFT_SEC_DEFAULT)),
        "bigDigitColorText": bool(CONFIG.get("bigDigitColorText", False)),
        "bigDigitStaticColor": str(CONFIG.get("bigDigitStaticColor", "#000000")),
        "bigDigitTextColor": str(CONFIG.get("bigDigitTextColor", "#ffffff")),
        "arcGradient": bool(CONFIG.get("arcGradient", False)),
        "hudGradient": bool(CONFIG.get("hudGradient", False)),
        "hudTrueBlack": bool(CONFIG.get("hudTrueBlack", False)),
        "neonMarqueeSpin": bool(CONFIG.get("neonMarqueeSpin", False)),
        "teSync": bool(CONFIG.get("teSync", False)),
        "teScanline": bool(CONFIG.get("teScanline", False)),
        "rotation": int(CONFIG.get("rotation", 0)),
        "demoMode": bool(CONFIG.get("demoMode", False)),
        "demoFastSweep": bool(CONFIG.get("demoFastSweep", False)),
        "tpmsBle": bool(CONFIG.get("tpmsBle", False)),
        "vaultFace": str(CONFIG.get("vaultFace", "#05281a")),
        "vaultVignette": int(CONFIG.get("vaultVignette", 60)),
        "vaultNeedleRed": bool(CONFIG.get("vaultNeedleRed", False)),
        "vaultNeedleTail": bool(CONFIG.get("vaultNeedleTail", False)),
        "neonLayout": int(CONFIG.get("neonLayout", 1)),
        "neonPreset": int(CONFIG.get("neonPreset", 0)),
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
            if MOCK["stateFail"]:
                self.send_json({"error": "state unavailable"}, HTTPStatus.SERVICE_UNAVAILABLE)
                return
            self.send_json(state_payload())
        elif path == "/api/v1/config":
            self.send_json(CONFIG)
        elif path == "/api/v1/themes":
            self.send_json(themes_payload())
        elif path == "/api/v1/themes/config":
            self.send_json(themes_payload())
        elif path == "/api/v1/logs":
            limit = int(parse_qs(parsed.query).get("limit", ["120"])[0])
            self.send_json({"sessionStartedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(STARTED_AT)), "samples": LOGS[-limit:]})
        elif path == "/api/v1/logs.csv":
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/csv")
            self.send_header("Content-Disposition", "attachment; filename=boost-gauge-log.csv")
            self.end_headers()
            text = self.wfile_text()
            writer = csv.DictWriter(text, fieldnames=["ts", "psi", "zone", "demo"])
            writer.writeheader()
            writer.writerows(LOGS)
            text.flush()
            text.detach()
        elif path == "/api/v1/media/status":
            self.send_json(MEDIA)
        elif path == "/api/v1/network":
            self.send_json(NETWORK)
        elif path == "/api/v1/network/scan":
            forced = parse_qs(parsed.query).get("fail", [None])[0] or MOCK["scanFail"]
            if forced:
                self.send_json({"error": forced}, HTTPStatus.SERVICE_UNAVAILABLE)
                return
            self.send_json({"networks": SCAN_NETWORKS})
        elif path == "/api/v1/sensors/calibration":
            self.send_json(calibration_payload())
        elif path == "/api/v1/sensors/scan":
            found = []
            if MOCK["adsPresent"]:
                found.append("0x48")
            if MOCK["bmpPresent"]:
                found.append("0x76")
            self.send_json({"busUp": True, "recoveries": 0, "found": found})
        elif path == "/api/v1/mock/sensors":
            self.send_json(MOCK)
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
        return io.TextIOWrapper(self.wfile, encoding="utf-8", newline="")

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
            if "pixelShiftSec" in payload:
                # Mirrors boost_web.c: reject what cannot be a period at all,
                # clamp what merely sits outside the supported band.
                try:
                    seconds = int(payload["pixelShiftSec"])
                except (TypeError, ValueError):
                    seconds = 0
                if not 1 <= seconds <= 86400:
                    self.send_json(
                        {"error": "invalid_pixel_shift_sec"}, HTTPStatus.BAD_REQUEST
                    )
                    return
                CONFIG["pixelShiftSec"] = max(
                    PXSHIFT_SEC_MIN, min(PXSHIFT_SEC_MAX, seconds)
                )
            if "bigDigitColorText" in payload:
                CONFIG["bigDigitColorText"] = bool(payload["bigDigitColorText"])
            if "bigDigitStaticColor" in payload:
                CONFIG["bigDigitStaticColor"] = str(payload["bigDigitStaticColor"])
            if "bigDigitTextColor" in payload:
                CONFIG["bigDigitTextColor"] = str(payload["bigDigitTextColor"])
            if "arcGradient" in payload:
                CONFIG["arcGradient"] = bool(payload["arcGradient"])
            if "hudGradient" in payload:
                CONFIG["hudGradient"] = bool(payload["hudGradient"])
            if "hudTrueBlack" in payload:
                CONFIG["hudTrueBlack"] = bool(payload["hudTrueBlack"])
            if "neonMarqueeSpin" in payload:
                CONFIG["neonMarqueeSpin"] = bool(payload["neonMarqueeSpin"])
            if "teSync" in payload:
                CONFIG["teSync"] = bool(payload["teSync"])
            if "teScanline" in payload:
                CONFIG["teScanline"] = bool(payload["teScanline"])
            if "rotation" in payload:
                # Quarter turns only, matching boost_theme_set_rotation().
                if payload["rotation"] not in (0, 90, 180, 270):
                    self.send_json({"error": "invalid_rotation"}, HTTPStatus.BAD_REQUEST)
                    return
                CONFIG["rotation"] = int(payload["rotation"])
            if "demoMode" in payload:
                CONFIG["demoMode"] = bool(payload["demoMode"])
            if "demoFastSweep" in payload:
                CONFIG["demoFastSweep"] = bool(payload["demoFastSweep"])
            if "tpmsBle" in payload:
                CONFIG["tpmsBle"] = bool(payload["tpmsBle"])
            if "vaultFace" in payload:
                CONFIG["vaultFace"] = str(payload["vaultFace"])
            if "vaultVignette" in payload:
                CONFIG["vaultVignette"] = int(payload["vaultVignette"])
            if "vaultNeedleRed" in payload and isinstance(payload["vaultNeedleRed"], bool):
                CONFIG["vaultNeedleRed"] = payload["vaultNeedleRed"]
            if "vaultNeedleTail" in payload and isinstance(payload["vaultNeedleTail"], bool):
                CONFIG["vaultNeedleTail"] = payload["vaultNeedleTail"]
            if "neonLayout" in payload and isinstance(payload["neonLayout"], int):
                if payload["neonLayout"] not in (0, 1, 2):
                    self.send_json({"error": "invalid_neon_layout"}, HTTPStatus.BAD_REQUEST)
                    return
                CONFIG["neonLayout"] = int(payload["neonLayout"])
            if "neonPreset" in payload and isinstance(payload["neonPreset"], int):
                if payload["neonPreset"] not in (0, 1, 2):
                    self.send_json({"error": "invalid_neon_preset"}, HTTPStatus.BAD_REQUEST)
                    return
                CONFIG["neonPreset"] = int(payload["neonPreset"])
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
        elif parsed.path == "/api/v1/sensors/supply":
            payload = self.read_json()
            body, status = set_supply_volts(payload.get("supplyVolts"))
            self.send_json(body, status)
        elif parsed.path == "/api/v1/mock/sensors":
            # Mock-only fault injection. No firmware equivalent.
            payload = self.read_json()
            for key in MOCK:
                if key in payload:
                    MOCK[key] = payload[key]
            self.send_json(MOCK)
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
        elif parsed.path == "/api/v1/sensors/calibration":
            self.rfile.read(length)  # body ignored, per the contract
            # The firmware observes sensor snapshots for ~2 s; the dashboard has
            # to hold a pending state for the whole window, so the mock blocks
            # for the same time rather than answering instantly.
            forced = parse_qs(parsed.query).get("fail", [None])[0]
            if forced:
                MOCK["calFail"] = forced
            time.sleep(float(MOCK["calDelaySec"]))
            body, status = run_calibration()
            if forced:
                MOCK["calFail"] = None
            self.send_json(body, status)
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
