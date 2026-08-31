# Boost Gauge Backplane (tscircuit)

A Ø46 mm rear daughterboard for the Waveshare ESP32-S3-Touch-AMOLED-1.75 that
powers and reads the car's analog boost sensors, adds a BMP280 + DS3231, and
mates face-to-face to the Waveshare's 8-pin H2 expansion header.

## Status

**Source repair and automated PCB checks complete; physical pre-order gates
remain** (see `DESIGN_REVIEW.md` §7). The tracked release files are release
candidates, not fabrication approval, until those checks are recorded.

| Gate | Result |
|---|---|
| `tsci check netlist` | 0 errors / 0 warnings |
| `tsci check placement` | 0 errors; 3 JST access-direction advisories |
| final circuit JSON | 0 error records, 0 long-trace warnings, 0 unrouted records |
| `tsci check shorts --mode pcb` | toolchain-blocked: `layerCanvas.getContext is not a function` twice |
| `tsci build` | autorouted 128 connections / 142 PCB traces / 106 vias, 0 router errors |

## What it does

- **Power**: car 12 V (J1) → F1 fuse → bidirectional SMBJ16CA TVS → 40 V
  DMP4065S reverse-protection P-FET → LMR36520 synchronous buck → 5.016 V →
  VBUS ORing (SS54) → 500 mA-hold F2 PTC →
  SENSOR_5V rail for the sensors. USB VBUS from the Waveshare board ORs onto
  the same rail so the board is bench-configurable without car power.
- **Analog front end**: 4× JST-PH sensor inputs (J2–J5), each with 1 kΩ series
  + 100 nF filter (fc ≈ 1.6 kHz) + BAT54S rail clamps, into an ADS1115 4-ch
  16-bit ADC on the 5 V side of a TCA9406 I2C level translator.
- **Environment**: BMP280 (baro) and DS3231MZ RTC (CR1220 backup) on the 3.3 V
  side, sharing the Waveshare's GPIO17/18 I2C bus through H2.
- **Mechanical**: Ø46.00 mm circle, 3× Ø2.2 NPTH mounts at the Waveshare's
  brass standoffs, H2 1×8 2.54 mm header at y=−18.68 (pin 1 = VBUS).

## Files

- `index.circuit.tsx` — the whole design (schematic + placement + form factor)
- `imports/` — LCSC-verified component models (`tsci import` outputs)
- `reference/` — mechanical mating spec (authoritative) + Waveshare PDFs
- `PLAN.md` — design history, decisions, session log
- `DESIGN_REVIEW.md` — bug audit with evidence, power-path math, gate status
- `dist/` — gitignored build output (regenerate with the commands below)
- `release/` — **tracked** release-candidate outputs of the verified build: Gerbers,
  drills, raw + JLC-filtered BOM/CPL, SVG renders, STEP, SHA256SUMS

## Regenerate outputs

```sh
npm run typecheck
tsci build index.circuit.tsx

# Export the verified route, not a second independent source reroute.
tsci export dist/index/circuit.json -f gerbers -o dist/gerbers
tsci export dist/index/circuit.json -f pcb-svg -o dist/pcb.svg
tsci export dist/index/circuit.json -f schematic-svg -o dist/schematic.svg
tsci export dist/index/circuit.json -f assembly-svg -o dist/assembly.svg
tsci export dist/index/circuit.json -f step -o dist/board.step
```

The Gerber export is a ZIP. Unpack it into `dist/gerbers_dir`, then generate
`bom_jlc.csv` and `cpl_jlc.csv` by excluding `BT1,H2,J1,J2,J3,J4,J5` and every
`TP_*` designator. Validate the archive, exact excluded set, supplier IDs, and
matching filtered BOM/CPL designator sets before explicitly promoting the 19
release files and regenerating `release/SHA256SUMS`. Verify checksums from this
directory with `shasum -a 256 -c release/SHA256SUMS`.

## Ordering (JLCPCB)

- PCB: upload `release/*.gbr` + drill files (2 layer).
- Assembly: upload `release/bom_jlc.csv` and `release/cpl_jlc.csv`
  — these **exclude** the 7 hand-solder parts (J1, J2–J5, BT1, H2) and 17
  PCB-only `TP_*` pads which the raw exports still include.
- Hand-solder at assembly: the bottom/outward JST-PH connectors, the H2 mating
  header, and—only after stack-clearance validation—the top/mating-face
  BS-12-B2AA002 horizontal SMT CR1220 holder. Install the cell after soldering.
- Checksums: `release/SHA256SUMS` covers every tracked artifact; verify after
  download from JLC.

## Before ordering — mandatory

See `DESIGN_REVIEW.md` §7. Required checks include a 1:1 overlay and H2 pinout,
gender, stack-height, and continuity confirmation; measured inter-board
separation and loaded BT1 clearance; bottom connector and cable access against
the flush mount; USB-C plug clearance; BMP280 vent clearance; manual U1 land
pattern comparison; an independent PCB-short DRC; and electrical/thermal bench
validation before installation in a vehicle.
