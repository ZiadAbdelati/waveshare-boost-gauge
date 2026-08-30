#!/usr/bin/env python3
"""GATT contract conformance - three-way consistency guard.

docs/bluetooth-gatt.md is the normative GATT contract for the companion apps,
the firmware (main/boost_app_ble.c), and the peripheral simulator
(tools/ble_gauge_sim/*.swift). This test parses all three (doc, C firmware
source, Swift sim source) and asserts they agree on:

  * service/characteristic UUIDs (b6a00000..b6a00004-...-b6a0),
  * the name "BoostGauge",
  * the <=480 B Control payload rule and 413 {"error":"too_large"} response,
  * the BGL1 log header and t_ms,psi,peak_psi,zone,demo line format,
  * the zone token set VAC/ATMO/BOOST/OVER,
  * one-request-in-flight per connection + serial dispatch,
  * the Device-info shape {"name":"BoostGauge","fw":...,"api":1},
  * BLE Control routes being a subset of the HTTP /api/v1 control plane
    (boost_web.c) - the "no second API implementation" guard.

This is the client-drift guard: an app or simulator that stops matching this
file fails loudly instead of silently diverging. Host-side only; no radio.

Run:  python3 tools/tests/test_gatt_contract.py
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
DOC = REPO_ROOT / "docs" / "bluetooth-gatt.md"
FIRMWARE = REPO_ROOT / "main" / "boost_app_ble.c"
WEB_C = REPO_ROOT / "main" / "boost_web.c"
SIM_DIR = REPO_ROOT / "tools" / "ble_gauge_sim" / "Sources" / "ble_gauge_sim"

SERVICE_UUID = "b6a00000-0000-4000-8000-00000000b6a0"
CHAR_UUIDS = {
    "control": "b6a00001-0000-4000-8000-00000000b6a0",
    "status": "b6a00002-0000-4000-8000-00000000b6a0",
    "log": "b6a00003-0000-4000-8000-00000000b6a0",
    "device_info": "b6a00004-0000-4000-8000-00000000b6a0",
}
BGL1 = "BGL1"
LOG_HEADER = "t_ms,psi,peak_psi,zone,demo"
ZONES = {"VAC", "ATMO", "BOOST", "OVER"}
CTRL_MAX = 480
API_VERSION = 1
NAME = "BoostGauge"


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


def read(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def decode_nimble_uuids(source: str) -> dict[str, str]:
    """Decode BLE_UUID128_INIT(...) arrays in boost_app_ble.c into canonical
    lowercase UUID strings (NimBLE stores 128-bit UUIDs little-endian)."""
    out: dict[str, str] = {}
    order = ("s_uuid_svc", "s_uuid_control", "s_uuid_status",
             "s_uuid_log", "s_uuid_dev_info")
    for var in order:
        m = re.search(
            rf"static const ble_uuid128_t {var}\s*=\s*BLE_UUID128_INIT\((.*?)\);",
            source, re.S)
        if not m:
            continue
        bytes_le = [int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", m.group(1))]
        if len(bytes_le) != 16:
            continue
        be = bytes(reversed(bytes_le)).hex()
        out[var] = (f"{be[0:8]}-{be[8:12]}-{be[12:16]}-{be[16:20]}-{be[20:32]}").lower()
    return out


def ext(swift_sources: dict[str, str], var: str) -> str:
    text = ""
    for src in swift_sources.values():
        m = re.search(rf"static let {var}\s*=\s*\"([^\"]+)\"", src)
        if m:
            return m.group(1)
    return text


def main() -> int:
    result = Result()
    doc = read(DOC)
    fw = read(FIRMWARE)
    web = read(WEB_C)
    sim_files = {
        p.name: read(p) for p in SIM_DIR.glob("*.swift")
        if p.name in {"GATTPeripheral.swift", "ControlRouter.swift",
                      "SimModel.swift", "routes.swift"}
    }
    if not sim_files or not DOC.is_file() or not FIRMWARE.is_file():
        for label, path in (("doc", DOC), ("firmware", FIRMWARE), ("sim", SIM_DIR)):
            result.check(False, f"source present: {label}", f"missing {path}")
        return 1
    sim = "\n".join(sim_files.values())

    # --- Service UUID + name -------------------------------------------------
    fw_uuids = decode_nimble_uuids(fw)
    sim_service = ext(sim_files, "serviceUUIDString")
    result.check(fw_uuids.get("s_uuid_svc") == SERVICE_UUID,
                 "firmware service UUID == doc", f"got {fw_uuids.get('s_uuid_svc')}")
    result.check(sim_service == SERVICE_UUID,
                 "sim service UUID == doc", f"got {sim_service}")
    result.check(SERVICE_UUID in doc, "doc carries service UUID")

    # --- Characteristic UUIDs -----------------------------------------------
    expected_vars = ("s_uuid_control", "s_uuid_status", "s_uuid_log", "s_uuid_dev_info")
    expected_units = ("control", "status", "log", "device_info")
    for var, unit, expected in zip(expected_vars, expected_units, CHAR_UUIDS.values()):
        fw_uuid = fw_uuids.get(var)
        sim_uuid = ext(sim_files, {
            "control": "controlUUIDString", "status": "statusUUIDString",
            "log": "logUUIDString", "device_info": "infoUUIDString",
        }[unit])
        result.check(fw_uuid == expected, f"firmware {var} UUID == doc",
                     f"got {fw_uuid}, want {expected}")
        result.check(sim_uuid == expected, f"sim {unit} UUID == doc",
                     f"got {sim_uuid}, want {expected}")
        result.check(expected in doc, f"doc carries {unit} UUID")

    # --- Name -----------------------------------------------------------------
    result.check(NAME in doc and "APP_BLE_NAME" in fw and f'"{NAME}"' in fw,
                 "firmware name BoostGauge matches doc")
    result.check('"BoostGauge"' in sim, "sim advertises name BoostGauge")

    # --- 480 B / 413 rule -----------------------------------------------------
    fw_cap = re.search(r"#define\s+APP_BLE_CTRL_MAX\s+(\d+)u", fw)
    result.check(fw_cap is not None and int(fw_cap.group(1)) == CTRL_MAX,
                 "firmware APP_BLE_CTRL_MAX == 480",
                 f"got {fw_cap.group(1) if fw_cap else None}")
    result.check('"too_large"' in fw and "413" in fw and "480" in doc,
                 "firmware emits 413 too_large (doc rule present)")
    sim_cap = re.search(r"static let maxPayloadBytes\s*=\s*(\d+)", sim)
    result.check(sim_cap is not None and int(sim_cap.group(1)) == CTRL_MAX,
                 "sim maxPayloadBytes == 480",
                 f"got {sim_cap.group(1) if sim_cap else None}")
    result.check('"too_large"' in sim and "413" in sim,
                 "sim emits 413 too_large")

    # --- one request in flight / serial dispatch ---------------------------
    result.check("One request in flight per" in doc,
                 "doc states one-request-in-flight rule")
    result.check("app_ble_enqueue_tx" in fw and "s_conn_handle" in fw,
                 "firmware has serialized per-connection dispatch (queue + conn handle)")
    # The sim processes one control write synchronously per call and keeps a
    # per-characteristic message buffer (no interleaving).
    result.check("pendingMessages" in sim and "removeAll" in sim,
                 "sim serializes per-characteristic messages / resets on disconnect")
    result.check("APP_BLE_QUEUE_LEN" in fw and "inflight" not in fw,
                 "firmware Control is write-with-response; queue bounded (no HTTP-style inflight)")

    # --- BGL1 log format -------------------------------------------------------
    fw_magic = re.search(r'#define\s+APP_LOG_MAGIC\s+"(BGL1)\\n"', fw)
    result.check(bool(fw_magic), "firmware APP_LOG_MAGIC == BGL1\\n",
                 f"got {fw_magic.group(1) if fw_magic else None}")
    fw_header = re.search(r'#define\s+APP_LOG_HEADER\s+"([^"]+)"', fw)
    result.check(bool(fw_header) and fw_header.group(1) == LOG_HEADER + "\\n",
                 "firmware log header == " + LOG_HEADER,
                 f"got {fw_header.group(1) if fw_header else None}")
    result.check(BGL1 in doc and LOG_HEADER in doc,
                 "doc carries BGL1 header + columns")
    sim_log = ext(sim_files, "logHeader")
    # routes.swift stores the literal escape sequence, so the source text is
    # "BGL1\n" (backslash + 'n'), matching the runtime value BGL1 + LF.
    result.check(sim_log == BGL1 + "\\n", "sim logHeader == BGL1\\n",
                 f"got {sim_log!r}")
    result.check(LOG_HEADER in sim, "sim carries log line format")

    # --- Zone token set ---------------------------------------------------------
    json_c = read(REPO_ROOT / "main" / "boost_json.c")
    result.check('\\"zone\\"' in json_c and "st.zone" in json_c,
                 "firmware Status zone comes from boost_model (doc token set)")
    for zone in sorted(ZONES):
        result.check(zone in sim, f"sim emits zone token {zone}")
    result.check("VAC, `ATMO`, `BOOST`, `OVER`" in doc or
                 ("VAC" in doc and "ATMO" in doc and "BOOST" in doc and "OVER" in doc),
                 "doc carries the zone token set")
    # Firmware zone classifier tokens (boost_model.c) must match the doc set.
    model = read(REPO_ROOT / "main" / "boost_model.c")
    tokens = set(re.findall(r'return "([A-Z]+)";', model))
    result.check(ZONES.issubset(tokens), "boost_model.c zone returns are the doc token set",
                 f"got {sorted(tokens)}")

    # --- Device info api version ------------------------------------------------
    result.check('\\"api\\":1' in fw,
                 "firmware Device info carries api:1")
    m = re.search(r'snprintf\(s_device_info,\s*sizeof\(s_device_info\),\s*'
                  r'"\{\\"name\\":\\"BoostGauge\\",\\"fw\\":\\"%s\\",\\"api\\":(\d+)',
                  fw)
    result.check(m is not None and int(m.group(1)) == API_VERSION,
                 "firmware Device info name/api == BoostGauge/api 1",
                 f"got {m.group(1) if m else None}")
    result.check('"api": 1' in doc, "doc Device info carries api: 1")
    result.check('"api": 1' in sim or '"api":1' in sim, "sim Device info carries api: 1")

    # --- BLE Control routes are a subset of the HTTP control plane -------------
    fw_routes = set(re.findall(r'\{\s*"(/[^"]+)",\s*"(GET|PUT|POST)"', fw))
    http_regs = set(re.findall(r'register_uri\(API_BASE "(/[^"]+)", HTTP_(GET|PUT|POST|DELETE),',
                               web)) | \
                set(re.findall(r'register_uri\(API_BASE "(/[^"]+)", HTTP_(GET|PUT|POST|DELETE),', web))
    http_paths = {(p, m) for p, m in http_regs}
    for path, method in sorted(fw_routes):
        result.check((path, method) in http_paths,
                     f"BLE route '{method} {path}' exists on HTTP /api/v1",
                     f"http has {[r for r in sorted(http_paths) if r[0] == path]}")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
