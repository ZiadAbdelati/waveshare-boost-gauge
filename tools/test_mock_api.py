#!/usr/bin/env python3
"""End-to-end assertions for the host Boost Gauge mock (tools/mock_server.py).

Stdlib only. Starts BoostMockServer on an ephemeral port in a background
thread and exercises every HTTP endpoint the firmware registers, asserting the
response shapes phone apps depend on (boost_web.c is the normative source).

Run with:

    python3 tools/test_mock_api.py

Prints PASS/FAIL per check and exits non-zero when anything fails.
"""

from __future__ import annotations

import csv
import io
import json
import os
import sys
import threading
import time
import urllib.error
import urllib.request


sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mock_server import BoostMockServer  # noqa: E402


class Result:
    """Collects check outcomes; prints PASS/FAIL rows as they run."""

    def __init__(self) -> None:
        self.checks = 0
        self.failures: list[str] = []

    def check(self, ok: bool, label: str, detail: str = "") -> None:
        self.checks += 1
        suffix = f"  [{detail}]" if detail and not ok else ""
        if ok:
            print(f"PASS {label}")
        else:
            self.failures.append(label)
            print(f"FAIL {label}{suffix}")

    def summary(self) -> tuple[int, int]:
        return self.checks, len(self.failures)


def call(base: str, method: str, path: str, body=None, headers=None, timeout: float = 20.0):
    """HTTP helper. `body` may be a dict (JSON), bytes, bytearray, or None."""
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
    status, headers, body = call(base, "GET", path)
    return status, headers, json.loads(body)


def expect_error(result: Result, status: int, error_token: str, base: str, method: str, path: str,
                 body=None, label: str | None = None) -> None:
    got_status, _headers, got_body = call(base, method, path, body=body)
    try:
        payload = json.loads(got_body)
        got_token = payload.get("error")
    except (ValueError, AttributeError):
        got_token = None
    result.check(
        got_status == status and got_token == error_token,
        label or path,
        detail=f"status={got_status} body={got_body[:120]!r}",
    )


def run_tests(base: str, result: Result) -> None:
    # ------------------------------------------------------------------ CORS
    status, headers, body = call(base, "OPTIONS", "/api/v1/state")
    result.check(
        status == 200
        and body == b""
        and headers.get("Access-Control-Allow-Origin") == "*"
        and headers.get("Access-Control-Allow-Headers") == "Content-Type"
        and headers.get("Access-Control-Allow-Methods") == "GET,PUT,POST,DELETE,OPTIONS",
        "OPTIONS mirrors options_handler + set_common_headers",
    )

    status, headers, body = call(base, "GET", "/api/v1/state")
    result.check(
        status == 200 and headers.get("Access-Control-Allow-Origin") == "*",
        "JSON responses carry Access-Control-Allow-Origin",
    )

    # ---------------------------------------------------------------- state
    status, headers, state = get_json(base, "/api/v1/state")
    state_keys = {
        "psi", "peakPsi", "zone", "demo", "brightness", "firmwareVersion",
        "uptimeMs", "epochMs", "timezoneOffsetMinutes", "activeThemeId",
        "activePage", "display", "sensors", "tpms", "obd",
    }
    result.check(
        status == 200 and state_keys.issubset(state.keys()),
        "GET /state exposes every boost_web.c state key",
        detail=f"missing={state_keys - set(state.keys())}",
    )
    display_keys = {
        "renderFps", "gaugeDemandPerSecond", "flushesPerSecond", "pixelsPerSecond",
        "worstRenderUs", "renderGapP50Us", "renderGapMaxUs", "framesOverBudget",
        "tePeriodUs", "teWaits", "teTimeouts", "teSkips", "teScanlineWaits",
    }
    result.check(display_keys.issubset(state["display"].keys()),
                 "GET /state display metrics block complete")
    result.check(
        isinstance(state["psi"], (int, float))
        and isinstance(state["peakPsi"], (int, float))
        and isinstance(state["demo"], bool)
        and isinstance(state["uptimeMs"], int)
        and state["zone"] in {"VAC", "ATMO", "BOOST", "OVER"},
        "GET /state scalar types (psi/peakPsi/zone/demo/uptimeMs)",
    )
    result.check(state["firmwareVersion"] == "v0.8.0",
                 "firmwareVersion preserves the verified release identity")
    result.check(
        {"state", "lastError", "peer", "peerAddr", "uptimeMs", "ageMs", "valid",
         "lastReply", "protocol", "rpm", "speedKph", "coolantC", "mapKpa", "iatC",
         "throttlePct", "mafGps", "fuelPct", "batteryV"}.issubset(state["obd"].keys()),
        "GET /state obd block complete (incl. lastReply/protocol)",
    )
    result.check(
        {"status", "lowPsi", "wheels"}.issubset(state["tpms"].keys())
        and len(state["tpms"]["wheels"]) == 4,
        "GET /state tpms block complete with 4 wheels",
    )

    time.sleep(0.05)
    _s, _h, state2 = get_json(base, "/api/v1/state")
    result.check(
        state2["uptimeMs"] > state["uptimeMs"] and state2["epochMs"] > state["epochMs"],
        "uptimeMs/epochMs advance between /state samples",
    )
    time.sleep(1.15)
    _s, _h, state3 = get_json(base, "/api/v1/state")
    result.check(
        state3["psi"] != state["psi"],
        "psi moves on the live demo waveform",
        detail=f"psi1={state['psi']} psi2={state3['psi']}",
    )

    # ---------------------------------------------------------------- config
    status, _h, config = get_json(base, "/api/v1/config")
    result.check(
        status == 200
        and config.get("appBle") is False
        and {"brightnessHigh", "brightnessLow", "dimSchedule", "timezoneOffsetMinutes",
             "timezoneTz", "activeThemeId", "psiMin", "psiMax", "psiOverboost",
             "zeroAngle"}.issubset(config.keys()),
        "GET /config exposes boost config incl. appBle:false default",
    )
    status, _h, body = call(base, "PUT", "/api/v1/config",
                            {"appBle": True, "brightnessHigh": 140, "brightnessLow": 10,
                             "timezoneOffsetMinutes": 300})
    cfg = json.loads(body)
    result.check(
        status == 200 and cfg.get("appBle") is True and cfg["brightnessHigh"] == 100,
        "PUT /config round-trips appBle and clamps brightness to 100",
    )
    _s, _h, config_after = get_json(base, "/api/v1/config")
    result.check(config_after.get("appBle") is True,
                 "appBle persists in-memory across GET /config")
    expect_error(result, 400, "invalid_config", base, "PUT", "/api/v1/config",
                 {"psiMax": 1.0, "psiMin": -15.0}, "PUT /config rejects invalid gauge range")
    expect_error(result, 400, "invalid_json", base, "PUT", "/api/v1/config", b"not json",
                 "PUT /config rejects malformed JSON body")
    expect_error(result, 400, "invalid_config", base, "PUT", "/api/v1/config",
                 {"activeThemeId": "sport"}, "PUT /config rejects a removed theme id")

    # ------------------------------------------------------------------ time
    epoch_ms = int(time.time() * 1000)
    status, _h, body = call(base, "POST", "/api/v1/time",
                            {"epochMs": epoch_ms, "timezoneOffsetMinutes": -300,
                             "timezoneTz": "EST5EDT,M3.2.0/2,M11.1.0/2"})
    state_time = json.loads(body)
    result.check(
        status == 200 and "psi" in state_time and state_time["timezoneOffsetMinutes"] == -300
        and abs(state_time["epochMs"] - epoch_ms) < 2000,
        "POST /time accepts epoch/tz and returns the full /state body",
    )
    expect_error(result, 400, "invalid_time", base, "POST", "/api/v1/time",
                 {"epochMs": epoch_ms}, "POST /time rejects missing timezoneOffsetMinutes")
    expect_error(result, 400, "time_not_set", base, "POST", "/api/v1/time",
                 {"epochMs": 123456789, "timezoneOffsetMinutes": 0},
                 "POST /time rejects an implausible epoch")

    # ---------------------------------------------------------------- themes
    status, _h, themes = get_json(base, "/api/v1/themes")
    theme_order = [(t["id"], t["name"]) for t in themes["themes"]]
    expected_order = [("dyno-cell", "Dyno Cell"), ("vault-tec", "Vault-Tec"),
                      ("night-city", "Night City"), ("big-digit", "Big Digit"),
                      ("neon", "Neon")]
    result.check(
        status == 200 and theme_order == expected_order,
        "GET /themes order matches boost_theme.c s_defaults (5 themes)",
        detail=f"{theme_order}",
    )
    result.check(
        all({"id", "name", "style", "colors", "customized"}.issubset(t.keys())
            and set(t["colors"]) == {"face", "track", "text", "muted", "vacuum",
                                     "boost", "overboost", "zero"}
            and isinstance(t["customized"], bool)
            for t in themes["themes"]),
        "theme objects expose id/name/style/colors/customized",
    )
    result.check(
        {"bigDigitStaticBg", "bigDigitColorText", "bigDigitStaticColor", "bigDigitTextColor",
         "arcGradient", "hudGradient", "hudTrueBlack", "neonMarqueeSpin", "teSync",
         "regionDBuf", "teScanline", "rotation", "vaultFace", "vaultVignette",
         "vaultNeedleRed", "vaultNeedleTail", "neonLayout", "neonPreset", "demoMode",
         "demoFastSweep", "tpmsBle", "pixelShift", "pixelShiftSec"}.issubset(themes.keys()) and
        themes["activeThemeId"] == "dyno-cell",
        "GET /themes exposes the full firmware theme-settings block",
    )

    status, _h, body = call(base, "PUT", "/api/v1/themes/active", {"id": "neon"})
    result.check(status == 200 and json.loads(body)["activeThemeId"] == "neon",
                 "PUT /themes/active switches the active theme")
    expect_error(result, 404, "theme_not_found", base, "PUT", "/api/v1/themes/active",
                 {"id": "bogus"}, "PUT /themes/active rejects unknown theme")
    expect_error(result, 400, "invalid_theme", base, "PUT", "/api/v1/themes/active", {},
                 "PUT /themes/active rejects a body without id")

    status, _h, body = call(base, "PUT", "/api/v1/themes/config", {
        "demoMode": True, "demoFastSweep": True, "regionDBuf": True, "teScanline": True,
        "teSync": True, "neonPreset": 3, "rotation": 180, "pixelShiftSec": 120,
        "vaultNeedleTail": True, "vaultVignette": 30,
    })
    theme_cfg = json.loads(body)
    result.check(
        status == 200
        and theme_cfg["demoMode"] is True and theme_cfg["demoFastSweep"] is True
        and theme_cfg["regionDBuf"] is True and theme_cfg["teScanline"] is True
        and theme_cfg["teSync"] is True and theme_cfg["neonPreset"] == 3
        and theme_cfg["rotation"] == 180 and theme_cfg["pixelShiftSec"] == 120
        and theme_cfg["vaultNeedleTail"] is True and theme_cfg["vaultVignette"] == 30,
        "PUT /themes/config round-trips the full flat settings block",
    )
    blood_moon = {
        "track": "#0c1440", "muted": "#35509e", "vacuum": "#0064ff",
        "boost": "#c4172e", "overboost": "#ff6a00",
    }
    neon_colors = theme_cfg["themes"][4]["colors"]
    result.check(
        all(neon_colors[k] == v for k, v in blood_moon.items()),
        "neon preset 3 applies the Blood Moon palette",
        detail=f"{neon_colors}",
    )
    expect_error(result, 400, "invalid_rotation", base, "PUT", "/api/v1/themes/config",
                 {"rotation": 45}, "PUT /themes/config rejects non-quarter-turn rotation")
    expect_error(result, 400, "invalid_pixel_shift_sec", base, "PUT", "/api/v1/themes/config",
                 {"pixelShiftSec": 99999}, "PUT /themes/config rejects pixelShiftSec outside 1..86400")
    expect_error(result, 400, "invalid_vignette", base, "PUT", "/api/v1/themes/config",
                 {"vaultVignette": 200}, "PUT /themes/config rejects out-of-range vignette")
    expect_error(result, 400, "invalid_neon_layout", base, "PUT", "/api/v1/themes/config",
                 {"neonLayout": 5}, "PUT /themes/config rejects invalid neon layout")
    expect_error(result, 400, "invalid_neon_preset", base, "PUT", "/api/v1/themes/config",
                 {"neonPreset": 9}, "PUT /themes/config rejects invalid neon preset")
    expect_error(result, 400, "invalid_color", base, "PUT", "/api/v1/themes/config",
                 {"bigDigitStaticColor": "#zzzzzz"}, "PUT /themes/config rejects invalid color")
    expect_error(result, 404, "theme_not_found", base, "PUT", "/api/v1/themes/config",
                 {"id": "sport"}, "PUT /themes/config rejects unknown theme id")

    status, _h, body = call(base, "PUT", "/api/v1/themes/config",
                            {"id": "dyno-cell", "colors": {"boost": "#aa00ff"}})
    colors = json.loads(body)
    result.check(
        status == 200 and colors["themes"][0]["colors"]["boost"] == "#aa00ff"
        and colors["themes"][0]["customized"] is True,
        "PUT /themes/config customizes theme colors and sets customized",
    )
    status, _h, body = call(base, "PUT", "/api/v1/themes/config",
                            {"id": "dyno-cell", "reset": True})
    colors = json.loads(body)
    result.check(
        status == 200 and colors["themes"][0]["colors"]["boost"] == "#b8f35a"
        and colors["themes"][0]["customized"] is False,
        "PUT /themes/config reset restores compiled-in colors",
    )

    # ------------------------------------------------------------ tpms config
    status, _h, tpms = get_json(base, "/api/v1/tpms/config")
    result.check(
        status == 200 and {"lowKpa", "lowPsi", "staleAfterMs"}.issubset(tpms.keys()),
        "GET /tpms/config exposes lowKpa/lowPsi/staleAfterMs",
    )
    status, _h, body = call(base, "PUT", "/api/v1/tpms/config",
                            {"lowKpa": 250.0, "staleAfterMs": 20000})
    tpms = json.loads(body)
    result.check(
        status == 200 and tpms["lowKpa"] == 250.0 and tpms["staleAfterMs"] == 20000
        and round(tpms["lowPsi"], 1) == 36.3,
        "PUT /tpms/config round-trips threshold + staleness",
    )
    expect_error(result, 400, "invalid_tpms_config", base, "PUT", "/api/v1/tpms/config",
                 {"lowKpa": 50.0}, "PUT /tpms/config rejects out-of-range lowKpa")

    # ------------------------------------------------------------ calibration
    status, _h, cal = get_json(base, "/api/v1/sensors/calibration")
    result.check(
        status == 200
        and {"supplyVolts", "live", "calibration"}.issubset(cal.keys())
        and {"adsPresent", "bmpPresent", "fault", "mapVolts", "mapAgeMs", "nominalKpa",
             "correctedKpa", "bmpKpa", "bmpAgeMs", "bmpUpdates",
             "ambientIsFallback"}.issubset(cal["live"].keys())
        and {"valid", "version", "offsetKpa", "offsetPsi", "supplyVolts", "refMapVolts",
             "refNominalKpa", "refBmpKpa", "samples", "epochMs"}.issubset(cal["calibration"].keys()),
        "GET /sensors/calibration exposes boost_web.c shape",
    )
    status, _h, body = call(base, "PUT", "/api/v1/sensors/supply", {"supplyVolts": 5.10})
    result.check(status == 200 and json.loads(body)["supplyVolts"] == 5.1,
                 "PUT /sensors/supply accepts a valid supply voltage")
    expect_error(result, 400, "invalid_supply", base, "PUT", "/api/v1/sensors/supply",
                 {"supplyVolts": 2.0}, "PUT /sensors/supply rejects out-of-range voltage")

    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors",
                            {"calDelaySec": 0.05, "calFail": None})
    result.check(status == 200 and json.loads(body)["calDelaySec"] == 0.05,
                 "mock fault-injection endpoint accepts calDelaySec")
    status, _h, body = call(base, "POST", "/api/v1/sensors/calibration", {})
    cal = json.loads(body)
    result.check(
        status == 200 and cal["calibration"]["valid"] is True
        and cal["calibration"]["version"] == 1,
        "POST /sensors/calibration produces a valid calibration",
    )
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"calFail": "no_ads"})
    result.check(status == 200, "mock fault injection sets calFail")
    expect_error(result, 409, "no_ads", base, "POST", "/api/v1/sensors/calibration", {},
                 "POST /sensors/calibration mirrors 409 calibration error codes")
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"calFail": None})
    result.check(status == 200, "mock fault injection clears calFail")

    status, _h, scan = get_json(base, "/api/v1/sensors/scan")
    result.check(
        status == 200 and {"busUp", "recoveries", "found"}.issubset(scan.keys())
        and scan["busUp"] is True and "0x48" in scan["found"] and "0x76" in scan["found"],
        "GET /sensors/scan returns busUp/recoveries/found",
    )

    # ------------------------------------------------------------------ page
    status, _h, body = call(base, "PUT", "/api/v1/page", {"page": 1})
    result.check(status == 200 and json.loads(body) == {"ok": True, "activePage": 1},
                 "PUT /page accepts page 1")
    _s, _h, state_after_page = get_json(base, "/api/v1/state")
    result.check(state_after_page["activePage"] == 1,
                 "GET /state reflects the active page")
    status, _h, body = call(base, "PUT", "/api/v1/page", {"activePage": 0})
    result.check(status == 200 and json.loads(body)["activePage"] == 0,
                 "PUT /page accepts activePage alias")
    expect_error(result, 400, "invalid_page", base, "PUT", "/api/v1/page", {"page": 2},
                 "PUT /page rejects page 2")

    # ------------------------------------------------------------------ logs
    status, _h, body = call(base, "GET", "/api/v1/logs?limit=25")
    logs = json.loads(body)
    result.check(
        status == 200 and len(logs["samples"]) == 25,
        "GET /logs?limit=25 honors limit",
        detail=f"got {len(logs['samples'])} samples",
    )
    result.check(
        all({"tMs", "psi", "peakPsi", "zone", "demo"}.issubset(s.keys())
            and isinstance(s["tMs"], int) and isinstance(s["psi"], (int, float))
            and isinstance(s["peakPsi"], (int, float)) and isinstance(s["demo"], bool)
            for s in logs["samples"]),
        "GET /logs rows use tMs/psi/peakPsi/zone/demo",
    )
    tms_list = [s["tMs"] for s in logs["samples"]]
    result.check(tms_list == sorted(tms_list) and len(set(tms_list)) == len(tms_list),
                 "GET /logs ring rows are strictly increasing in tMs")
    _s, _h, body = call(base, "GET", "/api/v1/logs?limit=0")
    result.check(len(json.loads(body)["samples"]) == 300,
                 "GET /logs?limit=0 falls back to the default 300")
    _s, _h, body = call(base, "GET", "/api/v1/logs?limit=999999")
    result.check(len(json.loads(body)["samples"]) <= 18000,
                 "GET /logs?limit caps at BOOST_LOG_CAPACITY (18000)")

    status, headers, body = call(base, "GET", "/api/v1/logs.csv")
    result.check(
        status == 200
        and headers.get("Content-Type") == "text/csv"
        and headers.get("Content-Disposition") == 'attachment; filename="boost-gauge-log.csv"',
        "GET /logs.csv mirrors Content-Type + Content-Disposition",
    )
    csv_rows = list(csv.reader(io.StringIO(body.decode("utf-8"))))
    result.check(
        csv_rows[0] == ["timestamp_local", "utc_offset_minutes", "epoch_ms", "uptime_ms",
                        "psi", "peakPsi", "zone", "demo"],
        "GET /logs.csv header matches boost_web.c columns",
    )
    data_rows = csv_rows[1:]
    result.check(
        len(data_rows) >= 25
        and all(len(row) == 8 for row in data_rows)
        and all(float(row[4]) == float(row[4]) and row[7] in ("0", "1") for row in data_rows),
        "GET /logs.csv parses with numeric psi and demo 0/1",
    )

    status, _h, body = call(base, "DELETE", "/api/v1/logs")
    result.check(status == 200 and json.loads(body) == {"ok": True},
                 "DELETE /logs returns 200 {'ok': true}")
    _s, _h, body = call(base, "GET", "/api/v1/logs?limit=10")
    result.check(json.loads(body)["samples"] == [],
                 "DELETE /logs empties the ring")

    # ---------------------------------------------------------------- media
    status, _h, media = get_json(base, "/api/v1/media/status")
    result.check(
        status == 200 and set(media) == {"present", "name", "size", "uploadedAtMs",
                                         "playbackSupported", "playback"}
        and media["present"] is False and media["playback"] == "unavailable",
        "GET /media/status exposes boost_web.c shape when empty",
    )
    gif = b"GIF89a\x01\x00\x01\x00\x00\x00\x00;"
    status, _h, body = call(base, "POST", "/api/v1/media", gif,
                            {"Content-Type": "image/gif"})
    media = json.loads(body)
    result.check(
        status == 200 and media["present"] is True and media["size"] == len(gif)
        and media["name"] == "active.gif" and media["playbackSupported"] is True
        and media["playback"] == "active",
        "POST /media accepts a bounded GIF and reports active playback",
    )
    _s, _h, media_after = get_json(base, "/api/v1/media/status")
    result.check(media_after["present"] is True and media_after["size"] == len(gif),
                 "GET /media/status reflects the committed upload")
    expect_error(result, 400, "gif_dimensions", base, "POST", "/api/v1/media",
                 b"not a gif at all!!", "POST /media rejects a non-GIF >= 10 bytes")
    expect_error(result, 400, "not_gif", base, "POST", "/api/v1/media", b"GIF",
                 "POST /media rejects a truncated GIF header")
    expect_error(result, 400, "gif_size", base, "POST", "/api/v1/media", b"",
                 "POST /media rejects an empty body with gif_size")

    # Overlapping upload: hold the slot open via the mock delay, then verify
    # both a second POST and a DELETE are rejected 409 without disturbing the
    # in-flight upload.
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"mediaDelaySec": 0.8})
    result.check(status == 200, "mock upload delay armed")
    upload_result: list = []
    def upload_in_background() -> None:
        upload_result.append(call(base, "POST", "/api/v1/media", gif,
                                  {"Content-Type": "image/gif"}))
    thread = threading.Thread(target=upload_in_background)
    thread.start()
    time.sleep(0.2)
    expect_error(result, 409, "media_upload_in_progress", base, "POST", "/api/v1/media", gif,
                 "POST /media is 409 while an upload is in flight")
    expect_error(result, 409, "media_upload_in_progress", base, "DELETE", "/api/v1/media", {},
                 "DELETE /media is 409 while an upload is in flight")
    thread.join(timeout=10)
    result.check(not thread.is_alive(), "in-flight media upload completes")
    result.check(len(upload_result) == 1 and upload_result[0][0] == 200,
                 "first upload still commits despite the 409 rejections")
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"mediaDelaySec": 0.05})
    result.check(status == 200, "mock upload delay restored")

    status, _h, body = call(base, "DELETE", "/api/v1/media")
    media = json.loads(body)
    result.check(status == 200 and media["present"] is False,
                 "DELETE /media returns the empty media status")
    status, _h, body = call(base, "DELETE", "/api/v1/media")
    result.check(status == 200, "repeated DELETE /media stays harmless")

    # ------------------------------------------------------------------- ota
    # Larger than MAX_JSON_BODY (4096): the firmware streams the whole image,
    # so the mock must not cap OTA at the JSON body limit.
    image = b"\xE9" + bytes(range(1, 256)) * 32
    result.check(len(image) > 4096, "OTA fixture exceeds the JSON body cap")
    status, _h, body = call(base, "POST", "/api/v1/ota", image)
    ota = json.loads(body)
    result.check(
        status == 200 and ota == {"ok": True, "pendingReboot": False,
                                  "bytesWritten": len(image), "restartRequired": True},
        "POST /ota accepts 0xE9 magic and returns the firmware response",
    )
    expect_error(result, 400, "ota_invalid", base, "POST", "/api/v1/ota", b"\x00badmagic",
                 "POST /ota rejects a body whose first byte is not 0xE9")
    expect_error(result, 400, "ota_unavailable", base, "POST", "/api/v1/ota", b"",
                 "POST /ota rejects an empty body")

    # -------------------------------------------------------------- restart
    status, _h, body = call(base, "POST", "/api/v1/restart", {})
    result.check(status == 200 and json.loads(body) == {"ok": True, "restartingInMs": 400},
                 "POST /restart is a 200 no-op with the firmware response")

    # --------------------------------------------------------------- network
    status, _h, net = get_json(base, "/api/v1/network")
    result.check(
        status == 200
        and set(net) >= {"mode", "staEnabled", "staConnected", "staSsid", "staIp",
                         "apSsid", "apIp", "rssi", "hasPassword", "saved"}
        and all({"ssid"}.issubset(item.keys()) for item in net["saved"]),
        "GET /network exposes boost_web.c network status keys",
    )
    status, _h, body = call(base, "PUT", "/api/v1/network",
                            {"mode": "ap", "ssid": "NewNet", "password": "pw",
                             "keepPassword": False})
    net = json.loads(body)
    result.check(
        status == 200 and net["mode"] == "ap" and net["staEnabled"] is False
        and net["staConnected"] is False
        and any(item["ssid"] == "NewNet" for item in net["saved"]),
        "PUT /network applies mode ap and saves the SSID",
    )
    expect_error(result, 400, "invalid_mode", base, "PUT", "/api/v1/network",
                 {"mode": "bogus"}, "PUT /network rejects an invalid mode")
    status, _h, body = call(base, "PUT", "/api/v1/network",
                            {"mode": "apsta", "ssid": "PitWall-5G", "keepPassword": True})
    result.check(status == 200 and json.loads(body)["staConnected"] is True,
                 "PUT /network re-enables sta in apsta mode")
    status, _h, body = call(base, "DELETE", "/api/v1/network", {"ssid": "Garage-IoT"})
    net = json.loads(body)
    result.check(status == 200 and not any(item["ssid"] == "Garage-IoT" for item in net["saved"]),
                 "DELETE /network removes a saved SSID (JSON body, per boost_web.c)")
    expect_error(result, 400, "missing_ssid", base, "DELETE", "/api/v1/network", {},
                 "DELETE /network rejects a missing ssid")
    expect_error(result, 404, "not_found", base, "DELETE", "/api/v1/network",
                 {"ssid": "DoesNotExist"}, "DELETE /network 404s for an unknown SSID")
    status, _h, body = call(base, "POST", "/api/v1/network/reconnect", {})
    net = json.loads(body)
    result.check(status == 200 and "mode" in net and "saved" in net,
                 "POST /network/reconnect returns the network status")
    status, _h, scan = get_json(base, "/api/v1/network/scan")
    result.check(
        status == 200 and "networks" in scan
        and all({"ssid", "rssi", "auth"}.issubset(n.keys()) for n in scan["networks"]),
        "GET /network/scan returns ssid/rssi/auth records",
    )
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"scanFail": "scan busy"})
    result.check(status == 200, "mock scan fault armed")
    expect_error(result, 400, "scan_failed", base, "GET", "/api/v1/network/scan", None,
                 "failed scan mirrors 400 scan_failed")
    status, _h, body = call(base, "PUT", "/api/v1/mock/sensors", {"scanFail": None})
    result.check(status == 200, "mock scan fault cleared")

    # ----------------------------------------------------------- 404 quirks
    expect_error(result, 404, "not_found", base, "GET", "/api/v1/bogus", None,
                 "unknown /api/v1 path mirrors the wildcard asset handler 404")
    expect_error(result, 404, "not_found", base, "GET", "/api/v1/themes/config", None,
                 "GET /themes/config is not registered on the device (404 quirk)")

    # -------------------------------------------------------------- static
    status, headers, body = call(base, "GET", "/")
    result.check(status == 200 and "text/html" in headers.get("Content-Type", ""),
                 "GET / serves the dashboard index")


def main() -> int:
    result = Result()
    try:
        server = BoostMockServer(port=0, seed=20260823, verbose=False).start()
    except OSError as error:
        print(f"FAIL could not start BoostMockServer: {error}")
        return 1
    try:
        print(f"mock server on {server.base_url}")
        run_tests(server.base_url, result)
    finally:
        server.stop()
    passed, failed = result.summary()
    print(f"\n{passed - failed}/{passed} checks passed, {failed} failed")
    if failed:
        print("failures:")
        for label in result.failures:
            print(f"  - {label}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
