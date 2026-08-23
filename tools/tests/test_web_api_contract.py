#!/usr/bin/env python3
"""Web API contract test - every /api/v1 endpoint schema and rule the phone
apps parse, exercised against the in-process host mock (tools/mock_server.py ->
BoostMockServer) and cross-checked against the firmware source (boost_web.c).

Covers the deliverable-2(a) list:
  * /state schema incl. tpms / obd / display metrics blocks (types + key sets)
  * /config schema incl. appBle; brightness clamps; range validation bounds
  * /themes order (Dyno Cell -> Vault-Tec -> Night City -> Big Digit -> Neon)
  * /themes/config validation bounds (colors, rotation, vignette, pixelshift,
    neon layout/preset, demoMode vs demoFastSweep kept separate, tpmsBle)
  * /tpms/config validation bounds (lowKpa 100-400, staleAfterMs 2000-120000)
  * /time: valid epoch, invalid_time/time_not_set, and the firmware 409
    clock_rejected rule (source-contract; the mock has no RTC model)
  * /logs JSON sample schema + strictly increasing t_ms
  * /logs.csv exact header + parseable rows with firmware zone tokens
  * /media tiny-GIF upload -> commit, 409 overlap, bad GIF rejection, DELETE,
    repeated DELETE harmless
  * /ota 0xE9 magic boundary (empty -> ota_unavailable, bad magic ->
    ota_invalid, 0xE9 -> accepted on the mock)
  * /restart shape, /network GET/PUT/DELETE incl. saved list semantics,
    /network/scan schema, 404 quirks, static index

Stdlib only. Run:  python3 tools/tests/test_web_api_contract.py
"""

from __future__ import annotations

import csv
import io
import json
import os
import pathlib
import queue
import re
import struct
import sys
import threading
import time
import urllib.error
import urllib.request

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

from mock_server import BoostMockServer  # noqa: E402

# Exact schema the firmware state_json() renders (boost_web.c).
STATE_TOP_KEYS = {
    "psi", "peakPsi", "zone", "demo", "brightness", "firmwareVersion",
    "uptimeMs", "epochMs", "timezoneOffsetMinutes", "activeThemeId",
    "activePage", "display", "sensors", "tpms", "obd",
}
DISPLAY_KEYS = {
    "renderFps", "gaugeDemandPerSecond", "flushesPerSecond", "pixelsPerSecond",
    "worstRenderUs", "renderGapP50Us", "renderGapMaxUs", "framesOverBudget",
    "tePeriodUs", "teWaits", "teTimeouts", "teSkips", "teScanlineWaits",
}
SENSOR_KEYS = {"adsPresent", "bmpPresent", "fault", "mapVolts", "mapAbsKpa", "ambientKpa"}
TPMS_KEYS = {"status", "lowPsi", "wheels"}
WHEEL_KEYS = {"psi", "valid"}
OBD_KEYS = {
    "state", "lastError", "peer", "peerAddr", "uptimeMs", "ageMs", "valid",
    "lastReply", "protocol", "rpm", "speedKph", "coolantC", "mapKpa", "iatC",
    "throttlePct", "mafGps", "fuelPct", "batteryV",
}
CONFIG_KEYS = {
    "brightnessHigh", "brightnessLow", "dimSchedule", "timezoneOffsetMinutes",
    "timezoneTz", "activeThemeId", "psiMin", "psiMax", "psiOverboost",
    "zeroAngle", "appBle",
}
DIM_KEYS = {"enabled", "startMinutes", "endMinutes"}
THEMES_FLAG_KEYS = {
    "bigDigitStaticBg", "bigDigitColorText", "bigDigitStaticColor",
    "bigDigitTextColor", "arcGradient", "hudGradient", "hudTrueBlack",
    "neonMarqueeSpin", "teSync", "regionDBuf", "teScanline", "rotation",
    "vaultFace", "vaultVignette", "vaultNeedleRed", "vaultNeedleTail",
    "neonLayout", "neonPreset", "demoMode", "demoFastSweep", "tpmsBle",
    "pixelShift", "pixelShiftSec",
}
THEME_KEYS = {"id", "name", "style", "colors", "customized"}
EXPECTED_THEME_ORDER = [
    ("dyno-cell", "Dyno Cell"),
    ("vault-tec", "Vault-Tec"),
    ("night-city", "Night City"),
    ("big-digit", "Big Digit"),
    ("neon", "Neon"),
]
NETWORK_KEYS = {"mode", "staEnabled", "staConnected", "staSsid", "staIp",
                "apSsid", "apIp", "rssi", "hasPassword", "saved"}
CSV_HEADER = "timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo"
ZONES = {"VAC", "ATMO", "BOOST", "OVER"}


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

    def schema(self, payload: dict, keys: set, label: str) -> None:
        got = set(payload.keys())
        missing = keys - got
        extra = got - keys
        self.check(not missing and not extra,
                   f"{label}: exact key set",
                   f"missing={sorted(missing)} extra={sorted(extra)}")
        return missing, extra


def call(base: str, method: str, path: str, body=None, headers=None, timeout: float = 20.0):
    data = None
    request_headers = dict(headers or {})
    if isinstance(body, dict):
        data = json.dumps(body).encode()
        request_headers.setdefault("Content-Type", "application/json")
    elif body is not None:
        data = bytes(body)
    req = urllib.request.Request(base + path, data=data, method=method, headers=request_headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return response.status, dict(response.headers), response.read()
    except urllib.error.HTTPError as error:
        return error.code, dict(error.headers), error.read()


def get_json(base: str, path: str):
    status, _h, body = call(base, "GET", path)
    return status, json.loads(body) if body else {}


def put_json(base: str, path: str, body: dict):
    status, _h, raw = call(base, "PUT", path, body)
    try:
        return status, json.loads(raw) if raw else {}
    except ValueError:
        return status, raw


def post_json(base: str, path: str, body: dict):
    status, _h, raw = call(base, "POST", path, body)
    try:
        return status, json.loads(raw) if raw else {}
    except ValueError:
        return status, raw


def tiny_gif_bytes() -> bytes:
    """A minimal 2x2 GIF89a (two-colour GCT, one frame). Valid enough for the
    media-store header checks on both the mock and the firmware."""
    w = h = 2
    lsd = struct.pack("<HHB", w, h, 0x80) + b"\x00\x00"
    gct = b"\xff\xff\xff\x00\x00\x00"           # white, black
    image = b"\x2c" + struct.pack("<HHHH", 0, 0, w, h) + b"\x00"
    lzw = b"\x02\x02\x44\x01\x00"               # min-code 2, clear/colour/end
    return b"GIF89a" + lsd + gct + image + lzw + b"\x3b"


def is_type(payload: dict, key: str, kind) -> bool:
    return isinstance(payload.get(key), kind)


def main() -> int:
    result = Result()
    fw_web = (REPO_ROOT / "main" / "boost_web.c").read_text(encoding="utf-8")
    fw_model = (REPO_ROOT / "main" / "boost_model.c").read_text(encoding="utf-8")
    fw_sensors_h = (REPO_ROOT / "main" / "boost_sensors.h").read_text(encoding="utf-8")

    try:
        server = BoostMockServer(port=0, seed=20260823, verbose=False).start()
    except OSError as error:
        print(f"FAIL could not start BoostMockServer: {error}")
        return 1
    base = server.base_url
    try:
        # ------------------------------------------------------------ /state
        status, state = get_json(base, "/api/v1/state")
        result.check(status == 200, "GET /state 200")
        missing, extra = result.schema(state, STATE_TOP_KEYS, "/state top-level")
        if not missing:
            result.check(is_type(state, "psi", float) and is_type(state, "peakPsi", float),
                         "/state psi/peakPsi are numbers")
            result.check(state["zone"] in ZONES, "/state zone token in VAC/ATMO/BOOST/OVER",
                         f"got {state['zone']!r}")
            result.check(is_type(state, "demo", bool) and is_type(state, "brightness", int),
                         "/state demo bool, brightness int")
            result.check(0 <= state["brightness"] <= 100, "/state brightness within 0..100")
            result.check(is_type(state, "firmwareVersion", str)
                         and is_type(state, "uptimeMs", int)
                         and is_type(state, "epochMs", int)
                         and is_type(state, "timezoneOffsetMinutes", int),
                         "/state firmwareVersion str, uptimeMs/epochMs/tz int")
            result.check(state["activeThemeId"] in {t[0] for t in EXPECTED_THEME_ORDER},
                         "/state activeThemeId is a selectable theme")
            result.check(state["activePage"] in (0, 1), "/state activePage 0|1")

            display = state["display"]
            result.schema(display, DISPLAY_KEYS, "/state display")
            result.check(all(isinstance(display[k], int) for k in DISPLAY_KEYS),
                         "/state display metrics are all integers")
            result.check(display["teTimeouts"] >= 0 and display["renderFps"] >= 0,
                         "/state display counters non-negative")

            sensors = state["sensors"]
            result.schema(sensors, SENSOR_KEYS, "/state sensors")
            result.check(is_type(sensors, "adsPresent", bool)
                         and is_type(sensors, "bmpPresent", bool)
                         and is_type(sensors, "fault", bool)
                         and is_type(sensors, "mapVolts", float)
                         and is_type(sensors, "mapAbsKpa", float)
                         and is_type(sensors, "ambientKpa", float),
                         "/state sensors types")

            tpms = state["tpms"]
            result.schema(tpms, TPMS_KEYS, "/state tpms")
            result.check(is_type(tpms, "status", int) and is_type(tpms, "lowPsi", float),
                         "/state tpms status int, lowPsi float")
            result.check(len(tpms["wheels"]) == 4, "/state tpms.wheels has 4 entries")
            for wheel in tpms["wheels"]:
                result.schema(wheel, WHEEL_KEYS, "/state tpms wheel")
                result.check(is_type(wheel, "psi", float) and is_type(wheel, "valid", bool),
                             "/state tpms wheel types")

            obd = state["obd"]
            result.schema(obd, OBD_KEYS, "/state obd")
            result.check(is_type(obd, "state", int) and is_type(obd, "lastError", int)
                         and is_type(obd, "valid", bool) and is_type(obd, "ageMs", int)
                         and is_type(obd, "uptimeMs", int),
                         "/state obd core types")
            for k in ("rpm", "speedKph", "coolantC", "mapKpa", "iatC",
                      "throttlePct", "mafGps", "fuelPct", "batteryV"):
                result.check(is_type(obd, k, float), f"/state obd.{k} is float")

        # ------------------------------------------------------------ /config
        status, config = get_json(base, "/api/v1/config")
        result.check(status == 200, "GET /config 200")
        result.schema(config, CONFIG_KEYS, "/config")
        result.check(is_type(config, "appBle", bool), "/config.appBle is bool")
        result.check(isinstance(config["dimSchedule"], dict), "/config.dimSchedule is dict")
        result.schema(config["dimSchedule"], DIM_KEYS, "/config.dimSchedule")

        # Brightness clamp round trip (firmware clamp_percent).
        status, patched = put_json(base, "/api/v1/config", {"brightnessHigh": 250})
        result.check(status == 200 and patched["brightnessHigh"] == 100,
                     "brightnessHigh 250 clamps to 100")
        status, patched = put_json(base, "/api/v1/config", {"brightnessLow": -5})
        result.check(status == 200 and patched["brightnessLow"] == 0,
                     "brightnessLow -5 clamps to 0")

        # Gauge range validation bounds (firmware invalid_config gate).
        for body, label in (
            ({"psiMin": 0.0}, "psiMin must be in [-30, -1]"),
            ({"psiMax": 1.0}, "psiMax must be >= 5"),
            ({"psiOverboost": 50.0}, "overboost must be < psiMax"),
            ({"zeroAngle": 90.0}, "zeroAngle must be in [180, 315]"),
            ({"activeThemeId": "sport-cluster"}, "unknown theme id rejected"),
        ):
            status, _ = put_json(base, "/api/v1/config", body)
            result.check(status == 400, f"/config rejects {label}")

        # ----------------------------------------------------------- /themes
        status, themes = get_json(base, "/api/v1/themes")
        result.check(status == 200, "GET /themes 200")
        order = [(t["id"], t["name"]) for t in themes["themes"]]
        result.check(order == EXPECTED_THEME_ORDER,
                     "/themes order Dyno Cell -> Vault-Tec -> Night City -> Big Digit -> Neon",
                     f"got {order}")
        for t in themes["themes"]:
            result.schema(t, THEME_KEYS, f"/themes {t['id']}")
            result.check(is_type(t, "customized", bool), f"/themes {t['id']} customized bool")
            for color in ("face", "track", "text", "muted", "vacuum", "boost", "overboost", "zero"):
                result.check(re.fullmatch(r"#[0-9a-f]{6}", t["colors"].get(color, "") or ""),
                             f"/themes {t['id']} colors.{color} is #rrggbb")
        for key in THEMES_FLAG_KEYS:
            result.check(key in themes, f"/themes carries {key}")

        # --------------------------------------------------- themes/active
        status, resp = put_json(base, "/api/v1/themes/active", {"id": "neon"})
        result.check(status == 200 and resp["activeThemeId"] == "neon",
                     "PUT themes/active {neon} switches theme")
        status, _ = put_json(base, "/api/v1/themes/active", {"id": "nope"})
        result.check(status == 404, "PUT themes/active unknown id -> 404 theme_not_found")
        status, _ = put_json(base, "/api/v1/themes/active", {"id": 3})
        result.check(status == 400, "PUT themes/active non-string id -> 400")

        # ---------------------------------------------------- themes/config
        status, _ = put_json(base, "/api/v1/themes/config",
                             {"bigDigitStaticColor": "not a color"})
        result.check(status == 400, "PUT themes/config invalid color -> 400 invalid_color")
        status, resp = put_json(base, "/api/v1/themes/config", {"rotation": 90})
        result.check(status == 200 and resp["rotation"] == 90,
                     "PUT themes/config rotation 90 accepted")
        status, _ = put_json(base, "/api/v1/themes/config", {"rotation": 45})
        result.check(status == 400, "PUT themes/config rotation 45 -> 400 invalid_rotation")
        status, _ = put_json(base, "/api/v1/themes/config", {"vaultVignette": 120})
        result.check(status == 400, "PUT themes/config vaultVignette 120 -> 400")
        status, _ = put_json(base, "/api/v1/themes/config", {"pixelShiftSec": 0.5})
        result.check(status == 400, "PUT themes/config pixelShiftSec 0.5 -> 400 invalid_pixel_shift_sec")
        status, resp = put_json(base, "/api/v1/themes/config", {"pixelShiftSec": 3600})
        result.check(status == 200 and 30 <= resp["pixelShiftSec"] <= 3600,
                     "PUT themes/config pixelShiftSec clamps into [30, 3600]")
        status, _ = put_json(base, "/api/v1/themes/config", {"neonLayout": 9})
        result.check(status == 400, "PUT themes/config neonLayout 9 -> 400 invalid_neon_layout")
        status, _ = put_json(base, "/api/v1/themes/config", {"neonPreset": 7})
        result.check(status == 400, "PUT themes/config neonPreset 7 -> 400 invalid_neon_preset")

        # demoMode and demoFastSweep are separate persisted flags.
        status, resp = put_json(base, "/api/v1/themes/config",
                                {"demoMode": True, "demoFastSweep": True})
        result.check(status == 200 and resp["demoMode"] is True and resp["demoFastSweep"] is True,
                     "demoMode + demoFastSweep round-trip independently")
        status, resp = put_json(base, "/api/v1/themes/config", {"demoFastSweep": False})
        result.check(status == 200 and resp["demoMode"] is True and resp["demoFastSweep"] is False,
                     "demoFastSweep toggles without touching demoMode")
        status, resp = put_json(base, "/api/v1/themes/config", {"demoMode": False})
        result.check(status == 200 and resp["demoMode"] is False and resp["demoFastSweep"] is False,
                     "demoMode toggles without touching demoFastSweep")
        status, resp = put_json(base, "/api/v1/themes/config", {"tpmsBle": True})
        result.check(status == 200 and resp["tpmsBle"] is True, "tpmsBle round-trips")
        status, resp = put_json(base, "/api/v1/themes/config", {"tpmsBle": False})
        result.check(status == 200 and resp["tpmsBle"] is False, "tpmsBle back off")

        # -------------------------------------------------------- /tpms/config
        status, tpms_cfg = get_json(base, "/api/v1/tpms/config")
        result.check(status == 200, "GET /tpms/config 200")
        result.check({"lowKpa", "lowPsi", "staleAfterMs"} == set(tpms_cfg.keys()),
                     "/tpms/config exact key set")
        result.check(is_type(tpms_cfg, "lowKpa", float) and is_type(tpms_cfg, "lowPsi", float)
                     and is_type(tpms_cfg, "staleAfterMs", int),
                     "/tpms/config types")
        for body in ({"lowKpa": 50.0}, {"lowKpa": 500.0}, {"staleAfterMs": 100},
                     {"staleAfterMs": 500000}, {"lowPsi": 1000000.0}):
            status, _ = put_json(base, "/api/v1/tpms/config", body)
            result.check(status == 400, f"/tpms/config rejects {body}")
        status, resp = put_json(base, "/api/v1/tpms/config",
                                {"lowKpa": 240.0, "staleAfterMs": 15000})
        result.check(status == 200 and resp["lowKpa"] == 240.0
                     and resp["staleAfterMs"] == 15000,
                     "/tpms/config accepts in-bounds values")

        # -------------------------------------------------------------- /time
        status, _ = post_json(base, "/api/v1/time",
                              {"epochMs": 1000, "timezoneOffsetMinutes": 0})
        result.check(status == 400, "POST /time below RTC floor -> 400 time_not_set")
        status, _ = post_json(base, "/api/v1/time", {"epochMs": "x"})
        result.check(status == 400, "POST /time non-number epoch -> 400 invalid_time")
        now_ms = int(time.time() * 1000)
        status, resp = post_json(base, "/api/v1/time",
                                 {"epochMs": now_ms, "timezoneOffsetMinutes": -240,
                                  "timezoneTz": "EST5EDT,M3.2.0/2,M11.1.0/2"})
        result.check(status == 200 and "psi" in resp,
                     "POST /time valid epoch returns full /state")

        # Firmware source contract: 409 clock_rejected on >5 min RTC mismatch.
        result.check("clock_rejected" in fw_web,
                     "boost_web.c maps RTC disagreement to clock_rejected")
        result.check("ESP_ERR_INVALID_STATE" in fw_web and '"409 Conflict"' in fw_web,
                     "boost_web.c sends 409 Conflict for ESP_ERR_INVALID_STATE")
        result.check("#define BOOST_RTC_SYNC_TOLERANCE_MS (5LL * 60 * 1000)" in fw_sensors_h,
                     "BOOST_RTC_SYNC_TOLERANCE_MS == 5 minutes")
        result.check("diff > BOOST_RTC_SYNC_TOLERANCE_MS || diff < -BOOST_RTC_SYNC_TOLERANCE_MS" in fw_model
                     and "return ESP_ERR_INVALID_STATE;" in fw_model,
                     "boost_model_set_time rejects >5-min client epoch before settimeofday")

        # -------------------------------------------------------------- /logs
        status, logs = get_json(base, "/api/v1/logs?limit=5")
        result.check(status == 200, "GET /logs?limit=5 200")
        result.check("samples" in logs and len(logs["samples"]) == 5,
                     "/logs returns exactly 5 samples")
        sample = logs["samples"][0]
        result.check({"tMs", "psi", "peakPsi", "zone", "demo"} == set(sample.keys()),
                     "/logs sample exact key set")
        t_ms = [s["tMs"] for s in logs["samples"]]
        result.check(all(a < b for a, b in zip(t_ms, t_ms[1:])) and t_ms == sorted(t_ms),
                     "/logs t_ms strictly increasing")
        result.check(all(s["zone"] in ZONES for s in logs["samples"]),
                     "/logs zone tokens in VAC/ATMO/BOOST/OVER")

        # ------------------------------------------------------------ logs.csv
        status, headers, raw_csv = call(base, "GET", "/api/v1/logs.csv")
        result.check(status == 200 and "text/csv" in headers.get("Content-Type", ""),
                     "GET /logs.csv 200 text/csv")
        text = raw_csv.decode("utf-8")
        result.check(text.splitlines()[0] == CSV_HEADER,
                     "/logs.csv header exact",
                     f"got {text.splitlines()[0]!r}")
        rows = list(csv.reader(io.StringIO(text)))
        result.check(len(rows) > 2, "/logs.csv has data rows")
        for row in rows[1:5]:
            result.check(len(row) == 8, "/logs.csv row has 8 columns", f"got {row}")
            try:
                float(row[3]) and float(row[4]) and float(row[5])
                ok = True
            except ValueError:
                ok = False
            result.check(ok and row[6] in ZONES and row[7] in ("0", "1"),
                         "/logs.csv row numbers + zone token + demo flag", f"got {row[:8]}")

        # -------------------------------------------------------------- /media
        status, media = get_json(base, "/api/v1/media/status")
        result.check(status == 200, "GET /media/status 200")
        result.check({"present", "name", "size", "uploadedAtMs",
                      "playbackSupported", "playback"} == set(media.keys()),
                     "/media/status exact key set")
        gif = tiny_gif_bytes()
        status, _hdr, raw = call(base, "POST", "/api/v1/media", gif)
        resp = json.loads(raw) if raw else {}
        result.check(status == 200 and resp.get("present") is True,
                     "POST media tiny GIF commits present:true")
        status, media = get_json(base, "/api/v1/media/status")
        result.check(media["present"] is True and media["size"] == len(gif),
                     "/media/status reflects the committed size")

        # 409 overlap guard: hold the upload slot open via the mock fault and
        # fire a second POST while the first is still in flight.
        status, _ = put_json(base, "/api/v1/mock/sensors", {"mediaDelaySec": 0.8})
        result.check(status == 200, "mock mediaDelaySec armed")
        outcomes = queue.Queue()
        def slow_upload():
            _st, _hdr, body = call(base, "POST", "/api/v1/media", gif)
            outcomes.put(body)
        t = threading.Thread(target=slow_upload, daemon=True)
        t.start()
        time.sleep(0.2)
        status, _hdr, _raw = call(base, "POST", "/api/v1/media", gif)
        result.check(status == 409, "overlapping POST media -> 409 media_upload_in_progress")
        t.join(timeout=5)
        status, _ = put_json(base, "/api/v1/mock/sensors", {"mediaDelaySec": 0.0})
        result.check(status == 200, "mock mediaDelaySec cleared")

        status, _hdr, _raw = call(base, "POST", "/api/v1/media", b"hello")
        result.check(status == 400, "POST media non-GIF -> 400")
        big_header = b"GIF89a" + struct.pack("<HH", 500, 2)
        status, _hdr, _raw = call(base, "POST", "/api/v1/media", big_header + b"\x00" * 40)
        result.check(status == 400, "POST media oversized dimension -> 400 gif_dimensions")

        status, _hdr, raw = call(base, "DELETE", "/api/v1/media")
        resp = json.loads(raw) if raw else {}
        result.check(status == 200 and resp.get("present") is False,
                     "DELETE media -> present:false")
        status, _hdr, raw = call(base, "DELETE", "/api/v1/media")
        resp = json.loads(raw) if raw else {}
        result.check(status == 200 and resp.get("present") is False,
                     "repeated DELETE media is harmless")

        # ---------------------------------------------------------------- /ota
        status, _hdr, _raw = call(base, "POST", "/api/v1/ota", b"")
        result.check(status == 400, "POST ota empty -> 400 ota_unavailable")
        status, _hdr, _raw = call(base, "POST", "/api/v1/ota", b"GARBAGE" * 8)
        result.check(status == 400, "POST ota bad magic -> 400 ota_invalid")
        status, _hdr, raw = call(base, "POST", "/api/v1/ota", b"\xe9\x00\x00" + b"\x00" * 32)
        resp = json.loads(raw) if raw else {}
        result.check(status == 200 and resp.get("ok") is True,
                     "POST ota 0xE9 magic accepted by the mock (firmware validates via esp_ota_end)")

        # ------------------------------------------------------------ /restart
        status, resp = post_json(base, "/api/v1/restart", {})
        result.check(status == 200 and resp.get("restartingInMs") == 400
                     and resp.get("ok") is True,
                     "POST /restart -> {ok, restartingInMs:400}")

        # ------------------------------------------------------------ /network
        status, net = get_json(base, "/api/v1/network")
        result.check(status == 200, "GET /network 200")
        result.schema(net, NETWORK_KEYS, "/network")
        result.check(net["mode"] in ("ap", "apsta") and is_type(net, "staEnabled", bool)
                     and is_type(net, "staConnected", bool) and is_type(net, "rssi", int)
                     and is_type(net, "hasPassword", bool),
                     "/network types")
        result.check(all(isinstance(s, dict) and set(s.keys()) == {"ssid"}
                         for s in net["saved"]),
                     "/network saved items are {ssid} objects")

        status, resp = put_json(base, "/api/v1/network", {"ssid": "NewNet-5G", "mode": "apsta"})
        result.check(status == 200 and resp["saved"][0]["ssid"] == "NewNet-5G",
                     "PUT network adds new ssid to the front of saved")
        status, _hdr, raw = call(base, "DELETE", "/api/v1/network", {"ssid": "NewNet-5G"})
        resp = json.loads(raw) if raw else {}
        result.check(status == 200 and all(s["ssid"] != "NewNet-5G" for s in resp["saved"]),
                     "DELETE network removes the saved ssid")
        status, _hdr, _raw = call(base, "DELETE", "/api/v1/network", {"ssid": "MissingNet"})
        result.check(status == 404, "DELETE network unknown ssid -> 404 not_found")
        status, _hdr, _raw = call(base, "DELETE", "/api/v1/network", {})
        result.check(status == 400, "DELETE network missing ssid -> 400 missing_ssid")
        status, _ = put_json(base, "/api/v1/network", {"mode": "bogus"})
        result.check(status == 400, "PUT network invalid mode -> 400 invalid_mode")
        status, scan = get_json(base, "/api/v1/network/scan")
        result.check(status == 200 and all({"ssid", "rssi", "auth"}.issubset(n.keys())
                                           for n in scan["networks"]),
                     "GET /network/scan returns ssid/rssi/auth")
        status, _ = put_json(base, "/api/v1/mock/sensors", {"scanFail": "busy"})
        status, _ = get_json(base, "/api/v1/network/scan?fail=1")
        result.check(status == 400, "failed scan mirrors 400 scan_failed")
        status, _ = put_json(base, "/api/v1/mock/sensors", {"scanFail": None})
        result.check(status == 200, "mock scan fault cleared")

        # ----------------------------------------------------------- quirks
        status, _ = get_json(base, "/api/v1/bogus")
        result.check(status == 404, "unknown /api/v1 path -> 404 (wildcard asset handler)")
        status, _ = get_json(base, "/api/v1/themes/config")
        result.check(status == 404, "GET /themes/config is not registered (404 quirk)")

        # ------------------------------------------------------------ static
        status, headers, _ = call(base, "GET", "/")
        result.check(status == 200 and "text/html" in headers.get("Content-Type", ""),
                     "GET / serves the dashboard index")
    finally:
        server.stop()

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
