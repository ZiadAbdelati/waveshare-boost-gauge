#!/usr/bin/env python3
"""Host mock for the Boost Gauge web control plane API.

Serves a faithful HTTP mirror of the firmware's boost_web.c surface so phone
apps and the dashboard can be exercised end-to-end without hardware:

  * Every /api/v1 endpoint boost_web.c registers: state, config, time, themes,
    page, tpms, sensors (scan/calibration/supply), logs (+ logs.csv), media,
    ota, restart, network (+ scan/reconnect), and OPTIONS/CORS handling.
  * JSON field names, value types, formatting precision, error bodies and
    status codes match boost_web.c - including quirks such as the wildcard
    asset handler answering unknown /api/v1 paths with 404 {"error":"not_found"}
    and GET /api/v1/themes/config not existing on the device.
  * Live behaviour is simulated: psi follows the demo waveform (organic sweep,
    or the constant-slew 9.789 psi/s triangle when demoFastSweep is on),
    uptimeMs advances, peakPsi ratchets, and the 5 Hz background log ring is
    synthesised on first request with the firmware's exact JSON/CSV shapes.

Mock-only extensions (no firmware equivalent, used by host dashboard tests):
GET/PUT /api/v1/mock/sensors (fault injection), the one-shot `fail` query on
POST /api/v1/sensors/calibration, and the `mediaDelaySec` fault that holds an
upload slot open so the 409 overlap guard can be exercised.

Import the server in-process (used by tools/test_mock_api.py):

    from mock_server import BoostMockServer
    server = BoostMockServer(port=0, seed=42, verbose=False).start()
    base = server.base_url

or run standalone:

    python3 tools/mock_server.py --host 127.0.0.1 --port 8080 --seed 42 --verbose
"""

from __future__ import annotations

import argparse
import io
import json
import math
import mimetypes
import threading
import time
from collections import deque
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread
from urllib.parse import parse_qs, urlparse


ROOT = Path(__file__).resolve().parents[1]
WEB_ROOT = ROOT / "web"

STARTED_AT = time.time()
TIME_ANCHOR_MS = int(STARTED_AT * 1000)
SEED: int | None = None

STATE_LOCK = threading.RLock()

# --------------------------------------------------------------- themes ----
# Order and palette values mirror main/boost_theme.c:s_defaults[] exactly
# (including 4 neon palettes - Violet, Miami, Toxic, Blood Moon - in
# s_neon_palettes[]). The web picker and physical swipes consume this order.
THEMES = [
    {
        "id": "dyno-cell",
        "name": "Dyno Cell",
        "style": "arc",
        "colors": {
            "face": "#090a0d",
            "track": "#20242c",
            "text": "#f5f7fa",
            "muted": "#8c95a3",
            "vacuum": "#4dd2ff",
            "boost": "#b8f35a",
            "overboost": "#ff4f6d",
            "zero": "#ffffff",
        },
    },
    {
        "id": "vault-tec",
        "name": "Vault-Tec",
        "style": "vault",
        "colors": {
            "face": "#05281a",
            "track": "#0c3d24",
            "text": "#38f08a",
            "muted": "#1f7a4d",
            "vacuum": "#38f08a",
            "boost": "#38f08a",
            "overboost": "#eafc50",
            "zero": "#38f08a",
        },
    },
    {
        "id": "night-city",
        "name": "Night City",
        "style": "hud",
        "colors": {
            "face": "#080a08",
            "track": "#1a1c0a",
            "text": "#fcee0a",
            "muted": "#5a7a0a",
            "vacuum": "#00e5ff",
            "boost": "#fcee0a",
            "overboost": "#ff003c",
            "zero": "#00e5ff",
        },
    },
    {
        "id": "big-digit",
        "name": "Big Digit",
        "style": "bigdigit",
        "colors": {
            "face": "#0b0c0e",
            "track": "#20242c",
            "text": "#ffffff",
            "muted": "#0b0c0e",
            "vacuum": "#4dd2ff",
            "boost": "#b8f35a",
            "overboost": "#ff4f6d",
            "zero": "#ffffff",
        },
    },
    {
        "id": "neon",
        "name": "Neon",
        "style": "neon",
        "colors": {
            "face": "#000000",
            "track": "#241038",
            "text": "#ffffff",
            "muted": "#5a3a7a",
            "vacuum": "#7b00ff",
            "boost": "#ff2bd6",
            "overboost": "#ff1500",
            "zero": "#ffffff",
        },
    },
]

# Boost theme s_neon_palettes[]: Violet, Miami, Toxic, Blood Moon.
NEON_PRESETS = [
    {"track": "#241038", "muted": "#5a3a7a", "vacuum": "#7b00ff", "boost": "#ff2bd6", "overboost": "#ff1500"},
    {"track": "#10222e", "muted": "#3f6e80", "vacuum": "#00e5ff", "boost": "#ff2bd6", "overboost": "#ff2a00"},
    {"track": "#12300a", "muted": "#4c7a2e", "vacuum": "#39ff14", "boost": "#fff000", "overboost": "#ff00a0"},
    {"track": "#0c1440", "muted": "#35509e", "vacuum": "#0064ff", "boost": "#c4172e", "overboost": "#ff6a00"},
]

# Compiled-in theme palette snapshot: reset() restores from here, and the
# `customized` flag for non-neon themes compares against it.
THEME_DEFAULTS = {entry["id"]: dict(entry["colors"]) for entry in THEMES}

# Keep in step with BOOST_PXSHIFT_SEC_* in main/boost_theme.h.
PXSHIFT_SEC_MIN = 30
PXSHIFT_SEC_MAX = 3600
PXSHIFT_SEC_DEFAULT = 90

# Firmware defaults (boost_model.c defaults(), boost_theme.c statics).
CONFIG = {
    "brightnessHigh": 92,
    "brightnessLow": 18,
    "dimSchedule": {"enabled": False, "startMinutes": 21 * 60, "endMinutes": 7 * 60},
    "timezoneOffsetMinutes": 0,
    "timezoneTz": "",
    "activeThemeId": "dyno-cell",
    "psiMin": -15.0,
    "psiMax": 10.0,
    "psiOverboost": 8.0,
    "zeroAngle": 236.25,
    # NEW phone-app setting (concurrent boost_web.c work): in-memory only.
    "appBle": False,
}

THEME = {
    "bigDigitStaticBg": False,
    "bigDigitColorText": False,
    "bigDigitStaticColor": "#000000",
    "bigDigitTextColor": "#ffffff",
    "arcGradient": False,
    "hudGradient": False,
    "hudTrueBlack": False,
    "neonMarqueeSpin": False,
    "teSync": False,
    "regionDBuf": False,
    "teScanline": False,
    "rotation": 0,
    "vaultFace": "#05281a",
    "vaultVignette": 60,
    "vaultNeedleRed": False,
    "vaultNeedleTail": False,
    "neonLayout": 1,
    "neonPreset": 0,
    "demoMode": False,
    "demoFastSweep": False,
    "tpmsBle": False,
    "pixelShift": True,
    "pixelShiftSec": PXSHIFT_SEC_DEFAULT,
}

MEDIA = {
    "present": False,
    "name": "active.gif",          # boost_web.c hard-codes this name
    "size": 0,
    "uploadedAtMs": 0,
    "playbackSupported": False,
    "playback": "unavailable",
}

NETWORK = {
    "mode": "apsta",
    "staEnabled": True,
    "staConnected": True,
    "staSsid": "PitWall-5G",
    "staIp": "192.168.1.100",
    "apSsid": "BoostGauge-7F3A",
    "apIp": "192.168.4.1",
    "rssi": -54,
    "hasPassword": True,
    "saved": [
        {"ssid": "PitWall-5G"},
        {"ssid": "Garage-IoT"},
    ],
}

SCAN_NETWORKS = [
    {"ssid": "PitWall-5G", "rssi": -54, "auth": 3},
    {"ssid": "Paddock-Guest", "rssi": -67, "auth": 0},
    {"ssid": "Garage-IoT", "rssi": -72, "auth": 3},
]

# Mock-only fault injection for host dashboard tests; see module docstring.
MOCK = {
    "calFail": None,
    "adsPresent": True,
    "bmpPresent": True,
    "ambientIsFallback": False,
    "fault": False,
    "mapAgeMs": None,
    "bmpAgeMs": None,
    "calDelaySec": 2.0,
    "mediaDelaySec": 0.1,
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

LOGS: deque = deque(maxlen=18000)
# True once the full 1-hour ring has been synthesised (a live append before the
# first /logs request must not suppress ring synthesis; DELETE keeps it True).
LOGS_RING_BUILT = False

PEAK = 0.0
ACTIVE_PAGE = 0
MEDIA_UPLOAD_IN_PROGRESS = False

# boost_media_store.h
MAX_GIF_BYTES = 3500 * 1024
MAX_GIF_DIMENSION = 466
# boost_model.h / boost_web.c
MAX_JSON_BODY = 4096
# OTA streams the full app image over HTTP; the 4 KB JSON cap does not apply.
MAX_OTA_BYTES = 64 * 1024 * 1024
LOG_CAPACITY = 18000
BOOST_RTC_EPOCH_MIN_MS = 1700000000000
# Synthetic log ring: a virtual one-hour session (18000 x 200 ms) that ends at
# "now", so every row has a distinct monotonic timestamp and CSV epoch values
# land in the past. Ring time 0 == TIME_ANCHOR_MS - 1 hour.
RING_BASE_MS = LOG_CAPACITY * 200
RING_START_EPOCH_MS = TIME_ANCHOR_MS - RING_BASE_MS
# boost_theme.c / boost_sim.c demo constants
FAST_SWEEP_MIN = -14.5
FAST_SWEEP_MAX = 10.0
FAST_SWEEP_SLEW_PSI_PER_S = 9.789
KPA_TO_PSI = 0.145037738

# ---------------------------------------------------------------- sensors ---
# Mirrors main/boost_sensors.c. The GM 12223861 curve is defined at 5.00 V and
# the sensor is ratiometric, so the configured supply is normalized out before
# the transfer function is applied:
#     normalized = map_volts * 5.00 / supply_volts
#     nominal    = 62.8721124 * normalized + 1.08216242
MAP_KPA_PER_VOLT = 62.8721124
MAP_KPA_INTERCEPT = 1.08216242
MAP_SUPPLY_MIN = 4.50
MAP_SUPPLY_MAX = 5.50
MAP_CAL_MAX_KPA = 10.0

SENSORS = {
    "supplyVolts": 5.20,
    # version 0 == never calibrated; reports rest as 0 in that case.
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


def reset_mock_state(seed: int | None = None) -> None:
    """Restore every simulation variable to its fresh-boot default."""
    global STARTED_AT, TIME_ANCHOR_MS, SEED
    global CONFIG, THEME, MEDIA, NETWORK, SCAN_NETWORKS, MOCK, SENSORS
    global LOGS, LOGS_RING_BUILT, PEAK, ACTIVE_PAGE, MEDIA_UPLOAD_IN_PROGRESS
    global RING_START_EPOCH_MS
    STARTED_AT = time.time()
    TIME_ANCHOR_MS = int(STARTED_AT * 1000)
    SEED = seed
    RING_START_EPOCH_MS = TIME_ANCHOR_MS - RING_BASE_MS
    CONFIG = {
        "brightnessHigh": 92,
        "brightnessLow": 18,
        "dimSchedule": {"enabled": False, "startMinutes": 21 * 60, "endMinutes": 7 * 60},
        "timezoneOffsetMinutes": 0,
        "timezoneTz": "",
        "activeThemeId": "dyno-cell",
        "psiMin": -15.0,
        "psiMax": 10.0,
        "psiOverboost": 8.0,
        "zeroAngle": 236.25,
        "appBle": False,
    }
    THEME = {
        "bigDigitStaticBg": False,
        "bigDigitColorText": False,
        "bigDigitStaticColor": "#000000",
        "bigDigitTextColor": "#ffffff",
        "arcGradient": False,
        "hudGradient": False,
        "hudTrueBlack": False,
        "neonMarqueeSpin": False,
        "teSync": False,
        "regionDBuf": False,
        "teScanline": False,
        "rotation": 0,
        "vaultFace": "#05281a",
        "vaultVignette": 60,
        "vaultNeedleRed": False,
        "vaultNeedleTail": False,
        "neonLayout": 1,
        "neonPreset": 0,
        "demoMode": False,
        "demoFastSweep": False,
        "tpmsBle": False,
        "pixelShift": True,
        "pixelShiftSec": PXSHIFT_SEC_DEFAULT,
    }
    # Deep-copy the compiled-in theme table so per-request color overrides do
    # not bleed across reset() calls (THEMES entries are mutated in place).
    for entry in THEMES:
        entry["colors"] = dict(THEME_DEFAULTS[entry["id"]])
    MEDIA = {
        "present": False,
        "name": "active.gif",
        "size": 0,
        "uploadedAtMs": 0,
        "playbackSupported": False,
        "playback": "unavailable",
    }
    NETWORK = {
        "mode": "apsta",
        "staEnabled": True,
        "staConnected": True,
        "staSsid": "PitWall-5G",
        "staIp": "192.168.1.100",
        "apSsid": "BoostGauge-7F3A",
        "apIp": "192.168.4.1",
        "rssi": -54,
        "hasPassword": True,
        "saved": [{"ssid": "PitWall-5G"}, {"ssid": "Garage-IoT"}],
    }
    SCAN_NETWORKS = [
        {"ssid": "PitWall-5G", "rssi": -54, "auth": 3},
        {"ssid": "Paddock-Guest", "rssi": -67, "auth": 0},
        {"ssid": "Garage-IoT", "rssi": -72, "auth": 3},
    ]
    MOCK = {
        "calFail": None,
        "adsPresent": True,
        "bmpPresent": True,
        "ambientIsFallback": False,
        "fault": False,
        "mapAgeMs": None,
        "bmpAgeMs": None,
        "calDelaySec": 2.0,
        "mediaDelaySec": 0.1,
        "scanFail": None,
        "stateFail": False,
    }
    SENSORS = {
        "supplyVolts": 5.20,
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
    LOGS = deque(maxlen=LOG_CAPACITY)
    LOGS_RING_BUILT = False
    PEAK = 0.0
    ACTIVE_PAGE = 0
    MEDIA_UPLOAD_IN_PROGRESS = False


def uptime_ms() -> int:
    """Monotonic uptime (firmware esp_timer); unaffected by POST /time."""
    return int((time.time() - STARTED_AT) * 1000)


def now_epoch_ms() -> int:
    return TIME_ANCHOR_MS + uptime_ms()


def seeded_noise(elapsed: float) -> float:
    """Deterministic per-seed flutter; 0.0 when no --seed was given."""
    if SEED is None:
        return 0.0
    bucket = int(elapsed * 4)
    value = (hash((SEED, bucket)) % 2001 - 1000) / 10000.0
    return value


def waveform_psi_at(elapsed: float) -> float:
    """Demo/sim waveform at `elapsed` seconds since server start.

    Mirrors boost_sim_tick(): the constant-slew triangle when the persisted
    demoFastSweep flag is set (only meaningful with demoMode on), otherwise the
    organic layered-sine sweep. The mock maps the organic sweep across the
    configured gauge range so the full dial is exercised.
    """
    if THEME.get("demoMode") and THEME.get("demoFastSweep"):
        period = 2.0 * (FAST_SWEEP_MAX - FAST_SWEEP_MIN) / FAST_SWEEP_SLEW_PSI_PER_S
        tt = math.fmod(elapsed, period)
        half = period * 0.5
        frac = (tt / half) if tt < half else (2.0 - tt / half)
        psi = FAST_SWEEP_MIN + (FAST_SWEEP_MAX - FAST_SWEEP_MIN) * frac
    else:
        phase = elapsed * ((2 * math.pi) / 7.5)
        envelope = 0.55 + 0.45 * math.sin(phase * 0.5 + 0.4)
        norm = (0.5 + 0.5 * math.sin(phase)) * envelope
        norm = norm * 0.92 + 0.08
        psi_min = float(CONFIG.get("psiMin", -15.0))
        psi_max = float(CONFIG.get("psiMax", 10.0))
        psi = psi_min + (psi_max - psi_min) * norm
        psi += 0.18 * math.sin(elapsed * 17.3) + 0.08 * math.sin(elapsed * 31.1)
    psi += seeded_noise(elapsed)
    return psi


def current_psi() -> float:
    elapsed = (time.time() - STARTED_AT)
    psi_min = float(CONFIG.get("psiMin", -15.0))
    psi_max = float(CONFIG.get("psiMax", 10.0))
    return round(max(psi_min, min(psi_max, waveform_psi_at(elapsed))), 2)


def zone_for(psi: float) -> str:
    overboost = float(CONFIG.get("psiOverboost", 8.0))
    if psi >= overboost:
        return "OVER"
    if psi >= 0.35:
        return "BOOST"
    if psi > -0.35:
        return "ATMO"
    return "VAC"


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
        "supplyVolts": round(float(SENSORS["supplyVolts"]), 4),
        "live": live_sensors(),
        "calibration": {
            "valid": valid,
            "version": int(cal["version"]) if valid else 0,
            "offsetKpa": round(float(cal["offsetKpa"]), 2) if valid else 0.0,
            "offsetPsi": round(float(cal["offsetKpa"]) * KPA_TO_PSI, 3) if valid else 0.0,
            "supplyVolts": round(float(cal["supplyVolts"]), 4) if valid else 0.0,
            "refMapVolts": round(float(cal["refMapVolts"]), 4) if valid else 0.0,
            "refNominalKpa": round(float(cal["refNominalKpa"]), 2) if valid else 0.0,
            "refBmpKpa": round(float(cal["refBmpKpa"]), 2) if valid else 0.0,
            "samples": int(cal["samples"]) if valid else 0,
            "epochMs": int(cal["epochMs"]) if valid else 0,
        },
    }


def run_calibration() -> tuple[dict, HTTPStatus]:
    """Validate, persist, activate - or fail and change nothing."""
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
        "epochMs": now_epoch_ms(),
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


# ----------------------------------------------------------------- state ----
def display_metrics() -> dict:
    """Healthy physical-display metrics block for /state."""
    return {
        "renderFps": 60,
        "gaugeDemandPerSecond": 60,
        "flushesPerSecond": 60,
        "pixelsPerSecond": 1000000,
        "worstRenderUs": 8500,
        "renderGapP50Us": 400,
        "renderGapMaxUs": 20000,
        "framesOverBudget": 0,
        "tePeriodUs": 16667,
        "teWaits": 60,
        "teTimeouts": 0,
        "teSkips": 0,
        "teScanlineWaits": 0,
    }


def obd_block() -> dict:
    """/state obd block; mirrors boost_model_publish_obd + web state_json."""
    if bool(THEME.get("tpmsBle")):
        psi = current_psi()
        return {
            "state": 3,
            "lastError": 0,
            "peer": "vlinker fd+",
            "peerAddr": "11:22:33:44:55:66",
            "uptimeMs": uptime_ms(),
            "ageMs": 120,
            "valid": True,
            "lastReply": "010C='410C 0A 00'",
            "protocol": "ISO 15765-4 (CAN 11/500)",
            "rpm": round(900 + 600 * abs(psi), 1),
            "speedKph": 0.0,
            "coolantC": 88.0,
            "mapKpa": round(33.0 + 6.9 * psi, 1),
            "iatC": 31.0,
            "throttlePct": 18.0,
            "mafGps": 5.2,
            "fuelPct": 62.0,
            "batteryV": 12.4,
        }
    return {
        "state": 0,
        "lastError": 0,
        "peer": "",
        "peerAddr": "",
        "uptimeMs": 0,
        "ageMs": 0,
        "valid": False,
        "lastReply": "",
        "protocol": "",
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


def state_payload() -> dict:
    """Full /api/v1/state body; names/types mirror boost_web.c:state_json."""
    global PEAK
    psi = current_psi()
    PEAK = max(PEAK, psi)
    demo = bool(THEME.get("demoMode", False))
    epoch = now_epoch_ms()
    ambient_kpa = 101.3
    map_abs_kpa = ambient_kpa + psi / KPA_TO_PSI
    map_volts = round(map_abs_kpa * 0.0059 + 0.6, 4)
    if demo:
        # boost_sim_tick() zero-fills the raw sensor block on the demo path.
        sensors = {
            "adsPresent": False,
            "bmpPresent": False,
            "fault": False,
            "mapVolts": 0.0,
            "mapAbsKpa": 0.0,
            "ambientKpa": 0.0,
        }
    else:
        sensors = {
            "adsPresent": True,
            "bmpPresent": True,
            "fault": False,
            "mapVolts": map_volts,
            "mapAbsKpa": round(map_abs_kpa, 2),
            "ambientKpa": ambient_kpa,
        }
    low_kpa = float(CONFIG.get("tpmsLowKpa", 220.0))
    low_psi = round(low_kpa * KPA_TO_PSI, 1)
    payload = {
        "psi": psi,
        "peakPsi": round(PEAK, 2),
        "zone": zone_for(psi),
        "demo": demo,
        "brightness": int(CONFIG["brightnessHigh"]),
        "firmwareVersion": "v0.8.0",
        "uptimeMs": uptime_ms(),
        "epochMs": epoch,
        "timezoneOffsetMinutes": int(CONFIG["timezoneOffsetMinutes"]),
        "activeThemeId": CONFIG["activeThemeId"],
        "activePage": int(ACTIVE_PAGE),
        "display": display_metrics(),
        "sensors": sensors,
        "tpms": {
            "status": 0,
            "lowPsi": low_psi,
            "wheels": [
                {"psi": 32.5, "valid": True},
                {"psi": 33.0, "valid": True},
                {"psi": 31.8, "valid": True},
                {"psi": 32.8, "valid": True},
            ],
        },
        "obd": obd_block(),
    }
    append_live_log(psi, payload["zone"], demo)
    return payload


# ------------------------------------------------------------------ logs ----
def log_at(t_ms: int, peak: float) -> dict:
    """Synthesise one ring row from its monotonic timestamp (uptime ms)."""
    elapsed = t_ms / 1000.0
    psi = waveform_for_ring(elapsed)
    return {
        "tMs": int(t_ms),
        "psi": psi,
        "peakPsi": round(peak, 2),
        "zone": zone_for(psi),
        "demo": bool(THEME.get("demoMode", False)),
    }


def waveform_for_ring(elapsed: float) -> float:
    """psi for a ring row without ratcheting the live PEAK."""
    psi_min = float(CONFIG.get("psiMin", -15.0))
    psi_max = float(CONFIG.get("psiMax", 10.0))
    psi = waveform_psi_at(elapsed)
    return round(max(psi_min, min(psi_max, psi)), 2)


def ensure_logs() -> None:
    """Synthesise the 1-hour, 5 Hz background ring once, ending at now."""
    global LOGS_RING_BUILT
    if LOGS_RING_BUILT:
        return
    LOGS_RING_BUILT = True
    LOGS.clear()
    # A device already up for an hour: 18000 distinct 200 ms rows ending now,
    # with the demo peak ratcheting across the session the way the sim does.
    peak = float("-inf")
    for i in range(LOG_CAPACITY):
        psi = waveform_for_ring(((i + 1) * 200) / 1000.0)
        peak = max(peak, psi)
        LOGS.append(log_at((i + 1) * 200, peak))


def append_live_log(psi: float, zone: str, demo: bool) -> None:
    """Append a row at most every 200 ms, mirroring the background logger."""
    ring_now = RING_BASE_MS + uptime_ms()
    if LOGS and ring_now - int(LOGS[-1]["tMs"]) < 200:
        return
    LOGS.append(
        {
            "tMs": int(ring_now),
            "psi": psi,
            "peakPsi": round(PEAK, 2),
            "zone": zone,
            "demo": demo,
        }
    )


def logs_json_payload(limit: int) -> dict:
    ensure_logs()
    return {"samples": list(LOGS)[-limit:]}


def logs_csv_text() -> str:
    """Full CSV export; header/columns mirror boost_web.c:logs_csv_get."""
    ensure_logs()
    offset_min = int(CONFIG["timezoneOffsetMinutes"])
    out = io.StringIO()
    out.write("timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo\n")
    for row in LOGS:
        epoch_ms = RING_START_EPOCH_MS + int(row["tMs"])
        timestamp = ""
        if epoch_ms > 0:
            timestamp = time.strftime(
                "%Y-%m-%dT%H:%M:%S", time.gmtime(epoch_ms / 1000.0 + offset_min * 60)
            )
        out.write(
            f"{timestamp},{offset_min},{epoch_ms},{int(row['tMs'])},"
            f"{row['psi']:.2f},{row['peakPsi']:.2f},{row['zone']},"
            f"{1 if row['demo'] else 0}\n"
        )
    return out.getvalue()


# ---------------------------------------------------------------- themes ----
def theme_color_dict(theme: dict) -> dict:
    """Effective palette: compiled-in colors, plus the active neon preset."""
    colors = dict(theme["colors"])
    if theme["id"] == "neon":
        base = NEON_PRESETS[int(THEME["neonPreset"]) % len(NEON_PRESETS)]
        colors.update({k: base[k] for k in ("track", "muted")})
        for key in ("vacuum", "boost", "overboost"):
            colors[key] = theme["colors"].get(key, base[key])
    return colors


def theme_is_customized(theme: dict) -> bool:
    if theme["id"] == "neon":
        zones = NEON_PRESETS[int(THEME["neonPreset"]) % len(NEON_PRESETS)]
    else:
        zones = THEME_DEFAULTS[theme["id"]]
    return (theme["colors"].get("vacuum") != zones["vacuum"]
            or theme["colors"].get("boost") != zones["boost"]
            or theme["colors"].get("overboost") != zones["overboost"])


def themes_payload() -> dict:
    """Body of GET /api/v1/themes; key order mirrors boost_web.c exactly."""
    out = []
    for t in THEMES:
        out.append(
            {
                "id": t["id"],
                "name": t["name"],
                "style": t["style"],
                "colors": theme_color_dict(t),
                "customized": theme_is_customized(t),
            }
        )
    return {
        "activeThemeId": CONFIG["activeThemeId"],
        "bigDigitStaticBg": bool(THEME["bigDigitStaticBg"]),
        "bigDigitColorText": bool(THEME["bigDigitColorText"]),
        "bigDigitStaticColor": str(THEME["bigDigitStaticColor"]),
        "bigDigitTextColor": str(THEME["bigDigitTextColor"]),
        "arcGradient": bool(THEME["arcGradient"]),
        "hudGradient": bool(THEME["hudGradient"]),
        "hudTrueBlack": bool(THEME["hudTrueBlack"]),
        "neonMarqueeSpin": bool(THEME["neonMarqueeSpin"]),
        "teSync": bool(THEME["teSync"]),
        "regionDBuf": bool(THEME["regionDBuf"]),
        "teScanline": bool(THEME["teScanline"]),
        "rotation": int(THEME["rotation"]),
        "vaultFace": str(THEME["vaultFace"]),
        "vaultVignette": int(THEME["vaultVignette"]),
        "vaultNeedleRed": bool(THEME["vaultNeedleRed"]),
        "vaultNeedleTail": bool(THEME["vaultNeedleTail"]),
        "neonLayout": int(THEME["neonLayout"]),
        "neonPreset": int(THEME["neonPreset"]),
        "demoMode": bool(THEME["demoMode"]),
        "demoFastSweep": bool(THEME["demoFastSweep"]),
        "tpmsBle": bool(THEME["tpmsBle"]),
        "pixelShift": bool(THEME["pixelShift"]),
        "pixelShiftSec": int(THEME["pixelShiftSec"]),
        "themes": out,
    }


def parse_hex_color(value: object) -> str | None:
    """'#rrggbb' or 'rrggbb' -> normalized '#rrggbb'; None when invalid."""
    if not isinstance(value, str):
        return None
    p = value[1:] if value.startswith("#") else value
    if len(p) != 6:
        return None
    try:
        rgb = int(p, 16)
    except ValueError:
        return None
    if rgb < 0 or rgb > 0xFFFFFF:
        return None
    return f"#{rgb:06x}"


def tpms_config_payload() -> dict:
    low_kpa = float(CONFIG.get("tpmsLowKpa", 220.0))
    return {
        "lowKpa": round(low_kpa, 1),
        "lowPsi": round(low_kpa * KPA_TO_PSI, 1),
        "staleAfterMs": int(CONFIG.get("tpmsStaleAfterMs", 15000)),
    }


def media_status_payload() -> dict:
    """Body of GET/POST/DELETE /media responses (boost_web.c:media_status_get)."""
    return {
        "present": bool(MEDIA["present"]),
        "name": "active.gif",
        "size": int(MEDIA["size"]),
        "uploadedAtMs": int(MEDIA["uploadedAtMs"]),
        "playbackSupported": bool(MEDIA["present"]),
        "playback": "active" if MEDIA["present"] else "unavailable",
    }


# ---------------------------------------------------------------- server ----
class Handler(BaseHTTPRequestHandler):
    server_version = "BoostGaugeMock/1.0"
    # Request logging is opt-in via --verbose (or BoostMockServer(verbose=True)).
    verbose = False

    def log_message(self, fmt: str, *args: object) -> None:
        if Handler.verbose:
            print(f"{self.address_string()} - {fmt % args}")

    # -- shared plumbing ------------------------------------------------
    def send_payload(
        self,
        body: bytes,
        content_type: str,
        status: int = HTTPStatus.OK,
        extra_headers: dict[str, str] | None = None,
        cors: bool = True,
    ) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        if cors:
            # Mirrors set_common_headers() in boost_web.c.
            self.send_header("Access-Control-Allow-Origin", "*")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET,PUT,POST,DELETE,OPTIONS")
            self.send_header("Cache-Control", "no-store")
        for key, value in (extra_headers or {}).items():
            self.send_header(key, value)
        self.end_headers()
        if body:
            self.wfile.write(body)

    def send_json(self, payload: object, status: int = HTTPStatus.OK) -> None:
        self.send_payload(json.dumps(payload).encode(), "application/json", status)

    def send_err(self, status: int, msg: str) -> None:
        """Mirror send_err(): body is exactly {"error":"<msg>"}."""
        self.send_json({"error": msg}, status)

    def read_body(self, max_len: int = MAX_JSON_BODY) -> tuple[bytes | None, str | None]:
        """Read the request body. Returns (body, None) or (None, error_msg)."""
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            return None, "invalid_body"
        if length < 0:
            return None, "invalid_body"
        if length > max_len:
            return None, "invalid_body"
        if length == 0:
            return b"", None
        body = self.rfile.read(length)
        return body, None

    def read_json(self) -> tuple[dict | None, str | None]:
        body, err = self.read_body()
        if err is not None:
            return None, err
        try:
            parsed = json.loads(body.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            return None, "invalid_json"
        if not isinstance(parsed, dict):
            return None, "invalid_json"
        return parsed, None

    # -- GET ------------------------------------------------------------
    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if path == "/api/v1/state":
            if MOCK["stateFail"]:
                # Mock-only fault injection (no firmware equivalent).
                self.send_err(HTTPStatus.SERVICE_UNAVAILABLE, "state unavailable")
                return
            self.send_json(state_payload())
        elif path == "/api/v1/config":
            self.send_json(CONFIG)
        elif path == "/api/v1/themes":
            self.send_json(themes_payload())
        elif path == "/api/v1/tpms/config":
            self.send_json(tpms_config_payload())
        elif path == "/api/v1/logs":
            limit = self.log_limit(parsed)
            self.send_json(logs_json_payload(limit))
        elif path == "/api/v1/logs.csv":
            self.send_payload(
                logs_csv_text().encode(),
                "text/csv",
                extra_headers={"Content-Disposition": 'attachment; filename="boost-gauge-log.csv"'},
            )
        elif path == "/api/v1/media/status":
            self.send_json(media_status_payload())
        elif path == "/api/v1/network":
            self.send_json(self.network_status_payload())
        elif path == "/api/v1/network/scan":
            forced = parse_qs(parsed.query).get("fail", [None])[0] or MOCK["scanFail"]
            if forced:
                # boost_web.c:network_scan_get() -> 400 scan_failed.
                self.send_err(HTTPStatus.BAD_REQUEST, "scan_failed")
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
        else:
            # The firmware routes unknown paths to the wildcard asset handler,
            # which answers 404 {"error":"not_found"}.
            self.serve_static_or_404(path)

    def log_limit(self, parsed) -> int:
        limit = 300
        raw = parse_qs(parsed.query).get("limit", [""])[0]
        try:
            parsed_limit = int(raw)
        except (TypeError, ValueError):
            parsed_limit = 0
        if parsed_limit > 0:
            limit = min(parsed_limit, LOG_CAPACITY)
        return limit

    def network_status_payload(self) -> dict:
        return {
            "mode": NETWORK["mode"],
            "staEnabled": bool(NETWORK["staEnabled"]),
            "staConnected": bool(NETWORK["staConnected"]),
            "staSsid": NETWORK["staSsid"],
            "staIp": NETWORK["staIp"],
            "apSsid": NETWORK["apSsid"],
            "apIp": NETWORK["apIp"],
            "rssi": int(NETWORK["rssi"]),
            "hasPassword": bool(NETWORK["hasPassword"]),
            "saved": [dict(item) for item in NETWORK["saved"]],
        }

    def serve_static_or_404(self, path: str) -> None:
        if path == "/":
            path = "/index.html"
        target = (WEB_ROOT / path.lstrip("/")).resolve()
        if WEB_ROOT not in target.parents and target != WEB_ROOT:
            self.send_err(HTTPStatus.FORBIDDEN, "bad_path")
            return
        if not target.is_file():
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")
            return
        body = target.read_bytes()
        content_type = mimetypes.guess_type(target.name)[0] or "application/octet-stream"
        # Firmware static assets are gzipped with ETag and no CORS headers
        # (root_get); the host mock serves the plain files instead.
        self.send_payload(body, content_type, cors=False)

    def do_HEAD(self) -> None:
        parsed = urlparse(self.path)
        path = "/index.html" if parsed.path == "/" else parsed.path
        target = (WEB_ROOT / path.lstrip("/")).resolve()
        if WEB_ROOT not in target.parents and target != WEB_ROOT:
            self.send_err(HTTPStatus.FORBIDDEN, "bad_path")
            return
        if not target.is_file():
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")
            return
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", mimetypes.guess_type(target.name)[0] or "application/octet-stream")
        self.send_header("Content-Length", str(target.stat().st_size))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()

    # -- OPTIONS ---------------------------------------------------------
    def do_OPTIONS(self) -> None:
        # boost_web.c options_handler(): text/plain + common headers, empty body.
        self.send_payload(b"", "text/plain", cors=True)

    # -- PUT -------------------------------------------------------------
    def do_PUT(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/config":
            self.handle_config_put()
        elif parsed.path == "/api/v1/themes/active":
            self.handle_theme_active_put()
        elif parsed.path == "/api/v1/themes/config":
            self.handle_themes_config_put()
        elif parsed.path == "/api/v1/sensors/supply":
            self.handle_supply_put()
        elif parsed.path == "/api/v1/mock/sensors":
            payload, err = self.read_json()
            if err is not None:
                self.send_err(HTTPStatus.BAD_REQUEST, err)
                return
            for key in MOCK:
                if key in payload:  # type: ignore[operator]
                    MOCK[key] = payload[key]  # type: ignore[index]
            self.send_json(MOCK)
        elif parsed.path == "/api/v1/network":
            self.handle_network_put()
        elif parsed.path == "/api/v1/tpms/config":
            self.handle_tpms_config_put()
        elif parsed.path == "/api/v1/page":
            self.handle_page_put()
        else:
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")

    def handle_config_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return

        # Validate the gauge range first: any invalid value is a whole-body
        # 400 invalid_config, matching boost_model_update_config()'s gate.
        psi_min = float(payload.get("psiMin", CONFIG["psiMin"]))
        psi_max = float(payload.get("psiMax", CONFIG["psiMax"]))
        psi_overboost = float(payload.get("psiOverboost", CONFIG["psiOverboost"]))
        zero_angle = float(payload.get("zeroAngle", CONFIG["zeroAngle"]))
        range_keys = ("psiMin", "psiMax", "psiOverboost", "zeroAngle")
        if any(key in payload for key in range_keys) and not (
            math.isfinite(psi_min) and math.isfinite(psi_max)
            and math.isfinite(psi_overboost) and math.isfinite(zero_angle)
            and -30.0 <= psi_min <= -1.0
            and 5.0 <= psi_max <= 40.0
            and 0.0 < psi_overboost < psi_max
            and 180.0 <= zero_angle <= 315.0
        ):
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_config")
            return
        if "activeThemeId" in payload and not any(
            theme["id"] == payload["activeThemeId"] for theme in THEMES
        ):
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_config")
            return

        patch = dict(CONFIG)
        for key in ("brightnessHigh", "brightnessLow"):
            if key in payload and isinstance(payload[key], (int, float)):
                value = int(payload[key])
                patch[key] = max(0, min(100, value))  # clamp_percent()
        if "timezoneOffsetMinutes" in payload and isinstance(payload["timezoneOffsetMinutes"], (int, float)):
            tz = int(payload["timezoneOffsetMinutes"])
            patch["timezoneOffsetMinutes"] = max(-14 * 60, min(14 * 60, tz))
        if "timezoneTz" in payload and isinstance(payload["timezoneTz"], str):
            patch["timezoneTz"] = payload["timezoneTz"]
        if "activeThemeId" in payload:
            patch["activeThemeId"] = payload["activeThemeId"]
        sched = payload.get("dimSchedule")
        if isinstance(sched, dict):
            merged = dict(patch["dimSchedule"])
            if isinstance(sched.get("enabled"), bool):
                merged["enabled"] = sched["enabled"]
            for key in ("startMinutes", "endMinutes"):
                if key in sched and isinstance(sched[key], (int, float)):
                    value = int(sched[key]) % (24 * 60)
                    merged[key] = value  # normalize_minutes()
            patch["dimSchedule"] = merged
        if any(key in payload for key in range_keys):
            patch["psiMin"] = psi_min
            patch["psiMax"] = psi_max
            patch["psiOverboost"] = psi_overboost
            patch["zeroAngle"] = zero_angle
        if "appBle" in payload and isinstance(payload["appBle"], bool):
            patch["appBle"] = payload["appBle"]
        CONFIG.clear()
        CONFIG.update(patch)
        self.send_json(CONFIG)

    def handle_theme_active_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        theme_id = payload.get("id")
        if not isinstance(theme_id, str):
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_theme")
            return
        if not any(theme["id"] == theme_id for theme in THEMES):
            self.send_err(HTTPStatus.NOT_FOUND, "theme_not_found")
            return
        CONFIG["activeThemeId"] = theme_id
        self.send_json(themes_payload())

    def handle_themes_config_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return

        # Flat fields; validation order and error tokens mirror
        # boost_web.c:themes_config_put().
        if "bigDigitStaticBg" in payload and isinstance(payload["bigDigitStaticBg"], bool):
            THEME["bigDigitStaticBg"] = payload["bigDigitStaticBg"]
        if "pixelShift" in payload and isinstance(payload["pixelShift"], bool):
            THEME["pixelShift"] = payload["pixelShift"]
        if "pixelShiftSec" in payload:
            try:
                seconds = float(payload["pixelShiftSec"])
            except (TypeError, ValueError):
                seconds = float("nan")
            if not math.isfinite(seconds) or not 1.0 <= seconds <= 86400.0:
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_pixel_shift_sec")
                return
            THEME["pixelShiftSec"] = max(PXSHIFT_SEC_MIN, min(PXSHIFT_SEC_MAX, int(seconds)))
        if "bigDigitColorText" in payload and isinstance(payload["bigDigitColorText"], bool):
            THEME["bigDigitColorText"] = payload["bigDigitColorText"]
        for key, target in (
            ("bigDigitStaticColor", "bigDigitStaticColor"),
            ("bigDigitTextColor", "bigDigitTextColor"),
        ):
            if key in payload:
                color = parse_hex_color(payload[key])
                if color is None:
                    self.send_err(HTTPStatus.BAD_REQUEST, "invalid_color")
                    return
                THEME[target] = color
        if "arcGradient" in payload and isinstance(payload["arcGradient"], bool):
            THEME["arcGradient"] = payload["arcGradient"]
        if "hudGradient" in payload and isinstance(payload["hudGradient"], bool):
            THEME["hudGradient"] = payload["hudGradient"]
        if "hudTrueBlack" in payload and isinstance(payload["hudTrueBlack"], bool):
            THEME["hudTrueBlack"] = payload["hudTrueBlack"]
        if "neonMarqueeSpin" in payload and isinstance(payload["neonMarqueeSpin"], bool):
            THEME["neonMarqueeSpin"] = payload["neonMarqueeSpin"]
        if "teSync" in payload and isinstance(payload["teSync"], bool):
            THEME["teSync"] = payload["teSync"]
        if "regionDBuf" in payload and isinstance(payload["regionDBuf"], bool):
            THEME["regionDBuf"] = payload["regionDBuf"]
        if "teScanline" in payload and isinstance(payload["teScanline"], bool):
            THEME["teScanline"] = payload["teScanline"]
        if "rotation" in payload:
            deg = payload["rotation"]
            if deg not in (0, 90, 180, 270):
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_rotation")
                return
            THEME["rotation"] = int(deg)
        if "demoMode" in payload and isinstance(payload["demoMode"], bool):
            THEME["demoMode"] = payload["demoMode"]
        if "demoFastSweep" in payload and isinstance(payload["demoFastSweep"], bool):
            THEME["demoFastSweep"] = payload["demoFastSweep"]
        if "tpmsBle" in payload and isinstance(payload["tpmsBle"], bool):
            THEME["tpmsBle"] = payload["tpmsBle"]
        if "vaultFace" in payload:
            color = parse_hex_color(payload["vaultFace"])
            if color is None:
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_color")
                return
            THEME["vaultFace"] = color
        if "vaultVignette" in payload:
            try:
                vignette = float(payload["vaultVignette"])
            except (TypeError, ValueError):
                vignette = float("nan")
            if not math.isfinite(vignette) or not 0.0 <= vignette <= 90.0:
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_vignette")
                return
            THEME["vaultVignette"] = int(vignette)
        if "vaultNeedleRed" in payload and isinstance(payload["vaultNeedleRed"], bool):
            THEME["vaultNeedleRed"] = payload["vaultNeedleRed"]
        if "vaultNeedleTail" in payload and isinstance(payload["vaultNeedleTail"], bool):
            THEME["vaultNeedleTail"] = payload["vaultNeedleTail"]
        if "neonLayout" in payload:
            layout = payload["neonLayout"]
            if layout not in (0, 1, 2):
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_neon_layout")
                return
            THEME["neonLayout"] = int(layout)
        if "neonPreset" in payload:
            preset = payload["neonPreset"]
            if preset not in (0, 1, 2, 3):
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_neon_preset")
                return
            THEME["neonPreset"] = int(preset)
            # boost_theme.c:apply_neon_preset() overwrites the theme zones.
            neon = next(theme for theme in THEMES if theme["id"] == "neon")
            palette = NEON_PRESETS[int(preset)]
            neon["colors"]["vacuum"] = palette["vacuum"]
            neon["colors"]["boost"] = palette["boost"]
            neon["colors"]["overboost"] = palette["overboost"]
        theme_id = payload.get("id")
        if isinstance(theme_id, str):
            theme = next((t for t in THEMES if t["id"] == theme_id), None)
            if theme is None:
                self.send_err(HTTPStatus.NOT_FOUND, "theme_not_found")
                return
            if payload.get("reset") is True:
                if theme["id"] == "neon":
                    palette = NEON_PRESETS[int(THEME["neonPreset"])]
                else:
                    palette = THEME_DEFAULTS[theme["id"]]
                for key in ("vacuum", "boost", "overboost"):
                    theme["colors"][key] = palette[key]
            else:
                colors = payload.get("colors")
                if isinstance(colors, dict):
                    parsed = {}
                    for key in ("vacuum", "boost", "overboost"):
                        if key in colors:
                            color = parse_hex_color(colors[key])
                            if color is None:
                                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_color")
                                return
                            parsed[key] = color
                    theme["colors"].update(parsed)
        self.send_json(themes_payload())

    def handle_supply_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        body, status = set_supply_volts(payload.get("supplyVolts"))
        if body.get("error"):
            self.send_err(status, body["error"])
            return
        self.send_json(body, status)

    def handle_network_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        mode = NETWORK["mode"]
        if "mode" in payload:
            if payload["mode"] not in ("ap", "apsta"):
                self.send_err(HTTPStatus.BAD_REQUEST, "invalid_mode")
                return
            mode = payload["mode"]
        ssid = payload.get("ssid")
        if isinstance(ssid, str) and ssid:
            NETWORK["staSsid"] = ssid
            if not any(item["ssid"] == ssid for item in NETWORK["saved"]):
                NETWORK["saved"].insert(0, {"ssid": ssid})
        NETWORK["mode"] = mode
        NETWORK["staEnabled"] = mode == "apsta"
        NETWORK["staConnected"] = mode == "apsta"
        self.send_json(self.network_status_payload())

    def handle_tpms_config_put(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        ok = True
        if "lowKpa" in payload:
            try:
                low_kpa = float(payload["lowKpa"])
            except (TypeError, ValueError):
                ok = False
            else:
                if not (100.0 <= low_kpa <= 400.0):
                    ok = False
                else:
                    CONFIG["tpmsLowKpa"] = low_kpa
        elif "lowPsi" in payload:
            try:
                low_psi = float(payload["lowPsi"])
            except (TypeError, ValueError):
                ok = False
            else:
                low_kpa = low_psi / KPA_TO_PSI
                if not (100.0 <= low_kpa <= 400.0):
                    ok = False
                else:
                    CONFIG["tpmsLowKpa"] = low_kpa
        if "staleAfterMs" in payload:
            try:
                stale = int(payload["staleAfterMs"])
            except (TypeError, ValueError):
                ok = False
            else:
                if not (2000 <= stale <= 120000):
                    ok = False
                else:
                    CONFIG["tpmsStaleAfterMs"] = stale
        if not ok:
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_tpms_config")
            return
        self.send_json(tpms_config_payload())

    def handle_page_put(self) -> None:
        global ACTIVE_PAGE
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        page = payload.get("page", payload.get("activePage"))
        if page not in (0, 1):
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_page")
            return
        ACTIVE_PAGE = int(page)
        self.send_json({"ok": True, "activePage": ACTIVE_PAGE})

    # -- POST ------------------------------------------------------------
    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/time":
            self.handle_time_post()
        elif parsed.path == "/api/v1/media":
            self.handle_media_post()
        elif parsed.path == "/api/v1/ota":
            self.handle_ota_post()
        elif parsed.path == "/api/v1/restart":
            self.read_body(MAX_JSON_BODY)
            self.send_json({"ok": True, "restartingInMs": 400})
        elif parsed.path == "/api/v1/sensors/calibration":
            self.handle_calibration_post(parsed)
        elif parsed.path == "/api/v1/network/reconnect":
            self.read_body(MAX_JSON_BODY)
            self.send_json(self.network_status_payload())
        else:
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")

    def handle_time_post(self) -> None:
        global TIME_ANCHOR_MS
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        epoch = payload.get("epochMs")
        tz = payload.get("timezoneOffsetMinutes")
        if not isinstance(epoch, (int, float)) or not isinstance(tz, (int, float)):
            self.send_err(HTTPStatus.BAD_REQUEST, "invalid_time")
            return
        epoch_ms = int(epoch)
        if epoch_ms < BOOST_RTC_EPOCH_MIN_MS:
            self.send_err(HTTPStatus.BAD_REQUEST, "time_not_set")
            return
        TIME_ANCHOR_MS = epoch_ms - uptime_ms()
        CONFIG["timezoneOffsetMinutes"] = max(-14 * 60, min(14 * 60, int(tz)))
        if isinstance(payload.get("timezoneTz"), str) and payload["timezoneTz"]:
            CONFIG["timezoneTz"] = payload["timezoneTz"]
        # boost_web.c returns the full /state body after a successful sync.
        self.send_json(state_payload())

    def handle_calibration_post(self, parsed) -> None:
        _, err = self.read_body(MAX_JSON_BODY)
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        forced = parse_qs(parsed.query).get("fail", [None])[0]
        if forced:
            MOCK["calFail"] = forced
        time.sleep(float(MOCK["calDelaySec"]))
        body, status = run_calibration()
        if forced:
            MOCK["calFail"] = None
        if body.get("error"):
            self.send_err(status, body["error"])
            return
        self.send_json(body, status)

    def handle_media_post(self) -> None:
        """Mirror media_post(): size gate, 409 overlap gate, GIF header checks."""
        global MEDIA_UPLOAD_IN_PROGRESS
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            length = 0
        if length == 0 or length > MAX_GIF_BYTES:
            self.send_err(HTTPStatus.BAD_REQUEST, "gif_size")
            return
        with STATE_LOCK:
            if MEDIA_UPLOAD_IN_PROGRESS:
                self.send_err(HTTPStatus.CONFLICT, "media_upload_in_progress")
                return
            MEDIA_UPLOAD_IN_PROGRESS = True
        try:
            body = self.rfile.read(length)
            delay = float(MOCK.get("mediaDelaySec", 0.0))
            if delay > 0:
                time.sleep(delay)
            if len(body) < 10:
                self.send_err(HTTPStatus.BAD_REQUEST, "not_gif")
                return
            header = body[:10]
            signature = header[:6]
            width = header[6] | (header[7] << 8)
            height = header[8] | (header[9] << 8)
            if (signature not in (b"GIF87a", b"GIF89a")
                    or not (0 < width <= MAX_GIF_DIMENSION)
                    or not (0 < height <= MAX_GIF_DIMENSION)):
                self.send_err(HTTPStatus.BAD_REQUEST, "gif_dimensions")
                return
            MEDIA.update(
                {
                    "present": True,
                    "name": "active.gif",
                    "size": len(body),
                    "uploadedAtMs": now_epoch_ms(),
                    "playbackSupported": True,
                    "playback": "active",
                }
            )
            self.send_json(media_status_payload())
        finally:
            with STATE_LOCK:
                MEDIA_UPLOAD_IN_PROGRESS = False

    def handle_ota_post(self) -> None:
        """Mirror ota_post(): empty body -> ota_unavailable, bad image magic ->
        ota_invalid (esp_ota_end validates the image header)."""
        body, err = self.read_body(MAX_OTA_BYTES)
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        if len(body) == 0:
            self.send_err(HTTPStatus.BAD_REQUEST, "ota_unavailable")
            return
        if body[0] != 0xE9:
            self.send_err(HTTPStatus.BAD_REQUEST, "ota_invalid")
            return
        self.send_json(
            {
                "ok": True,
                "pendingReboot": False,
                "bytesWritten": len(body),
                "restartRequired": True,
            }
        )

    # -- DELETE ----------------------------------------------------------
    def do_DELETE(self) -> None:
        parsed = urlparse(self.path)
        if parsed.path == "/api/v1/logs":
            LOGS.clear()
            LOGS_RING_BUILT = True  # do not re-synthesise after a clear
            self.send_json({"ok": True})
        elif parsed.path == "/api/v1/media":
            self.handle_media_delete()
        elif parsed.path == "/api/v1/network":
            self.handle_network_delete()
        else:
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")

    def handle_media_delete(self) -> None:
        with STATE_LOCK:
            if MEDIA_UPLOAD_IN_PROGRESS:
                self.send_err(HTTPStatus.CONFLICT, "media_upload_in_progress")
                return
        MEDIA.update(
            {
                "present": False,
                "name": "active.gif",
                "size": 0,
                "uploadedAtMs": 0,
                "playbackSupported": False,
                "playback": "unavailable",
            }
        )
        self.send_json(media_status_payload())

    def handle_network_delete(self) -> None:
        payload, err = self.read_json()
        if err is not None:
            self.send_err(HTTPStatus.BAD_REQUEST, err)
            return
        ssid = payload.get("ssid")
        if not isinstance(ssid, str) or not ssid:
            self.send_err(HTTPStatus.BAD_REQUEST, "missing_ssid")
            return
        before = len(NETWORK["saved"])
        NETWORK["saved"] = [item for item in NETWORK["saved"] if item["ssid"] != ssid]
        if len(NETWORK["saved"]) == before:
            self.send_err(HTTPStatus.NOT_FOUND, "not_found")
            return
        self.send_json(self.network_status_payload())


class BoostMockServer:
    """Importable, in-process mock server for host-side end-to-end tests."""

    def __init__(
        self,
        host: str = "127.0.0.1",
        port: int = 0,
        seed: int | None = None,
        verbose: bool = False,
    ) -> None:
        self.host = host
        self.port = port
        self.seed = seed
        self.verbose = verbose
        self._httpd: ThreadingHTTPServer | None = None
        self._thread: Thread | None = None

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    def start(self) -> "BoostMockServer":
        reset_mock_state(self.seed)
        Handler.verbose = self.verbose
        self._httpd = ThreadingHTTPServer((self.host, self.port), Handler)
        self.host, self.port = self._httpd.server_address[:2]
        self._thread = Thread(target=self._httpd.serve_forever, daemon=True)
        self._thread.start()
        return self

    def reset(self) -> None:
        reset_mock_state(self.seed)

    def serve_forever(self) -> None:
        if self._httpd is not None:
            self._httpd.serve_forever()

    def stop(self) -> None:
        if self._httpd is not None:
            self._httpd.shutdown()
            self._httpd.server_close()
            self._httpd = None
        if self._thread is not None:
            self._thread.join(timeout=5)
            self._thread = None

    def __enter__(self) -> "BoostMockServer":
        return self.start()

    def __exit__(self, *exc: object) -> None:
        self.stop()


def main() -> None:
    parser = argparse.ArgumentParser(description="Host mock for the Boost Gauge HTTP API")
    parser.add_argument("--host", default="127.0.0.1", help="bind address (default 127.0.0.1)")
    parser.add_argument("--port", default=8080, type=int, help="bind port (default 8080)")
    parser.add_argument("--seed", default=None, type=int,
                        help="deterministic noise seed for the live waveform")
    parser.add_argument("--verbose", action="store_true",
                        help="log each request line to stdout")
    args = parser.parse_args()
    server = BoostMockServer(args.host, args.port, seed=args.seed, verbose=args.verbose).start()
    print(f"Serving Boost Gauge mock at http://{args.host}:{server.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.stop()


if __name__ == "__main__":
    main()
