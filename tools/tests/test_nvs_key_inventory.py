#!/usr/bin/env python3
"""Config/NVS key inventory test - every persisted key gets a default and the
bounds-check the ledger demands at restore time.

Ledger guards exercised here:
  * tpms lowKpa/staleAfterMs bounds (100-400 kPa, 2000-120000 ms) - 2026-08-15
  * brightness clamps to 0..100 (clamp_percent) - boot/NVS/RAM row
  * pixelShiftSec clamped to BOOST_PXSHIFT_SEC_MIN/MAX (30/3600) - 2026-08-11
  * rotation restored only for 0/90/180/270 - theme persistence row
  * saved-network count capped at BOOST_NET_MAX_SAVED (5) - 2026-08-15
  * MAP supply (4.50-5.50 V) and calibration record schema gating - 2026-08-06
  * absent keys keep compiled defaults (every nvs_get is ESP_OK-gated) - 2026-07-25
  * app_ble default OFF so a fresh boot never advertises - 2026-08-23 plan

Stdlib only. Run:  python3 tools/tests/test_nvs_key_inventory.py
"""

from __future__ import annotations

import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
MAIN = REPO_ROOT / "main"


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


def sources() -> dict[str, str]:
    out = {}
    for path in sorted(MAIN.glob("*.c")) + sorted(MAIN.glob("*.h")):
        out[path.name] = path.read_text(encoding="utf-8")
    return out


def main() -> int:
    result = Result()
    src = sources()
    all_text = "\n".join(src.values())

    # --- Inventory: every NVS key definition across main/ --------------------
    key_defs = []
    for fname, text in src.items():
        for m in re.finditer(r'#define\s+((?:NVS_KEY|TPMS_NVS|APP_NVS_KEY)_\w+)\s+"([^"]+)"', text):
            key_defs.append((fname, m.group(1), m.group(2)))
    result.check(len(key_defs) >= 20, f"found {len(key_defs)} NVS key definitions (>= 20)")

    defined = {name: value for _, name, value in key_defs}
    for name, value in sorted(defined.items()):
        result.check(len(value) <= 15,
                     f"NVS key {name!r} ({value!r}) <= 15 chars",
                     f"len={len(value)}")

    # Every defined base key is either written by the firmware (nvs_set_*) or is
    # an explicit read-only prefix used with an index suffix.
    write_ok = True
    for fname, name, _ in key_defs:
        if name.endswith("_PFX") or name.endswith("_NS"):
            continue  # indexed prefix keys / namespace names are not keys
        if not re.search(rf"nvs_set_\w+\([^,]*,\s*{re.escape(name)}", all_text):
            write_ok = False
            result.check(False, f"NVS key {name} is written somewhere",
                         "no nvs_set_* call found")
    if write_ok:
        result.check(True, "every persisted NVS key has a nvs_set_* writer")

    # Absent key keeps defaults: every nvs_get_* restore is ESP_OK-gated.
    ungated = []
    for m in re.finditer(r"nvs_get_\w+\([^;]*?\)", all_text):
        call = m.group(0)
        # A saved network's password blob is optional (the SSID entry decides
        # membership); its read is intentionally unchecked after zeroing.
        if re.search(r"nvs_get_str\(h, k_pass", call):
            continue
        if "== ESP_OK" not in all_text[m.end():m.end() + 90]:
            ungated.append(call[:80])
    result.check(not ungated, "every nvs_get_* restore is ESP_OK-gated (absent key -> default)",
                 "; ".join(ungated[:4]))

    theme = src["boost_theme.c"]
    theme_h = src["boost_theme.h"]
    tpms = src["boost_tpms.c"]
    model = src["boost_model.c"]
    network = src["boost_network.c"]
    network_h = src["boost_network.h"]
    sensors = src["boost_sensors.c"]
    sensors_h = src["boost_sensors.h"]
    app_ble = src["boost_app_ble.c"]

    # --- Bounds at restore: TPMS (ledger 2026-08-15) -------------------------
    result.check(re.search(r"nvs_get_u16\(h, TPMS_NVS_LOW, &low\) == ESP_OK && low >= 100 && low <= 400", tpms) is not None,
                 "TPMS low_kpa restore bounds 100..400")
    result.check(re.search(r"nvs_get_u32\(h, TPMS_NVS_STALE, &stale\) == ESP_OK && stale >= 2000 && stale <= 120000", tpms) is not None,
                 "TPMS stale_ms restore bounds 2000..120000")
    result.check("low_kpa < 100.0f || low_kpa > 400.0f" in tpms or "low_kpa > 400.0f" in tpms,
                 "TPMS set_config rejects lowKpa outside 100..400")

    # --- Brightness clamps 0..100 ---------------------------------------------
    clamp = re.search(r"int boost_brightness_clamp_percent\(int percent\)\s*\{\s*return percent < 0 \? 0 : \(percent > 100 \? 100 : percent\);\s*\}", src["boost_brightness.c"])
    result.check(clamp is not None, "boost_brightness.c clamp returns 0..100 (single owner)")
    result.check("boost_brightness_clamp_percent(" in model,
                 "config path clamps brightness through the shared clamp")
    result.check("static int clamp_percent" not in model,
                 "no second clamp_percent copy in boost_model.c")

    # --- pixelShiftSec bounds -------------------------------------------------
    result.check("#define BOOST_PXSHIFT_SEC_MIN     30u" in theme_h and "#define BOOST_PXSHIFT_SEC_MAX     3600u" in theme_h,
                 "BOOST_PXSHIFT_SEC_MIN/MAX == 30/3600")
    result.check("pxsec != 0" in theme and re.search(r"clamp_pxshift_sec|boost_theme_set_pixel_shift_sec", theme) is not None,
                 "pixelShiftSec restore is nonzero-guarded and clamped")

    # --- Rotation restore only 0/90/180/270 -----------------------------------
    result.check(re.search(r"nvs_get_u16\(h, NVS_KEY_ROT, &rot\) == ESP_OK\).*?s_rotation = \(rot == 90u \|\| rot == 180u \|\| rot == 270u\) \? rot : 0u;", theme, re.S) is not None,
                 "rotation restore accepts only 0/90/180/270")

    # --- Vault vignette restore 0..90 ------------------------------------------
    result.check("s_vault_vig_pct = vv > 90u ? 90u : vv;" in theme,
                 "vault vignette restore clamps to 90")

    # --- neon layout/preset clamped on restore ----------------------------------
    result.check("boost_neon_layout_clamp(nl)" in theme and "boost_neon_preset_clamp(np)" in theme,
                 "neon layout/preset restore clamped")

    # --- Saved network count capped at 5 (ledger 2026-08-15) -------------------
    result.check(re.search(r"#define\s+BOOST_NET_MAX_SAVED\s+5\b", network_h) is not None,
                 "BOOST_NET_MAX_SAVED == 5")
    result.check("saved_cnt > BOOST_NET_MAX_SAVED" in network and "saved_cnt = BOOST_NET_MAX_SAVED" in network,
                 "saved-network NVS restore caps count at 5")

    # --- MAP supply + calibration record schema gating (2026-08-06) -----------
    result.check("supply >= BOOST_MAP_SUPPLY_MIN && supply <= BOOST_MAP_SUPPLY_MAX" in sensors,
                 "map supply restore bounded by BOOST_MAP_SUPPLY_MIN/MAX")
    result.check("rec.version == BOOST_MAP_CAL_VERSION" in sensors and "rec_len == sizeof(rec)" in sensors,
                 "calibration restore gated on schema version + exact blob size")

    # --- app_ble default off (fresh boot never advertises) ----------------------
    result.check('#define APP_NVS_KEY          "app_ble"' in app_ble,
                 "app_ble persisted under NVS key app_ble")
    result.check(re.search(r"nvs_get_u8\(h, APP_NVS_KEY, &v\) == ESP_OK", app_ble) is not None,
                 "app_ble restore is ESP_OK-gated (absent key -> default off)")

    # --- Module persistence must not depend on another module's NVS init -------
    # NVS itself is mounted once at boot (boost_model.c: nvs_flash_init is
    # idempotent) and the modules that must read before the model does
    # (theme, sensors, app_ble) mount it themselves. Every other module's
    # restore is nvs_open-failure-tolerant, which is what the ledger guard
    # (2026-07-25) actually requires.
    result.check("nvs_flash_init()" in model,
                 "boost_model.c mounts NVS at boot (nvs_flash_init, idempotent)")
    for fname in ("boost_theme.c", "boost_sensors.c", "boost_app_ble.c"):
        result.check("nvs_flash_init()" in src[fname],
                     f"{fname} mounts NVS itself before its first nvs_open")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
