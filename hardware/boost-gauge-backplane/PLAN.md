# Boost Gauge Backplane — tscircuit Implementation Plan

Self-contained prompt file for a fresh session. Read this file, then implement.
Original agent spec: `reference/agent-spec.md` (also at the upload path in the
task). All Phase-1 research is DONE; decisions are LOCKED (user-approved).
Do not re-open them. Do not touch firmware in this repo — this work is
additive under `hardware/boost-gauge-backplane/` only.

**2026-08-30 STATUS: source repair and automated PCB audit complete; physical
pre-order gates remain.** Sections §11 and earlier part selections are retained
as history where explicitly marked. The current implementation and audit are
summarized in §12 and `DESIGN_REVIEW.md`.

## 0. Toolchain state (verified this session)

- `bun` installed via `npm install -g bun` (tsci shebang needs it). `tsci` v0.0.2461 works.
- Skill: `/Users/chaotic/.agents/skills/tscircuit` (CLI.md, SYNTAX.md, FOOTPRINTS.md, templates/).
- Project dir exists: `hardware/boost-gauge-backplane/` with `reference/` containing:
  - `waveshare-esp32-s3-touch-amoled-1.75-schematic.pdf` (official, 3 pages)
  - `waveshare-esp32-s3-touch-amoled-1.75-dimensions.pdf` (official drawing, 1 page, 4 views)
  - `waveshare-esp32-s3-touch-amoled-1.75-dimensions.dwg` / `.step` (official CAD)
  - `agent-spec.md`
- This model cannot read PDFs directly. Convert pages with the Swift+CoreGraphics
  script pattern used in Phase 1 (render to PNG at 3x, crop with `sips`), or read
  the PNG crops. A working snippet: `swift /tmp/pdf2png.swift file.pdf outprefix`
  (see git history of this session / rewrite: CGPDFDocument → CGContext → NSBitmapImageRep).
- Bootstrap: `tsci init -y` inside `hardware/boost-gauge-backplane/`, or hand-write
  `package.json`/`tsconfig`/`tscircuit.config.json` + `index.circuit.tsx`.

## 1. Locked architecture decisions (user-approved)

| # | Decision | Choice |
|---|---|---|
| 1 | Stack topology | **Rigid stack off H2.** Waveshare H2 is a **female** 1×8 socket on the board's **bottom** face, axis parallel to the board (pins perpendicular to PCB, pointing away from screen). Backplane carries a **male** 1×8 pin header on its **top** face (the face toward the Waveshare) that plugs down into H2. |
| 2 | Mounting | **Reuse the Waveshare's own 3× M2 pattern.** Backplane has 3 unplated Ø2.2 mm holes with explicit copper keepouts at the exact Waveshare coordinates. Screw, standoff, solder-protrusion, and clamp-up dimensions remain physical validation gates; the mating spec does not define the required separation. |
| 3 | USB/VBUS backfeed | **Single Schottky (SS54) from buck output to VBUS_SHARED.** User asked why: even if car+USB are never simultaneous, plugging USB with the car OFF back-feeds the buck's output node and the 12V rail through the buck's body diode/high-side FET, and the sensor harness. One $0.10 diode makes USB power the display (and via the sensor branch, the bench) with zero contention. ~0.3–0.45 V drop → VBUS_SHARED ≈ 4.6–4.7 V, inside the firmware's 4.50–5.50 V MAP-supply setting. No jumper, no ideal-diode controller. |
| 4 | RTC backup | **BS-12-B2AA002 / C964721 horizontal SMT CR1220 holder, no charging circuit**, on the top/mating/inside face and `doNotPlace`. It is allowed there only after the assembled board gap proves loaded-cell clearance; otherwise relocate it to the outward face. Accept +85 °C cell limit and document hot-dashboard risk. |
| 5 | Sensor/power connectors | **JST-PH 2.0 mm** (user chose PH over GH): 4× B3B-PH-K-S (A0–A3) + 1× B2B-PH-K-S (12V/GND), all through-hole vertical, on the bottom (outward) face, near the perimeter. |
| 6 | Board | Ø46.00 mm round, 2-layer, JLCPCB fab/assemble. **TOP = Waveshare-facing/mating/inside; BOTTOM = enclosure/mount-facing/outward/back.** Put electronics on TOP where feasible; J1–J5 remain BOTTOM for cable access. BT1 is TOP only conditionally per decision #4. |
| 7 | BMP280 placement | **Top (Waveshare-facing) face**, near the perimeter edge — user override. Prevents cabin AC vent airflow from blowing directly onto the vent hole. All other ICs also prefer top face if space allows; connectors remain bottom-only. |
| 8 | Assembly mode | **JLCPCB partial assembly.** JLC installs: ADC, pressure sensor, RTC, buck circuitry, MOSFETs, level shifter, resistors, capacitors, inductor, TVS, fuses, diodes. User hand-solders (doNotPlace): 8-pin display header, 4× sensor connectors (J2–J5), 12V power connector (J1), battery holder (BT1). |
| 9 | Geometry source | **`reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md` supersedes PLAN §4.** Verified coordinates: mounts (0,+20.50)/(±13.75,−14.70); H2 row centerline Y=−18.68, X-center 0, pins −8.89…+8.89 @2.54 pitch. PLAN §4's old numbers (±18.30/+12.80, 0/−20.50, header centerline −12.70/row −13.75) are **WRONG — discard**. User physical observation (rear view: VBUS rightmost, GPIO18 leftmost, USB-C right side) confirms spec's rear-view frame. USB-C notch: keep +X perimeter open instead of cutting the outline. |
| 10 | Daughterboard transform | Face-to-face mirror: **X_db = −X_assembly, Y unchanged.** In backplane top-view coords the H2 row is therefore **pin 1 (VBUS) at (−8.89, −18.68) … pin 8 (GPIO16) at (+8.89, −18.68)**, and the USB-C keepout moves to the **left edge (X ≈ −23, Y ≈ 0)**. Mount holes are symmetric, unchanged. Confirmed by user's physical observation. |

## 2. Verified firmware constraints (read from `main/boost_sensors.c/.h` this session)

- Sensor I²C: port 0, **SDA=GPIO17, SCL=GPIO18, 100 kHz**, proven with ~4.7 kΩ pull-ups.
- **ADS1115 @ 0x48** (ADDR→GND), A0 single-ended, **±6.144 V FSR**, continuous 250 SPS, **VDD = ~5 V** (GM MAP swings to ~4.8 V).
- **BMP280 @ 0x76** (SDO→GND), 3.3 V, firmware requires chip id **0x58** (no BME280/BMP3xx substitution).
- **DS3231 @ 0x68** on the same bus; firmware handles OSF, 2.3–5.5 V supply OK.
- MAP supply config 4.50–5.50 V (default 5.20 V) → **record measured SENSOR_5V in firmware/NVS after first build; no code change needed.**
- H2 GPIOs are 3.3 V, not 5 V tolerant. GPIO17/18 pull-ups must be on the **3V3** side only.

## 3. Waveshare H2 pinout (official schematic page 1, right side)

| H2 pin | Signal | Backplane net |
|---|---|---|
| 1 | VBUS (5 V) | VBUS_SHARED (via SS54 ORing anode side) |
| 2 | GND | GND |
| 3 | 3V3 | 3V3_WAV (input; powers BMP280/RTC/translator low side + pull-ups) |
| 4 | GPIO44 / U0RXD | TP only |
| 5 | GPIO43 / U0TXD | TP only |
| 6 | GPIO17 = SDA | I2C_SDA_3V3 |
| 7 | GPIO18 = SCL | I2C_SCL_3V3 |
| 8 | GPIO16 | TP only |

**Pin-1 orientation — RESOLVED 2026-08-29.** The uploaded mating spec gives the
authoritative rear/component-side view: pin 1 (VBUS) at X=+8.89 (USB-C side),
pin 8 (GPIO16) at X=−8.89. The user's physical observation (rear view: VBUS
rightmost, GPIO18 leftmost, USB-C right) confirms this frame. The old
front-view mirror derivation below is kept only as audit trail — **discard it.**
→ Pre-fab check remains: face-to-face assembly rendering / 1:1 print overlay
per spec §7's mandatory validation rule (mating contact → H2 pin 1 → VBUS).

## 4. Mechanical geometry — **SUPERSEDED by decision #9**

The authoritative source is now `reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md`.
Old PLAN numbers below are retained only for audit trail; **do not use them.**

<details><summary>Old (incorrect) geometry — click to expand</summary>

Origin = board center, +X right, +Y up, **viewed from the FRONT of the Waveshare** unless noted.

- PCB outline: **Ø46.00 mm** (display OD 48.96, AA 43.76 — irrelevant to PCB).
- H2 (female): 1×8 @ 2.54 mm; **centerline at X = −12.70 mm, row at Y = −13.75 mm** (front view),
  axis along X. Pin 1 at the +X end of the row (front view).
- Mounting: **3× M2-H3.5 tapped holes** at **(−18.30, +12.80), (+18.30, +12.80), (0.00, −20.50)** (front view).
  (Drawing also shows 18.68/14.70/23.00/23.80 chains — the ±18.30/+12.80/0/−20.50 set is what the
  photo dimension lines point to; VERIFY per §5.)
- **2× 1.80 mm case-tab slots** near the USB-C end (photo: ~X +15.00, Y −16.50 region). Not needed
  on the backplane; just keep the perimeter clear there.
- **USB-C**: right edge of the board, on the horizontal centerline (Y ≈ 0). The plug extends
  radially outward in the board plane → **the backplane disc needs an edge notch/cutout at the
  USB-C corner** (~10 mm wide × ~5 mm deep at X≈+23, Y≈0) or the plug body collides with the
  backplane edge. Extend the notch to keep the PWR/BOOT edge buttons and the user's mount
  button cutouts usable. VERIFY with a real cable.
- Stack clearance: standard 1×8 female body ≈ 8.5 mm; male header tail engages it. **Top-face
  keepout on the backplane: nothing taller than ~4 mm anywhere within Ø44** (Waveshare bottom
  has TF slot, MX1.25 connectors, ESP32-S3 shield ~1.3 mm, flash ~1.5 mm). Buck/inductor/TVS/FET/
  diodes: SO-8/SMA/SOT-23 are ≤2 mm — fine on top. Coin holder, JST connectors, battery, tall caps:
  **bottom face only**.
- Backplane top-view coordinate transform (backplane's own PCB coords, viewed from its TOP):
  X_bp = −X_front, Y_bp = +Y_front (both boards viewed from the mating side). Header row:
  **Y_bp = −13.75, centerline X_bp = +12.70, pin 1 at X_bp = 12.70 − 3.5×2.54 = +3.81, pin 8 at +21.59.**
  Mount holes (symmetric set, unchanged): (±18.30, +12.80), (0, −20.50).

</details>

**Corrected geometry (from `WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md`):**

- Outline: Ø46.00 mm circle, center (0,0). Rear-view coordinate frame (+X = USB-C side,
  +Y = speaker/battery side, −Y = H2 side).
- Mounts: **(0, +20.50), (−13.75, −14.70), (+13.75, −14.70)** — Ø2.2 mm NPTH.
- H2: 1×8 @ 2.54 mm, row centerline **Y = −18.68**, X-center **0**. Pins at X = ±1.27, ±3.81,
  ±6.35, ±8.89. Rear-view pin order (left→right): GPIO16, GPIO18, GPIO17, U0TXD, U0RXD, 3V3,
  GND, VBUS. Pin 1 (VBUS) at X=+8.89 (USB-C side).
- USB-C: **+X perimeter**. Do NOT cut a notch into the backplane outline (spec §11 says keep
  perimeter open; PLAN's notch idea discarded). Instead: no tall parts near +X edge; the
  backplane is clamped behind by screws, USB plug routes around/below the disc edge.
- 2×1.80 features: ignore per spec §13.
- Inter-board height is undefined by the mating spec. It must be established from
  the tallest Waveshare rear part, the selected H2 stack, screws/standoffs, solder
  protrusion, and daughterboard parts. Electronics are on TOP/inside where feasible;
  J1–J5 remain BOTTOM/outward. BT1 on TOP is conditional on the measured gap.

## 5. Geometry verification procedure (do BEFORE ordering, user-assisted)

**2026-08-29 UPDATE: the uploaded `WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md`
supersedes this section's old numbers.** That spec is a verified extraction from
the official dimension drawing and gives exact rear-view coordinates for the
three mounts and the H2 header (see corrected geometry in §4). The remaining
pre-order check is the 1:1 print overlay + caliper verification per that spec's
§15 (circle Ø46.00, mount spacing 27.50, H2 row Y=−18.68, pitch 2.54, pin-1=VBUS).

1. Export the backplane mechanical layer at 100% scale (1:1).
2. Verify with calipers: circle Ø46.00, bottom mount spacing 27.50, top mount
   Y +20.50, bottom mounts Y −14.70, H2 pitch 2.54, H2 row Y −18.68.
3. Place the actual Waveshare PCB over the print; verify the three mount
   centers and all eight H2 pin centers.
4. Verify pin-1/VBUS orientation face-to-face (not from separate top views).
5. Only then order PCBs. Record results in DESIGN_REVIEW.md.

## 6. Schematic / netlist plan

Rail names (spec §Phase 2): `CAR_12V_RAW`, `CAR_12V_PROTECTED`, `BUCK_5V_RAW`, `VBUS_SHARED`,
`SENSOR_5V`, `3V3` (= H2 3V3 input, name `3V3_WAV`), `I2C_SDA_3V3`, `I2C_SCL_3V3`,
`I2C_SDA_5V`, `I2C_SCL_5V`, `GND`.

### Blocks (use `<schematicsection />` per block)

**A. Automotive input / protection**
- J1 2P JST-PH: `12V_IN`, `GND` (bottom face, perimeter, away from BMP280).
- F1 input fuse 2 A — **RESOLVED: `0466002.NRHF` C3105 (2 A/63 V SMD 1206, stock 94,916)**.
- D1 TVS **SMBJ16CA** (C353385, bidirectional) at CAR_12V_RAW→GND; normal operation is for a
  12 V automotive system, not continuous 24 V input.
- Q1 reverse-polarity high-side P-MOS **DMP4065S-7** (C182476, 40 V): drain=CAR_12V_RAW,
  source=CAR_12V_PROTECTED, gate→GND via 100 kΩ, 10 V zener gate-to-source.
- Input bulk: 2× 22 µF/50 V X7R 1210 + 100 nF at buck VIN. (Ceramic only — no electrolyte in a car.)

**B. Buck 12 V → 5 V**
- U1 **LMR36520ADDAR** (C2879422, 4.2–65 V in, 2 A out, 400 kHz, SOP-8/PowerPAD). Alt:
  MP9943GQ-Z (C477821, 36 V/3 A QFN) — keep LMR36520 as primary (65 V headroom = real load-dump
  margin; MP9943's 36 V needs the TVS to do all the clamping).
- L2 10 µH shielded (FXL0530-100-M C177248, 5.4×5.2×3 mm) on TOP.
- FB divider → 5.016 V. The synchronous converter uses a 100 nF BOOT-to-SW capacitor;
  no external catch or boot diode is fitted.
- Output: 2× 22 µF 0805/1210 X5R + 100 nF.
- **Layout: buck cluster in one sector; ≥8 mm from BMP280, ADS1115, RTC, sensor connectors.**

**C. Power path / VBUS**
- D2 **SS54** (C22452) anode=BUCK_5V_RAW → cathode=VBUS_SHARED → H2 pin 1.
- SENSOR_5V branch: VBUS_SHARED → F2 (nSMD050-24V C70076, 500 mA hold / 1 A trip) → SENSOR_5V.
  Powers: ADS1115 VDD, TCA9406 VHB, 4× sensor connector
  pin 1. A shorted engine-bay harness then browns out only the sensor rail, not the display.
- USB case: USB → Waveshare AXP2101 VBUS = H2 pin 1 = VBUS_SHARED; D2 blocks backfeed into the
  buck. Sensor rail is bench-powered from USB through D2's... NO — D2 is one-way INTO VBUS_SHARED,
  so USB also feeds SENSOR_5V via F2 (reverse-powered from H2). That's desired (spec §4: bench
  power OK) and safe (F2 limits). Document in README.

**D. I²C translation**
- U2 **TCA9406DCTR** (C337496, MSOP-8, 2-bit, auto-direction, Hi-Z when either rail is 0 V —
  satisfies the "sensor rail off, 3V3 alive" requirement). VCCA=3V3_WAV, VCCB=SENSOR_5V.
- Pull-ups 4.7 k on all four nets (SDA/SCL × 3V3/5V sides). 3V3-side pull-ups to 3V3_WAV only.
- A-side: H2 GPIO17/18. B-side: ADS1115 only.

**E. ADC**
- U3 **ADS1115IDGSR** (C37593, VSSOP-10). VDD=SENSOR_5V, ADDR→GND, decoupling 100 nF+1 µF at pins.
- A0–A3: each channel: connector pin 3 → ESD clamp BAV99 (**C916421**; alt 2× 1N4148WS C2128)
  to SENSOR_5V/GND → series 1 kΩ → 100 nF to GND at the ADC pin. RC fc ≈ 1.6 kHz (≫ 62.5 Hz
  gauge rate, fine for boost transients; document). 12 V-on-signal-wire fault: clamp + 1 kΩ limit
  current; verify ADC pin abs-max with the diode clamp math in DESIGN_REVIEW.
- No divider — 0–5 V scaling preserved.

**F. Ambient**
- U4 **BMP280** (C83291, LGA-8). 3V3_WAV, SDO→GND (0x76), CSB→3V3 (I²C mode). **Top
  (Waveshare-facing) face, perimeter edge — user decision #7** (AC vent protection), opposite
  sector from the buck, vent unobstructed, no copper/noise under it, silkscreen
  "DO NOT COVER / NO CONFORMAL COAT" ring + keepout note.

**G. RTC**
- U5 **DS3231MZ+** (C107410, SOIC-8, 2.3–5.5 V, 0x68). VCC=3V3_WAV, VBAT=coin cell + (direct,
  no charger, no series diode — MZ datasheet allows). BT/OSF handled by firmware.
- BT1 CR1220 holder (BS-12-B2AA002 C964721) — **user hand-solders (decision #8)**,
  top/mating face only after measured clearance; **+** pad silkscreened; 100 nF at VCC.
- Note in DESIGN_REVIEW: DS3231M vs old SN is register-compatible at 0x68 (firmware uses
  STATUS/OSF + time regs only — verified against `boost_sensors.c` DS3231 paths this session).

**H. Test points** (top face where reachable, else bottom): TP for CAR_12V_RAW,
CAR_12V_PROTECTED, BUCK_5V_RAW, VBUS_SHARED, SENSOR_5V, 3V3_WAV, GND, I2C_SDA_3V3,
I2C_SCL_3V3, I2C_SDA_5V, I2C_SCL_5V, A0–A3 (post-filter), GPIO16, U0RX, U0TX.

## 7. Part sourcing summary (all LCSC-verified in stock this session)

| Ref | MPN | LCSC | Role | Alt |
|---|---|---|---|---|
| U1 | LMR36520ADDAR | C2879422 | buck 65 V/2 A | MP9943GQ-Z C477821 |
| U2 | TCA9406DCTR | C337496 | I²C translator | PCA9306DCUR C33196 (verify unpowered behavior) |
| U3 | ADS1115IDGSR | C37593 | ADC | ADS1115BQDGSRQ1 C2868291 (auto grade) |
| U4 | BMP280 | C83291 | ambient | none (firmware chip-id lock) |
| U5 | DS3231MZ+TRL | C107410 | RTC | DS3231SN C9866 (SO-16, bigger) |
| Q1 | DMP4065S-7 | C182476 | 40 V reverse polarity | — |
| D1 | SMBJ16CA | C353385 | bidirectional input TVS | — |
| D2 | SS54 | C22452 | VBUS ORing | SS34 C8678 |
| L2 | FXL0530-100-M | C177248 | 10 µH buck inductor | — |
| F2 | nSMD050-24V | C70076 | 500 mA hold / 1 A trip sensor PTC | — |
| J2–J5 | B3B-PH-K-S(LF)(SN) | C131339 | sensor connectors | — |
| J1 | B2B-PH-K-S(LF)(SN) | C131337 | 12 V input | — |
| BT1 | BS-12-B2AA002 | C964721 | CR1220 holder | C5239868 gold |
| passives | 0603/1210, 4.7k/1k/100k, 100nF/1µF/22µF-X5R-50V | various | — | — |

**Open sourcing items — RESOLVED 2026-08-29 (all LCSC-verified in stock):**
1. **F1 input fuse**: `0466002.NRHF` **C3105** — 2 A / 63 V SMD fuse 1206, stock 94,916. No THT fallback needed.
2. **F2 sensor-branch PTC**: `nSMD050-24V` **C70076** — 500 mA hold / 1 A trip.
3. **Gate zener**: `LBZX84C10LT1G` **C12772** — 10 V SOT-23.
4. **Analog clamp**: `BAT54S,235` **C503463** — Schottky series pair.
5. **H2 mating header**: 1×8 2.54 mm THT — hand-solder (`doNotPlace`); gender and fitted
   stack height must be confirmed on the physical Waveshare before ordering.
6. **Test points**: `<testpoint />` pads (no BOM part).

**Assembly mode (user decision #8): JLCPCB partial assembly.**
JLC installs: all ICs, buck circuitry, MOSFET, translator, TVS, fuses, diodes,
inductor, resistors, capacitors. User hand-solders (doNotPlace): H2 1×8 header,
J1–J5 JST-PH connectors, BT1 CR1220 holder.

## 8. tscircuit implementation steps

1. `tsci init -y` in `hardware/boost-gauge-backplane/`.
2. Build the form factor: circular Ø46 outline, 3× Ø2.2 `<hole />` at the corrected
   coordinates, H2 at the corrected top-view coordinates, and a left-edge USB keepout.
   The authoritative outline has no USB notch; verify the Gerber and a real plug.
3. Schematic: one file per section or `<schematicsection />` groups; import parts with
   `tsci import "C..."` (JLCPCB footprints come with LCSC mapping) — do NOT hand-invent
   footprints; registry has none for these (checked: `tsci search "waveshare"` → nothing usable).
4. `tsci check netlist` → fix → `tsci check schematic-placement` → `tsci check placement` →
   `tsci build --pcb-png dist/placement.png` → manual inspect both faces →
   `tsci check routing-difficulty` → `tsci build` (autoroute) → `tsci check shorts` (must pass) →
   manual Gerber-outline/holes inspection (`tsci export` / `--help` for gerber flags).
5. Power nets: `<net>`-based routing intent + wide traces (≥0.5 mm on 5 V, ≥0.8 mm on 12 V),
   GND pour both layers and thermal vias under the LMR36520 PowerPAD.
6. If the autorouter can't close, hand-route the buck hot loop with `<trace />` + `<via />`
   first, then re-run.

## 9. Deliverables (spec §21) — all in `hardware/boost-gauge-backplane/`

source `.tsx` files, form-factor component, `dist/` renders (top/bottom/schematic/3D),
Gerbers + drill + BOM + CPL (or exact `tsci export` commands that produce them),
`README.md` (architecture, H2 pinout incl. mirror explanation, power path + USB caveat,
connector pinout table, I²C addresses 0x48/0x76/0x68, RTC battery, assembly notes,
bring-up procedure from spec §20), `DESIGN_REVIEW.md` (assumptions, load-dump math, buck
thermals at 46 mm, Vgs/zener check, RC filter fc, sensor-rail short behavior, CR1220 temp
risk, unresolved fuse sourcing, geometry-verification results), firmware note:
**no firmware change required; only set MAP-supply NVS to measured SENSOR_5V (~4.6–4.7 V
car / ~4.9 V USB-bench).**

## 10. Acceptance gates (do not claim done without)

- [ ] §5 geometry verification performed on the physical board, numbers recorded.
- [ ] `tsci check netlist`, `check placement`, `check schematic-placement`, `build`, `check shorts` all clean.
- [ ] Gerber edge = Ø46.00; 3 holes match mount; header not mirrored (pin-1 square pad lands on VBUS face-to-face).
- [ ] Measure the complete stack. Verify every TOP/inside component and the loaded BT1 against
      the real inter-board gap; keep J1–J5 BOTTOM/outward and verify cable access against the mount.
- [ ] Every BOM line has a real LCSC number (or documented hand-solder exception).
- [ ] BMP280 on the Waveshare-facing face at perimeter (user decision #7), ≥8 mm from buck, vent keepout + silkscreen warning.
- [ ] GPIO17/18 pull-ups on 3V3 side only; ADS1115 5 V side only; translator = TCA9406.
- [ ] README + DESIGN_REVIEW written; commit under `hardware/` only.

## 11. Historical intermission state (2026-08-29) — superseded by §12

This section records the state that was inherited by the final audit. Its part
choices and “not yet done” list are not current instructions.

**Done this session:**
- `tsci init -y` complete (tscircuit 0.0.2461, bun present).
- All 15 part imports in `imports/` (LCSC footprints verified 98.8–100% IoU):
  LMR36520ADDAR, TCA9406DCTR, ADS1115IDGSR, BMP280, DS3231MZ_TRL, AO3401A,
  SMBJ16A, SS54, FXL0530_4R7_M, B3B/B2B-PH-K-S, BS_12_B2AA002, BZX84_C12_215,
  BAV99 (C916421), BAT54S (C7420333, spare), A_0466002_NRHF (fuse).
- `index.circuit.tsx`: full netlist, blocks A–H, `tsci check netlist` = 0 errors /
  0 warnings. Only genuine datasheet no-connects float (BZX84 NC, ADS1115 pin2,
  DS3231MZ 32kHz/N_RST stub).
- Sourcing resolved (see §7): F1 = 0466002.NRHF C3105 (SMD fuse, no THT fallback
  needed); F2 = BSMD1206-200-12V C883135; zener = BZX84-C12,215 C108437;
  clamp = BAV99 C916421.
- Geometry superseded by `reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md`
  (user-confirmed rear view: VBUS rightmost, GPIO18 leftmost, USB-C right).
- PDF vector extraction tooling built (`/tmp/pdfgeom.c`); dimension drawing
  confirmed Ø46 outline, no mounting holes drawn in the top view (they live in
  the spec's numbers), side-view extracted for stack-height analysis.
- User decisions #7 (BMP280 top face) and #8 (JLC partial assembly) recorded in §1.

**Not yet done (next session):**
1. Schematic fixes: D2 currently wired as boot diode — must be the VBUS ORing
   (A=BUCK_5V_RAW → K=VBUS_SHARED); add a proper CBOOT (100 nF BOOT→SW); FB
   divider values must give 5.00 V exactly per LMR36520 equation (R3/R4 = 52.3k
   was a placeholder; recompute: Vout = 0.6×(1+R3/R4) with LMR36520's FB ref —
   verify against datasheet and adjust R3/R4).
2. `<fuse name="F2" />` has no supplier pinned — replace with a `chip` on the
   C883135 footprint (BSMD1206-200-12V) or `supplierPartNumbers`.
3. Form-factor component `WaveshareBackplane46.circuit.tsx` per mating spec §14:
   Ø46 circle outline (polygon, 72 segments), 3× Ø2.2 NPTH `<hole />` at
   (0,+20.50)/(±13.75,−14.70), H2 `<pinheader pinCount={8} pitch="2.54mm">`
   centered (0,−18.68), USB-C keepout marker on +X, center datum marker.
4. Merge form factor into `index.circuit.tsx`; place per §4 sector rules
   (buck SW/inductor cluster ≥8 mm from BMP280/ADS/RTC/connectors; BMP280 top
   face perimeter per decision #7; tall parts bottom face per decision #8;
   J1/J2–J5/BT1/H2 `doNotPlace` for JLC, hand-solder).
5. Checks → placement → routing (§8 steps 4–6) → shorts → renders → Gerbers.
6. README.md + DESIGN_REVIEW.md (load-dump math, Vgs/zener check, RC fc,
   sensor-rail short behavior, CR1220 temp risk, geometry-verification results,
   assembly-mode BOM/CPL split).

**Key file state:** `index.circuit.tsx` is the working schematic (netlist
clean); `imports/` has all 15 parts; `reference/` has the mating spec + PDFs.

**2026-08-29 intermission addendum (post-geometry-resolution):**
- Mating spec copied to `reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md`.
- User physically confirmed the spec's rear-view frame (rear view: VBUS
  rightmost, GPIO18 leftmost, USB-C right side). PLAN §3's old mirror derivation
  is superseded; §5 rewritten to the spec's §15 caliper/overlay procedure.
- Daughterboard transform (decision #10): X_db = −X_assembly, Y unchanged.
  In tscircuit top-view coords: H2 pin 1 (VBUS) at (−8.89, −18.68) … pin 8
  (GPIO16) at (+8.89, −18.68); USB-C keepout on the LEFT edge (X≈−23, Y≈0).
- BOM/CPL split for JLC partial assembly (decision #8): source marks J1–J5,
  H2, and BT1 `doNotPlace`, but tscircuit still emits them in raw BOM/CPL. The
  release filter removes those seven refs plus all PCB-only `TP_*` pads.
- F2 was subsequently resolved as nSMD050-24V C70076, 500 mA hold / 1 A trip.

## 12. Repair and audit state (2026-08-30)

The final source uses the corrected input topology, bidirectional SMBJ16CA,
40 V DMP4065S-7 P-MOS, 10 µH inductor, two 22 µF output capacitors, 500 mA-hold
sensor PTC, exact supplier copper for U2/U3/U5, close RTC/BMP/ADC/buck bypassing,
named-width power/I²C nets, dual GND pours, and explicit critical routes. H2 is
a real `<pinheader>`. TOP is the inside/mating face; only J1–J5 are BOTTOM.
BT1 is the horizontal SMT CR1220 holder on TOP and is conditional on measured
stack clearance.

Final verified `dist/index/circuit.json`:

- 128 routed connections, 142 PCB traces, 106 vias, 0 router errors.
- 0 circuit error records, 0 long-trace warnings, 0 unrouted records.
- `tsci check netlist`: 0 errors / 0 warnings.
- `tsci check placement`: 0 errors; three horizontal-access advisories on
  vertical/top-entry bottom JST-PH connectors remain a physical cable-access gate.
- `tsci check source`: 0 errors / 0 warnings.
- `tsci check shorts --mode pcb` is **toolchain-blocked**: tscircuit 0.0.2461
  throws `layerCanvas.getContext is not a function`; it failed identically twice.
- Schematic autolayout reports cosmetic overlaps/label routing. The electrical
  netlist is clean, but the generated schematic is an audit aid rather than a
  publication-quality drawing.
- One U1 supplier-footprint IoU warning remains (0.7255). Fresh exact import
  copper matches the local wrapper, but it still requires a manual TI/JLC land-
  pattern check rather than suppressing the warning.

Staged manufacturing outputs are exported from the verified circuit JSON so no
second autoroute is introduced. Raw BOM/CPL contain 73 designators; filtered JLC
files contain 48 placed parts and exclude exactly the seven hand-solder refs plus
17 `TP_*` PCB-only pads. All 48 filtered BOM rows have supplier IDs and the
filtered BOM/CPL designator sets match.

The STEP exporter emits a board model but its parser rejects several downloaded
supplier models. DNP bodies—including BT1, H2, and J1–J5—also do not establish
the assembled envelope. STEP therefore cannot prove stack or mount clearance.

**Remaining physical/pre-order gates:**

- [ ] 1:1 face-to-face overlay: outline, mounts, H2 pitch, pin 1 → VBUS.
- [ ] H2-8 continuity, fitted gender, selected header stack height, solder
      protrusion, screws/standoffs, and actual inter-board separation.
- [ ] Loaded BT1 clearance. Treat 4.10 mm as the likely installed-holder envelope
      and 6.10 mm only as a conservative additive bound until a physical section
      confirms how the 2.0 mm cell nests.
- [ ] Bottom J1–J5 connector bodies and cable exits against the flush rear mount;
      USB-C plug/cable access; BMP280 vent/no-coat clearance.
- [ ] Manual U1 land-pattern comparison and independent PCB-short DRC.
- [ ] Bench bring-up over the intended 12 V automotive operating range: polarity,
      transients, rail sequencing, sensor short, output ripple/load, and thermal.
