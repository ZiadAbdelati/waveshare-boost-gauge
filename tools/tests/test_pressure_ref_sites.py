#!/usr/bin/env python3
"""Pressure-reference CALL-SITE guard - source audit (no device, no compiler).

WHAT THIS IS
------------
A TEXT-LEVEL call-site guard, the companion to tools/test_pressure_ref.c. That
C test links ONLY the reference module and proves the module's formatters are a
byte-for-byte passthrough of the pre-reference boost_units_*() in Relative mode;
it cannot see whether the gauge still calls them. Reverting one of the ~20
boost-side call sites in main/boost_gauge.c back to the raw boost_units_*()
formatter would drop the reference from that numeral with the whole C suite
still green.

This audit parses main/boost_gauge.c and requires:

  * the dial tick text goes through boost_pressure_ref_format_tick(), never the
    raw boost_units_format_tick();
  * every PEAK / PK label buffer is produced by boost_pressure_ref_format();
  * the live readouts (arc, HUD, big-digit, neon, vault) route through
    boost_pressure_ref_display();
  * the only raw unit formatters left are the two documented exceptions in
    vault_build_cells / update_bigdigit, which apply the reference to their
    input FIRST and then format the already-adjusted value;
  * the reference module's public surface exposes exactly one setter (the
    atmospheric baseline) - no hidden "write the displayed value back" entry
    point. (This is the invariant the old tautological memcmp in
    tools/test_pressure_ref.c claimed to prove.)

LIMITS, stated honestly: this is a text-level guard. It proves the call sites
are spelled with the wrapper; it CANNOT see whether the wrapper's boolean
argument (`fold_deadband`) is the right one, nor whether the value passed is
the right sample. Those are runtime properties the C tests and the panel tests
cover. It also matches by name, so a rename of the wrapper would make it fail
loudly (see the empty-set checks) rather than silently pass.

Stdlib only. Run:  python3 tools/tests/test_pressure_ref_sites.py
"""

from __future__ import annotations

import pathlib
import re

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
GAUGE_C = REPO_ROOT / "main" / "boost_gauge.c"
REF_H = REPO_ROOT / "main" / "boost_pressure_ref.h"


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


def function_body(source: str, start_marker: str) -> str:
    """Slice from start_marker to the first '\\n}\\n' - good enough for these
    flat C functions and keeps the checks anchored to real code."""
    begin = source.index(start_marker)
    chunk = source[begin:]
    return chunk[: chunk.index("\n}\n")]


# A top-level function DEFINITION line: no leading whitespace, not a prototype
# (no ';' or '{' on the line - multi-line signatures continue with a ',').
FUNC_DEF = re.compile(
    r"^(?:static\s+)?[A-Za-z_][A-Za-z0-9_ \t\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*$",
    re.M,
)


def enclosing_function(src: str, pos: int) -> str | None:
    """Name of the nearest preceding top-level function definition."""
    name = None
    for m in FUNC_DEF.finditer(src, 0, pos):
        name = m.group(1)
    return name


# The RAW unit formatters. boost_units_format\s*\( does NOT match the
# _scaled / _tick variants because an '_' follows "format" there.
RAW_PATTERNS = {
    "boost_units_format": re.compile(r"\bboost_units_format\s*\("),
    "boost_units_format_tick": re.compile(r"\bboost_units_format_tick\s*\("),
    "boost_units_from_psi": re.compile(r"\bboost_units_from_psi\s*\("),
    "boost_units_format_scaled": re.compile(r"\bboost_units_format_scaled\s*\("),
}

# (function, raw formatter) pairs allowed to call the raw formatter because
# they have ALREADY pushed their input through boost_pressure_ref_display():
# what they format is no longer gauge psi.
RAW_ALLOW = {
    ("vault_build_cells", "boost_units_format"):
        "vault_build_cells pre-applies boost_pressure_ref_display(psi, false)",
    ("update_bigdigit", "boost_units_from_psi"):
        "update_bigdigit converts the already-reference-adjusted readout_psi",
    ("update_bigdigit", "boost_units_format_scaled"):
        "update_bigdigit formats the converted, reference-adjusted magnitude",
}

# A PEAK / PK label snprintf (not the "PEAK 0.0" placeholder in a build fn).
PEAK_LABEL = re.compile(r'snprintf\s*\([^;]*?"(?:PEAK|PK)[ "]')


def main() -> int:
    result = Result()
    gauge_c = GAUGE_C.read_text(encoding="utf-8")
    ref_h = REF_H.read_text(encoding="utf-8")
    lines = gauge_c.splitlines()

    # --- 0. the guard must actually be matching something -------------------
    ref_count = len(re.findall(r"\bboost_pressure_ref_\w+\s*\(", gauge_c))
    result.check(ref_count > 0,
                 "the guard found boost_pressure_ref_* call sites in boost_gauge.c",
                 "zero matches - the wrapper was renamed or the file moved; the "
                 "call-site guard is silently disabled, fix the pattern")

    # --- 1. dial tick text --------------------------------------------------
    result.check("boost_pressure_ref_format_tick(" in gauge_c,
                 "dial tick numerals go through boost_pressure_ref_format_tick()",
                 "no boost_pressure_ref_format_tick() call found")
    raw_ticks = [i + 1 for i, ln in enumerate(lines)
                 if "boost_units_format_tick(" in ln]
    result.check(not raw_ticks,
                 "no raw boost_units_format_tick() left in boost_gauge.c",
                 f"raw tick formatter at line(s) {raw_ticks}; the dial numerals "
                 "must carry the reference")

    # --- 2. PEAK / PK labels ------------------------------------------------
    peak_sites = [i for i, ln in enumerate(lines) if PEAK_LABEL.search(ln)]
    result.check(len(peak_sites) > 0,
                 "the guard found PEAK/PK label sites in boost_gauge.c",
                 "zero matches - a rename disabled this check")
    for i in peak_sites:
        # The reference format call sits immediately above the label, except in
        # the big-digit DEMO/plain pair where a comment + if/else interposes.
        window = "\n".join(lines[max(0, i - 6):i])
        result.check("boost_pressure_ref_format(" in window,
                     f"PEAK label at boost_gauge.c:{i + 1} is built by "
                     "boost_pressure_ref_format()",
                     "peak buffer not produced by the reference wrapper")

    # --- 3. live readouts route through boost_pressure_ref_display() --------
    arc_body = function_body(gauge_c, "static float arc_readout_display_psi(float psi)")
    result.check("boost_pressure_ref_display(psi, true)" in arc_body,
                 "arc readout (arc_readout_display_psi) applies the reference",
                 "arc readout bypasses boost_pressure_ref_display()")
    result.check(re.search(
        r"boost_neon_layout_readout_raw\s*\(\s*boost_pressure_ref_display\s*\(",
        gauge_c) is not None,
        "neon readout applies the reference before the fold-free layout",
        "boost_neon_layout_readout_raw() is not fed boost_pressure_ref_display()")
    hud_body = function_body(
        gauge_c, "static void update_hud(const boost_sample_t *sample, const boost_theme_t *theme)")
    result.check("boost_pressure_ref_display(sample->psi, true)" in hud_body,
                 "night-city HUD readout applies the reference",
                 "update_hud reads raw sample->psi")
    big_body = function_body(
        gauge_c, "static void update_bigdigit(const boost_sample_t *sample, const boost_theme_t *theme)")
    result.check("boost_pressure_ref_display(sample->psi, true)" in big_body,
                 "big-digit readout applies the reference",
                 "update_bigdigit reads raw sample->psi")
    vault_body = function_body(gauge_c, "static void vault_build_cells(float psi)")
    result.check("boost_pressure_ref_display(psi, false)" in vault_body,
                 "vault readout takes the reference (without the fold)",
                 "vault_build_cells reads raw psi")

    # --- 4. raw formatters only at the documented exceptions ----------------
    seen_pairs = set()
    unexpected = []
    for formatter, pattern in RAW_PATTERNS.items():
        for m in pattern.finditer(gauge_c):
            func = enclosing_function(gauge_c, m.start())
            line = gauge_c.count("\n", 0, m.start()) + 1
            pair = (func, formatter)
            if pair in RAW_ALLOW:
                seen_pairs.add(pair)
            else:
                unexpected.append((line, func, formatter))
    result.check(not unexpected,
                 "no raw unit formatter outside the documented post-reference "
                 "exceptions",
                 "; ".join(f"line {line}: {formatter}() in {func}() must route "
                           "through boost_pressure_ref_*"
                           for line, func, formatter in unexpected))
    missing = sorted(set(RAW_ALLOW) - seen_pairs)
    result.check(not missing,
                 "every allowlisted raw formatter exception is still present",
                 f"missing {missing} - a rename (e.g. of boost_units_format) "
                 "would silently disable those checks; fix the pattern")

    # --- 5. the module's public surface has one setter ----------------------
    setters = sorted(set(re.findall(r"\bboost_pressure_ref_set_\w+", ref_h)))
    result.check(setters == ["boost_pressure_ref_set_atmosphere_kpa"],
                 "boost_pressure_ref.h exposes exactly one setter - the "
                 "atmospheric baseline",
                 f"setters={setters}; a display/wire setter must not exist")
    result.check(re.search(r"boost_pressure_ref_\w*(?:write|store|commit|publish|push)\w*", ref_h) is None,
                 "boost_pressure_ref.h exposes no write-back helper",
                 "the module must never write a displayed psi back to the wire")

    print()
    if result.failures:
        print(f"{len(result.failures)} failure(s) of {result.checks} checks")
        return 1
    print(f"all {result.checks} checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
