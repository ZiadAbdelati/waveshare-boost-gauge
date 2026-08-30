# Boost Gauge Backplane (tscircuit)

A Ø46 mm rear daughterboard for the Waveshare ESP32-S3-Touch-AMOLED-1.75 that
powers and reads the car's analog boost sensors, adds a BMP280 + DS3231, and
mates face-to-face to the Waveshare's 8-pin H2 expansion header.

## Status

**Design complete, all gates green, awaiting user pre-order checks** (see
`DESIGN_REVIEW.md` §9).

| Gate | Result |
|---|---|
| `tsci check netlist` | 0 errors / 0 warnings |
| `tsci check placement` | no placement issues |
| `tsci check shorts` | no shorts |
| `tsci build` | Circuits 1 passed; autorouted 121 traces / 103 vias |

## What it does

- **Power**: car 12 V (J1) → F1 fuse → SMBJ16A TVS → AO3401A reverse-protection
  P-FET → LMR36520 synchronous buck → 5.016 V → VBUS ORing (SS54) → F2 PTC →
  SENSOR_5V rail for the sensors. USB VBUS from the Waveshare board ORs onto
  the same rail so the board is bench-configurable without car power.
- **Analog front end**: 4× JST-PH sensor inputs (J2–J5), each with 1 kΩ series
  + 100 nF filter (fc ≈ 1.6 kHz) + BAV99 rail clamps, into an ADS1115 4-ch
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
- `release/` — **tracked** fab-ready outputs of the verified build: Gerbers,
  drills, raw + JLC-filtered BOM/CPL, SVG renders, STEP, SHA256SUMS

## Regenerate outputs

```sh
tsci build index.circuit.tsx                 # circuit JSON + autoroute
tsci export index.circuit.tsx -f gerbers -o dist/gerbers
tsci export index.circuit.tsx -f pcb-svg -o dist/pcb.svg
tsci export index.circuit.tsx -f schematic-svg -o dist/schematic.svg
tsci export index.circuit.tsx -f step -o dist/board.step
```

## Ordering (JLCPCB)

- PCB: upload `release/*.gbr` + drill files (2 layer).
- Assembly: upload `release/bom_jlc.csv` and `release/cpl_jlc.csv`
  — these **exclude** the 7 hand-solder parts (J1, J2–J5, BT1, H2) which the
  raw exports still include.
- Hand-solder at assembly: the JST-PH connectors, CR1220 holder (install the
  cell only after soldering), and the H2 header.
- Checksums: `release/SHA256SUMS` covers every tracked artifact; verify after
  download from JLC.

## Before ordering — mandatory

See `DESIGN_REVIEW.md` §9: 1:1 paper overlay against the real Waveshare board,
continuity test of H2 pin 8 (GPIO16 vs GPIO18), H2 gender confirmation, USB-C
plug clearance.
