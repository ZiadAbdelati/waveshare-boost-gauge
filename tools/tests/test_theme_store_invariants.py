#!/usr/bin/env python3
"""Theme-store invariants (host-testable pieces) from the regression ledger.

Guards:
  * boost_theme.c:s_defaults[] is the single authoritative theme order: Dyno
    Cell, Vault-Tec, Night City, Big Digit, Neon (ledger rows 44-45, 74-75
    "theme system", 2026-08-10 audit).
  * Sport Cluster token absence: renderer, enum member, "sport" style token
    and web mirror are gone (2026-08-10 repo audit).
  * demoFastSweep is persisted SEPARATELY from demoMode (NVS key
    "demo_fast_sweep" vs "demo_mode"), loaded by boost_theme_init() before the
    sim ticks, and boost_sim_init() must never reset it (ledger 2026-08-15).
  * Firmware order must agree with the host mock (mock_server THEMES) and the
    BLE sim (routes.swift) - three-way drift guard.

Parses the C and Swift sources; nothing is compiled. Stdlib only.

Run:  python3 tools/tests/test_theme_store_invariants.py
"""

from __future__ import annotations

import os
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
THEME_C = REPO_ROOT / "main" / "boost_theme.c"
THEME_H = REPO_ROOT / "main" / "boost_theme.h"
SIM_C = REPO_ROOT / "main" / "boost_sim.c"
SIM_H = REPO_ROOT / "main" / "boost_sim.h"
ROUTES_SWIFT = REPO_ROOT / "tools" / "ble_gauge_sim" / "Sources" / "ble_gauge_sim" / "routes.swift"
MOCK_SERVER = REPO_ROOT / "tools" / "mock_server.py"

EXPECTED_THEMES = [
    ("dyno-cell", "Dyno Cell"),
    ("vault-tec", "Vault-Tec"),
    ("night-city", "Night City"),
    ("big-digit", "Big Digit"),
    ("neon", "Neon"),
]


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


def parse_s_defaults(source: str):
    """Extract (.id, .name, style) triples from s_defaults[] in declaration order."""
    block = re.search(r"s_defaults\[\]\s*=\s*\{(.*?)\n\};", source, re.S)
    if not block:
        return []
    out = []
    for entry in re.split(r"\n\s*\},?\n", block.group(1)):
        ids = re.search(r'\.id\s*=\s*"([^"]+)"', entry)
        names = re.search(r'\.name\s*=\s*"([^"]+)"', entry)
        styles = re.search(r"\.style\s*=\s*(\w+)", entry)
        if ids and names:
            out.append((ids.group(1), names.group(1), styles.group(1) if styles else None))
    return out


def style_token(style_enum: str, theme_c: str, theme_h: str) -> str | None:
    """Map BOOST_STYLE_* enum member -> JSON style token via boost_style_name().

    The default label in ``case BOOST_STYLE_ARC: default: return "arc";`` sits
    between the case and its return, so the scan spans to the first return
    inside the case block.
    """
    ctx = theme_c + theme_h
    m = re.search(rf"case\s+{style_enum}:[^;]*?return\s+\"([a-z]+)\";", ctx, re.S)
    return m.group(1) if m else None


def main() -> int:
    result = Result()
    theme_c = read(THEME_C)
    theme_h = read(THEME_H)
    sim_c = read(SIM_C)

    entries = parse_s_defaults(theme_c)
    result.check(len(entries) == len(EXPECTED_THEMES),
                 "s_defaults[] contains exactly 5 entries", f"got {len(entries)}")

    for idx, (entry, expected) in enumerate(zip(entries, EXPECTED_THEMES)):
        exp_id, exp_name = expected
        result.check(entry[0] == exp_id and entry[1] == exp_name,
                     f"s_defaults[{idx}] == {exp_id} ({exp_name})",
                     f"got {entry[:2]}")
        style_token_id = style_token(entry[2] if entry[2] else "", theme_c, theme_h)
        result.check(style_token_id is not None,
                     f"s_defaults[{idx}] style {entry[2]} has a JSON token",
                     f"got {style_token_id} for {entry[2]}")

    # Expected style token mapping (boost_style_name()).
    for style_enum, token in (("BOOST_STYLE_ARC", "arc"), ("BOOST_STYLE_VAULT", "vault"),
                              ("BOOST_STYLE_HUD", "hud"), ("BOOST_STYLE_BIGDIGIT", "bigdigit"),
                              ("BOOST_STYLE_NEON", "neon")):
        result.check(style_token(style_enum, theme_c, theme_h) == token,
                     f"boost_style_name maps {style_enum} -> {token!r}")

    # --- Sport Cluster absence (2026-08-10 repo audit) ----------------------
    banned = ("sport_cluster", "SPORT_CLUSTER", "BOOST_STYLE_SPORT",
              '"sport\\"', '"sport_cluster"', "sportCluster", "sport-cluster")
    found = []
    for root_dir, label in ((REPO_ROOT / "main", "main"),
                            (REPO_ROOT / "web", "web"),
                            (REPO_ROOT / "sim", "sim"),
                            (REPO_ROOT / "tools", "tools")):
        for path in sorted(root_dir.rglob("*")):
            if root_dir.name == "tools" and "tests" in path.parts:
                continue  # this test's own banned-token list lives here
            if not path.is_file() or path.suffix not in (".c", ".h", ".js", ".html", ".css", ".swift", ".py"):
                continue
            try:
                text = path.read_text(encoding="utf-8")
            except (OSError, UnicodeDecodeError):
                continue
            for token in banned:
                if token in text:
                    found.append(f"{path.relative_to(REPO_ROOT)}:{token}")
    result.check(not found, "sport-cluster tokens absent from code/art/tooling",
                 "; ".join(found[:5]) if found else "")

    # --- demoFastSweep persistence: separate flag, loaded before sim ticks ---
    fast_key = re.search(r'#define\s+NVS_KEY_FASTS\s+"([^"]+)"', theme_c)
    demo_key = re.search(r'#define\s+NVS_KEY_DEMO\s+"([^"]+)"', theme_c)
    result.check(fast_key is not None and fast_key.group(1) == "demo_fast_sweep",
                 "NVS key demo_fast_sweep exists",
                 f"got {fast_key.group(1) if fast_key else None}")
    result.check(demo_key is not None and demo_key.group(1) == "demo_mode",
                 "NVS key demo_mode exists (separate from fast sweep)",
                 f"got {demo_key.group(1) if demo_key else None}")

    # load: boost_theme_init() reads NVS_KEY_FASTS and applies via
    # boost_sim_set_fast_sweep() BEFORE boost_sim_init() may run.
    load = theme_c[theme_c.find("NVS_KEY_FASTS"):]
    result.check("nvs_get_u8(h, NVS_KEY_FASTS" in load and "boost_sim_set_fast_sweep(fs != 0)" in load,
                 "boost_theme_init() loads demo_fast_sweep and applies to the sim")

    # save: theme store persists both flags independently (u8 0/1 for each).
    save_block = theme_c[theme_c.find("nvs_set_u8(h, NVS_KEY_DEMO"):]
    result.check("nvs_set_u8(h, NVS_KEY_FASTS, boost_sim_fast_sweep() ? 1 : 0)" in theme_c,
                 "theme save persists demo_fast_sweep from the sim flag")
    result.check("nvs_set_u8(h, NVS_KEY_DEMO, s_demo_mode ? 1 : 0)" in theme_c,
                 "theme save persists demo_mode from its own flag")

    # boost_sim_init() must NOT reset the flag (ledger: boot value comes from
    # the theme store load).
    sim_init = sim_c[sim_c.find("boost_sim_init"):sim_c.find("}", sim_c.find("boost_sim_init")) + 1] if "boost_sim_init" in sim_c else ""
    result.check("Do NOT reset s_fast_sweep" in sim_c,
                 "boost_sim.c documents the no-reset invariant")
    result.check("s_fast_sweep = false" not in sim_init,
                 "boost_sim_init() does not reset s_fast_sweep")

    # --- Three-way order agreement --------------------------------------------
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    mock_text = read(MOCK_SERVER)
    mock_ids = re.findall(r'"id":\s*"([a-z0-9-]+)"\s*,\s*\n\s*"name":\s*"([^"]+)"', mock_text)
    mock_order = [(i, n) for i, n in mock_ids[:5]]
    fw_ids = [(e[0], e[1]) for e in entries]
    result.check(mock_order == EXPECTED_THEMES,
                 "mock_server THEMES order matches firmware",
                 f"mock={mock_order}")
    sim_text = read(ROUTES_SWIFT)
    sim_ids = re.findall(r'SimTheme\(id:\s*"([a-z0-9-]+)",\s*name:\s*"([^"]+)"\)', sim_text)
    result.check(sim_ids == EXPECTED_THEMES,
                 "ble_gauge_sim routes.swift theme order matches firmware",
                 f"sim={sim_ids}")

    passed = result.checks - len(result.failures)
    print(f"\n{passed}/{result.checks} checks passed, {len(result.failures)} failed")
    if result.failures:
        for label in result.failures:
            print(f"  - {label}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
