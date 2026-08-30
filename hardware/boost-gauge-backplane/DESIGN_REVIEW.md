# Boost Gauge Backplane — Design Review (working draft)

Living document. Updated as implementation progresses. Final version ships with
the deliverables (Gerbers, renders, README).

## 1. Geometry resolution (2026-08-29)

**Finding:** PLAN.md §4's original geometry was internally inconsistent — its
H2 header row (pin 8 at X=+21.59, Y=−13.75) would sit at radius ≈25.6 mm from
board center, **outside the Ø46.00 mm board**. The numbers could not have come
from a valid reading of the drawing.

**Evidence (independent verification):**
- Extracted all 15,878 vector line segments from the official dimension PDF
  (`reference/waveshare-esp32-s3-touch-amoled-1.75-dimensions.pdf`) via a
  CoreGraphics content-stream parser (no text objects exist in the PDF — all
  labels are vector outlines).
- Chain/loop analysis of the segments found the board outline at view center
  (376, 349) pt: a pure circle of **Ø46.01 mm** (scale 4.84 pt/mm derived from
  r=111.34 pt ↔ 23 mm), plus concentric circles at Ø45.08 / Ø41.51 / Ø41.12.
- No USB-C notch exists in the outline; no mounting-hole circles appear in the
  top view (hole positions come from the dimension chains, captured in the
  mating spec).

**Resolution:** `reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md` is now the
mechanical authority (PLAN decision #9):
- Mounts: (0, +20.50), (±13.75, −14.70), Ø2.2 NPTH.
- H2 row: centerline Y=−18.68, X-center 0, pins at ±1.27/±3.81/±6.35/±8.89.
- User's physical observation (rear view: VBUS rightmost pin, GPIO18 leftmost
  labeled pin, USB-C on the right) confirms the spec's rear-view frame and
  contradicts PLAN §3's old front-view mirror derivation.

**Daughterboard transform (decision #10):** the daughterboard's mating face is
its tscircuit TOP face. Because the mating faces oppose each other, the
daughterboard's top view is X-mirrored relative to the Waveshare rear view:

- `X_db = −X_assembly_rear`, `Y_db = +Y_assembly_rear`
- H2 row in backplane coords: **VBUS (−8.89, −18.68), GND (−6.35), 3V3 (−3.81),
  RX (−1.27), TX (+1.27), SDA/GPIO17 (+3.81), SCL/GPIO18 (+6.35), GPIO16 (+8.89)**
- USB-C access keepout: **left edge, X ≈ −23, Y ≈ 0** in backplane coords.
- Residual pin-level check before fab: user read the leftmost labeled pin as
  "IO18" while the schematic places unlabeled GPIO16 leftmost (GPIO18 second).
  Verify with a continuity test (H2-8 → GPIO16 vs GPIO18) before ordering;
  a one-pin shift would move SDA/SCL one position.

## 2. Sourcing resolutions (2026-08-29)

| Item | Part | LCSC | Stock | Note |
|---|---|---|---|---|
| F1 input fuse | 0466002.NRHF | C3105 | 94,916 | 2 A/63 V 1206 SMD — THT holder fallback not needed |
| F2 sensor PTC | BSMD1206-200-12V | C883135 | 78,598 | 2 A hold / 3.5 A trip; 500 mA-hold PTCs have no credible stock |
| Gate zener | BZX84-C12,215 | C108437 | 206,182 | 12 V SOT-23 — **replaced 2026-08-30: see §4.7** |
| Analog clamp | BAV99 | C916421 | 2,071,820 | SOT-23 series pair |
| 12 V input | B2B-PH-K-S(LF)(SN) | C131337 | 298,406 | JST-PH 2P THT |
| Sensor conn. | B3B-PH-K-S(LF)(SN) | C131339 | 214,823 | JST-PH 3P THT ×4 |
| Coin holder | BS-12-B2AA002 | C964721 | 4,301 | SMD pads, hand-solder |

## 3. Assembly mode (user decision #8)

- **JLC installs (top face, single-side assembly = cheapest):** U1 buck, U2
  translator, U3 ADC, U4 BMP280, U5 RTC, Q1, D1–D8, L2, F1, F2, all R/C.
- **User hand-solders (doNotPlace):** H2 1×8 male header, J1–J5 JST-PH THT,
  BT1 CR1220 holder. Cell inserted at assembly time — never reflow a battery.
- Height audit for the inside (top) face: tallest part is FXL0530 inductor at
  3.0 mm < 4 mm keepout ✓. All other parts ≤2.3 mm.

## 4. Schematic bug audit (2026-08-30, all fixed in index.circuit.tsx)

Found by re-deriving every block against primary sources (LMR36520 datasheet
PDF pulled from ti.com and text-extracted; BAV99/BZX84/AO3401A pinouts from the
imported LCSC footprints). Each fix below cites its evidence.

1. **FB divider would have made 6.23 V** (severity: critical). The old
   52.3k/10k values assume a 0.8 V FB reference. The LMR36520 datasheet block
   diagram states **"1.0 V Reference"**; Vout = 1.0 × (1 + 52.3/10) = 6.23 V —
   over the ADS1115's absolute maximum (VDD + 0.3 V) and the BMP280's 3.6 V
   rail upstream of nothing (3V3_WAV comes from the Waveshare 3.3 V regulator,
   but SENSOR_5V at 6.2 V would overdrive the ADC inputs and the TCA9406 B-side
   which is rated VCCB ≤ 5.5 V). Fix: R3=100k, R4=24.9k → **5.016 V**.
2. **The SS54 "boot diode" was a phantom part** (critical). The LMR36520 is a
   **synchronous** buck: the datasheet block diagram includes an internal
   low-side FET ("LS CURRENT SENSE") and the word "diode" appears **zero**
   times in the datasheet text. A boot diode from VIN to BOOT would have been
   a real diode conducting on every cycle (BOOT is driven internally; the pin
   only needs a fly capacitor). Fix: D2 repurposed as the **VBUS ORing diode**
   (A=BUCK_5V_RAW → K=VBUS_SHARED — needed so USB VBUS cannot backfeed the
   buck's output when the car is off) and C18 (100 nF) added BOOT→SW.
3. **BAV99 clamps inverted** (critical). BAV99 is a series pair: pin1 = D1
   anode, pin2 = D2 cathode, pin3 = the series junction. The old wiring put
   pin1 on SENSOR_5V and pin2 on GND, which forward-biases D1 from the signal
   junction (pin3) down to the diode drop below SENSOR_5V... but with pin2's
   D2 cathode to GND, D2 is reverse-biased and useless; the effective clamp is
   a single diode drop *below the rail* only if current flows from signal to
   rail — the real fault is the low clamp is missing entirely and the high
   clamp is a forward diode (clamps signal at ~4.4 V whenever the sensor
   exceeds it — breaks every reading). Fix: **pin1→GND** (D1 clamps low),
   **pin3→SENSOR_5V** (D2 clamps high), **pin2→signal junction**.
4. **C11–C14 were rail decoupling, not filters** (major). All four sat on
   SENSOR_5V↔GND, duplicating C9/C10 while leaving the ADC inputs unfiltered.
   Fix: moved to the AINx nodes after R9–R12, giving each channel the intended
   RC: fc = 1/(2π·1k·100n) ≈ **1.6 kHz** (MAP signals are ≤ ~400 Hz).
5. **D3 zener forward-biased** (critical). Old wiring: anode to CAR_12V_RAW,
   cathode to gate — a forward diode from source to gate holds Vgs ≈ +0.6 V,
   so the P-FET never turns on (car power "off" at the board). Fix: reversed
   (K=source/Raw, A=gate) so it avalanches in reverse and clamps Vgs to −10 V.
6. **Gate zener 12 V = AO3401A abs max** (major). The BZX84-C12's 12 V clamp
   equals the FET's ±12 V Vgs absolute maximum with zero margin; a cold
   12.4 V load-dump would exceed it. Fix: **LBZX84C10LT1G (C12772, 10 V)** —
   gate still gets Vgs = −10 V ≥ Vth(−1 V typ) with margin, and 2 V below the
   abs max. Also verified: R1 (100k) static gate current = 12 V/100k = 120 µA;
   zener dissipation at clamp ≈ 1.2 mW ≪ 250 mW.
7. **Q1 body-diode direction** (critical, caught during placement pass). For
   reverse-battery protection the P-FET body diode must point **from input to
   output**: D (pin3) = CAR_12V_RAW, S (pin2) = CAR_12V_PROTECTED, gate pulled
   to GND. Correct operation: battery normal → body diode conducts for the
   first microseconds, Vgs = −12 V pulls the channel on (Rds(on) ≈ 30 mΩ);
   battery reversed → body diode blocks, gate pulled to GND = source, Vgs = 0,
   channel off. The old S/D assignment passed a reversed battery straight
   through the body diode.
8. **F2 had no part** — anonymous `<fuse />` replaced with the imported
   BSMD1206-200-12V (C883135).
9. **H2 was missing entirely** — no connector existed to mate the backplane to
   the Waveshare. Added `<chip pinCount={8} footprint="pinheader8">` with
   pinLabels (VBUS/GND/V3V3/RX/TX/SDA/SCL/IO16); RX/TX/IO16 are true
   no-connects (marked `doNotConnect`).

## 5. Power-path review (2026-08-30)

- **Load-dump:** SMBJ16A (16 V standoff / 25.9 V clamp, 600 W) + Q1 + buck
  rated to 36 V (absolute 42 V). A clamped load dump (25.9 V) is inside the
  buck's operating range; the SMBJ absorbs the surge energy. F1 (2 A/63 V)
  clears on sustained fault, not on transients.
- **VBUS ORing:** with the car off but USB plugged, USB VBUS (5.1 V) holds
  VBUS_SHARED; D2 (SS54, Vf ≈ 0.45 V at A) keeps the buck output from feeding
  back into an unpowered buck. Reverse leakage ≪ 1 µA. Sensor rail stays
  alive from USB — deliberate: allows bench configuration without car power.
- **Sensor-rail short:** F2 (2 A hold / 3.5 A trip) limits a dead short on
  SENSOR_5V; buck current limit (~4 A) rides through the trip. MAP sensor
  nominal load ≈ 20 mA — the 2 A hold was chosen because 500 mA-hold PTCs
  have no credible LCSC stock (see PLAN §11 addendum).
- **I2C pull-ups:** 4.7 kΩ on both sides of the TCA9406; the A-side bus is
  short (BMP280 + RTC on-board), the B-side drives the ADS1115 only. Both
  well inside the 100 kHz drive budget (bus capacitance ≈ 20 pF).

## 6. Mechanical verification from Gerbers (2026-08-30, machine-checked)

Parsed `dist/gerbers_dir/Edge_Cuts.gbr` (FSLAX46Y46, MOMM) and the drill files:

| Check | Spec | Measured |
|---|---|---|
| Board outline | Ø46.00 circle | **46.00 mm × 46.00 mm** bbox |
| NPTH mounts | Ø2.2 ×3 | Ø2.2 ×3 at (0,+20.50), (±13.75,−14.70) |
| Bottom-mount spacing | 27.50 mm | **27.50 mm** |
| H2 row centerline | y = −18.68 | **−18.68** |
| H2 pitch | 2.54 mm | **2.54 ×7** |
| H2 pin x-positions | ±1.27/±3.81/±6.35/±8.89 | exact match |
| H2 pin 1 (VBUS) | db x = −8.89 | **−8.89** (footprinter pin1 leftmost) |

## 7. Gate status (final, 2026-08-30)

| Gate | Result |
|---|---|
| `tsci check netlist` | **0 errors / 0 warnings** |
| `tsci check placement` | **no placement issues** (zero pad/courtyard/connector/footprint conflicts, no off-board) |
| `tsci check shorts` | **no shorts** |
| `tsci build` | **Circuits 1 passed**; autorouted 121 traces / 103 vias, 0 unrouted |

Autorouter note: tscircuit auto-assigns a **1 mm `max_length`** to every
capacitor in a power→ground topology (`Capacitor_getAutomaticMaxDecouplingTraceLength`,
core dist line 16523), which silently blocks autorouting with "1mm maximum
length ... cannot be satisfied". Fix: explicit `maxDecouplingTraceLength` on
every rail cap (25 mm bulk, 5 mm on the U1-VCC decoupler C17).

## 8. Known deviations / accepted risks

- **F2 is 2 A-hold, not the ideal 500 mA-hold.** Accepts a dead short by
  limiting (not clearing fast); PTC clears in seconds at 3.5 A. Documented in
  PLAN §11 addendum; revisit only if the wiring harness is upgraded.
- **Supplier-footprint IoU mismatches on 4 ICs** (U1 0.73, U2 0.65, U3 0.53,
  U5 0.67): the tscircuit footprinter models are land-pattern-accurate but
  differ in thermal-pad/pill treatment from the JLC CAD models. These are
  build warnings, not errors — copper connectivity is correct. Recommend
  ordering the PCB from the exported Gerbers, not from JLC's "match by part"
  flow.
- **`doNotPlace` parts still appear in exported bom.csv/CPL.** Generated
  `bom_jlc.csv`/`cpl_jlc.csv` (46 rows) exclude the 7 hand-solder parts for
  JLC upload. Upload the filtered files, not the raw exports.
- **CR1220 soldering:** BS-12-B2AA002 pads take the cell from the SMD holder —
  never reflow with the cell installed (PLAN decision #8).

## 9. Pending verifications (pre-order gates)

- [ ] 1:1 print overlay (spec §15): outline Ø46.00, mounts, H2 pitch, pin-1 → VBUS.
- [ ] Continuity test H2-8 → GPIO16 vs GPIO18 (resolves "IO18 leftmost"
      physical observation vs official schematic pin numbering).
- [ ] Confirm Waveshare H2 gender (male pins → daughterboard needs female
      socket) before ordering the header.
- [ ] USB-C plug clearance with a real cable (left-edge keepout is
      component-free; verify the cable arc in the enclosure).
- [ ] Buck output measured at bring-up; firmware MAP supply setting updated
      to the measured SENSOR_5V value (5.016 V nominal).
