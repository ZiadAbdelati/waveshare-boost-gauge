# Boost Gauge Backplane — Design Review

Audit date: 2026-08-30. This review covers the tscircuit source, the uploaded
Waveshare mating specification, the final routed circuit JSON, and staged
manufacturing outputs. It does not replace physical fit or powered bench tests.

Face convention used throughout:

- **TOP** = Waveshare-facing, mating, inside face.
- **BOTTOM** = enclosure/mount-facing, outward, back face.

## 1. Mechanical authority and placement

`reference/WAVESHARE_1_75_MECHANICAL_MATING_SPEC.md` supersedes the original
PLAN geometry. The implemented board is a 72-segment Ø46.00 mm circle with
three Ø2.2 mm NPTH holes at `(0,+20.50)` and `(±13.75,-14.70)`. H2 is centered
at `(0,-18.68)`, with eight pins on 2.54 mm pitch. Under the face-to-face mirror,
backplane H2 pin 1/VBUS is at x = -8.89 mm. The USB-C access keepout is on the
backplane's left edge; there is no fabricated outline notch.

Electronics are on TOP where feasible. J1–J5 are the only BOTTOM components,
keeping the flush mount face free of electronics while leaving the cable
connectors outward. Their tscircuit orientation warnings are a horizontal-access
heuristic applied to vertical/top-entry JST-PH parts; real connector bodies and
cables still require a 1:1 mount check.

Explicit keepouts cover the USB-C envelope, all three mount holes, and the
BMP280 vent. The sensor also has a `DO NOT COVER / NO COAT` fabrication note.

## 2. BT1 identity and stack-height decision

BT1 is not a cable header. It is MYOUNG `BS-12-B2AA002` / LCSC `C964721`, a
horizontal SMT CR1220 holder. It is `doNotPlace`, on TOP at `(13.2,-0.5)`, and
must be hand-soldered without the cell installed.

The supplier-family drawing gives a 4.10 mm mechanical holder height. A
Panasonic CR1220 is at most 2.0 mm thick and normally nests in the holder. Use
these as unresolved bounds:

- **4.10 mm likely installed envelope** if the drawing's height includes the
  loaded/nesting envelope.
- **6.10 mm conservative additive bound** only until the supplier or a physical
  section confirms the loaded datum.

The mating specification intentionally leaves daughterboard separation
undefined. It must include the tallest Waveshare rear part, selected H2 stack,
screws/standoffs, solder protrusion, and daughterboard parts. BT1 may remain on
TOP only after that real assembled gap is measured. The generated STEP is not
evidence of fit: DNP bodies are not a complete assembly envelope and tscircuit's
STEP parser rejected several downloaded supplier models.

References: [LCSC holder page](https://www.lcsc.com/product-detail/Button-And-Strip-Battery-Connector_MYOUNG-BS-12-B2AA002_C964721.html),
[supplier-family drawing](https://atta.szlcsc.com/upload/public/pdf/source/20250915/C0E8C58C172AE3B5B92BC8CEB0A088C7.pdf),
[Panasonic CR1220 datasheet](https://energy.panasonic.com/dam/master/pdf/en/datasheet/lithium/CR1220_Datasheet_EN.pdf).

## 3. Electrical audit and repairs

### Vehicle input and buck

- Corrected the series path to `J1 → F1 → CAR_12V_RAW → Q1 →
  CAR_12V_PROTECTED`. D1 is connected from fused RAW to GND.
- D1 is now bidirectional `SMBJ16CA` (`C353385`), avoiding a unidirectional TVS
  forward crowbar during reverse battery. This remains a 12 V automotive design;
  it is not approved for continuous 24 V input.
- Q1 is now the 40 V `DMP4065S-7` (`C182476`) with drain on RAW, source on
  PROTECTED, a 100 kΩ gate pull-down, and a correctly polarized 10 V VGS zener.
- LMR36520 feedback is 100 kΩ / 24.9 kΩ for 5.016 V nominal. BOOT uses only a
  100 nF BOOT-to-SW capacitor; the converter is synchronous.
- L2 is the verified 10 µH `FXL0530-100-M` (`C177248`). Output capacitance is
  two 22 µF capacitors plus 100 nF. The switch node, input HF loop, output caps,
  feedback sense/ground, and exposed-pad grounding have explicit short routes.

These changes improve the paper design, but transient energy, TVS clamp voltage
at real current, MOSFET margin, output ripple, and thermals still require bench
measurement with automotive-source impedance. F1 is fault protection, not a
substitute for validating load-dump behavior.

### VBUS and sensor branch

- D2 (`SS54`) ORs buck output into `VBUS_SHARED` and blocks USB VBUS from
  feeding back into the unpowered buck.
- F2 is `nSMD050-24V` (`C70076`), 500 mA hold / 1 A trip, so the sensor branch
  has meaningful selectivity relative to the 2 A buck. A real harness-short test
  must confirm display behavior and trip/recovery time.

### ADC and I2C

- U2, U3, and U5 now use exact supplier copper land patterns. Their logical pin
  labels were checked against the device pin tables.
- ADS1115 has close 100 nF and 1 µF bypass paths. Each AIN channel is connector
  signal → 1 kΩ → filtered ADC node, with 100 nF to GND and a BAT54S rail clamp.
  The nominal RC corner is about 1.59 kHz.
- The TCA9406 uses 4.7 kΩ pull-ups on both domains and has local VCCA/VCCB
  bypassing. I2C nets carry an explicit 0.2 mm nominal width.
- BMP280 power and bus approaches were rerouted clear of the prior accidental
  contact; its local 100 nF bypass has a 3 mm maximum route.
- DS3231 VCC bypass was moved next to U5 and constrained to 3 mm. VBAT is direct
  to the non-rechargeable CR1220 holder; there is no charging path.

## 4. Supplier footprint review

Fresh exact-import copper for U2, U3, and U5 matches the local wrappers and their
old mismatch warnings are gone. U1 still produces one supplier-footprint IoU
warning (`0.7255`) even though its local copper matches a fresh exact import.
Do not suppress or reinterpret it: compare the exported U1 pads manually against
the TI package land pattern and JLC/EasyEDA part before fabrication.

## 5. Automated verification

The final source was built with tscircuit 0.0.2461 and TypeScript 5.9.3. The
authoritative routed snapshot is `dist/index/circuit.json`.

| Gate | Observed result |
|---|---|
| `npm run typecheck` | pass |
| `tsci check source` | 0 errors / 0 warnings |
| `tsci check netlist` | 0 errors / 0 warnings |
| `tsci check placement` | 0 errors; 3 JST access-direction advisories |
| routing-difficulty | pass; highest local estimate 12% |
| final autoroute | 128 routed connections; 0 jumpers; 0 router errors |
| final JSON | 142 PCB traces; 106 vias; 0 errors; 0 long traces; 0 unrouted |
| critical trace probes | U1 SW→C18 1.88 mm; U1 SW→L2 3.77 mm; U5 VCC→C16 2.08 mm; C7→C5 3.08 mm |
| supplier checker | U1 warning only, IoU 0.7255 |
| bitmap PCB shorts | **blocked by tscircuit CLI defect; failed identically twice** |
| repository host suite | 11/11 passed (loopback-enabled run) |
| repository live-device hardware suite | unavailable: configured board `192.168.50.102` timed out / host down |

`tsci check shorts --mode pcb dist/index/circuit.json` throws
`layerCanvas.getContext is not a function`, including a retry at reduced pixel
density. Therefore “no shorts” is not claimed from that raster checker. The
zero-error circuit JSON and completed router are positive evidence, but an
independent Gerber/PCB DRC remains mandatory.

Schematic autolayout is electrically clean but reports cosmetic overlaps and
long/awkward label routes. The exported schematic is an audit aid, not a
publication-quality service drawing.

The repository `tools/check_hardware_gates.py` suite targets a running firmware
device and is not a static PCB test. Its network-enabled retry could not reach
the configured board, so none of its live boot/display/BLE/media gates are
credited to this hardware revision.

## 6. Manufacturing-output audit

Manufacturing files were staged by exporting directly from the verified circuit
JSON, preventing a second independent autoroute. The Gerber ZIP passes `unzip
-t` and contains 13 raw entries: nine Gerbers, two drill files, raw BOM, and raw
pick-and-place.

Raw BOM/CPL contain 73 designators. The JLC files contain 48 placed components
and exclude exactly:

- hand-solder/DNP: `BT1,H2,J1,J2,J3,J4,J5`;
- PCB-only pads: all 17 `TP_*` designators.

Every filtered BOM row has a supplier ID, and filtered BOM/CPL designator sets
are identical. The fresh exporter produces 19 promoted artifacts plus
`SHA256SUMS`; the historical `F_Fab.gbr` is not regenerated and must not be
carried forward from an older release.

## 7. Pre-order and bring-up gates

The board is a release candidate, not fabrication-approved, until all boxes are
recorded with measurements:

- [ ] Print/export at 1:1 and overlay face-to-face: Ø46 outline, all mounts, H2
      centers, and pin 1/VBUS orientation.
- [ ] Continuity-check Waveshare H2-8 and confirm GPIO16/GPIO18 labeling.
- [ ] Confirm fitted H2 gender, body height, engagement, pin-tail length, and
      solder protrusion before selecting the daughterboard header.
- [ ] Measure assembled board separation and Waveshare rear-part envelope;
      verify loaded BT1 against the conservative height bounds.
- [ ] Check bottom J1–J5 bodies and cable exits against the flush rear mount,
      plus real USB-C plug/cable access.
- [ ] Verify BMP280 vent/no-coat clearance and airflow exposure.
- [ ] Compare U1 Gerber pads to TI and supplier land patterns; run an independent
      PCB/Gerber shorts and clearance DRC.
- [ ] Power first from a current-limited bench supply. Verify polarity behavior,
      5.016 V nominal buck output, VBUS ORing, rail sequencing, sensor-short
      isolation, ADC clamps, I2C devices, ripple/load regulation, and thermal rise.
- [ ] Only after bench validation, exercise the intended 12 V automotive range
      and representative transients; set firmware MAP supply to measured
      `SENSOR_5V`.
