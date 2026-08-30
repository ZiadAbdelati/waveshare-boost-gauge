# tscircuit AI Agent Specification — Waveshare Boost Gauge 46 mm Backplane PCB

## Mission

Use **tscircuit** to design a compact, manufacturable round daughterboard/backplane PCB for this project:

**Repository:** `https://github.com/ZiadAbdelati/waveshare-boost-gauge`

The new board replaces the collection of electronics currently housed separately from the display. It must mount directly behind the **Waveshare ESP32-S3-Touch-AMOLED-1.75** and electrically mate through the Waveshare board's rear **8-pin, 2.54 mm expansion header (H2)**.

The goal is a clean, self-contained gauge electronics stack that can be powered from the vehicle and connect directly to external analog automotive sensors.

Do not treat this as a generic ESP32 sensor board. Preserve compatibility with the firmware and electrical assumptions already implemented in the repository unless this specification explicitly allows a change.

---

# 1. Required research before designing

Before selecting components or placing anything, inspect and understand the following source material.

## Project repository

Read at minimum:

- `README.md`
- `docs/sensors-and-calibration.md`
- `docs/troubleshooting/i2c-sensor-bus.md`
- `main/boost_sensors.c`
- `main/boost_sensors.h`

Confirm the current firmware behavior rather than guessing.

Important existing firmware constraints include:

- Dedicated external sensor I²C bus:
  - **SDA = GPIO17**
  - **SCL = GPIO18**
  - **100 kHz**
- ADS1115:
  - address **0x48**
  - `ADDR -> GND`
  - MAP sensor on **A0**
  - powered from the sensor ~5 V rail
  - firmware uses the **±6.144 V PGA range**
  - continuous conversion at 250 SPS
- BMP280:
  - address **0x76**
  - `SDO -> GND`
  - must remain on **3.3 V logic**
  - firmware expects a real BMP280 chip ID, so do not silently substitute a BME280/BMP3xx without a firmware change
- External RTC:
  - firmware currently expects a **DS3231-compatible device at 0x68**
- MAP sensor:
  - GM 12223861 3-bar MAP
  - nominal 5 V ratiometric sensor
- Firmware MAP supply setting:
  - configurable range is **4.50–5.50 V**
  - current default is **5.20 V**
  - the final PCB's actual sensor rail voltage must be reflected in firmware/NVS calibration

The existing I²C troubleshooting documentation also establishes that **100 kHz and roughly 4.7 kΩ pull-ups are intentional and proven in the vehicle environment**. Preserve the 100 kHz design target.

## Waveshare hardware

Use the official Waveshare schematic, hardware reference, and dimension drawing for the **standard ESP32-S3-Touch-AMOLED-1.75** as the mechanical/electrical source of truth.

Important facts to verify:

### Board geometry

- Main PCB outline: **46.00 mm diameter**
- 8-pin header pitch: **2.54 mm**
- Reproduce the Waveshare mounting-hole and H2 locations from the official dimension drawing.
- Do not estimate the mounting-hole or header XY positions from a product photo.

### H2 expansion header pinout

Pin numbering follows the Waveshare schematic:

| H2 pin | Waveshare signal | Use on this daughterboard |
|---|---|---|
| 1 | VBUS | Display/shared ~5 V power |
| 2 | GND | Ground |
| 3 | 3V3 | 3.3 V logic reference / low-power peripheral rail |
| 4 | GPIO44 / U0RXD | Normally unused; optional test pad |
| 5 | GPIO43 / U0TXD | Normally unused; optional test pad |
| 6 | GPIO17 | **Sensor I²C SDA** |
| 7 | GPIO18 | **Sensor I²C SCL** |
| 8 | GPIO16 | Spare GPIO / optional test pad |

All GPIO pins are 3.3 V logic and are **not 5 V tolerant**.

The standard board also already contains a PCF85063 RTC on the Waveshare onboard I²C bus, but the existing boost-gauge firmware uses an external DS3231 on GPIO17/18. Do not remove the external RTC merely because the Waveshare board has its own RTC unless you deliberately implement and document the required firmware/power changes.

---

# 2. Overall board architecture

Create a **46.00 mm round PCB** that stacks directly behind the Waveshare board.

The board needs these functional blocks:

1. Vehicle 12 V input and protection
2. 12 V -> approximately 5 V buck conversion
3. Power injection into Waveshare H2 VBUS
4. Protected ~5 V rail for external sensors
5. ADS1115 4-channel ADC
6. 3.3 V <-> 5 V bidirectional I²C level translation
7. BMP280 ambient pressure sensor
8. Battery-backed RTC, preferably DS3231-compatible
9. External analog sensor connectors
10. Test points / bring-up access
11. Mechanical mating header and matching mounting holes

No separate MCU is required. The Waveshare ESP32-S3 remains the controller.

---

# 3. Automotive 12 V input and power protection

This board will be installed in a car. Do **not** feed raw automotive 12 V directly into a cheap low-voltage buck stage.

Design a compact automotive-tolerant input front end.

## Continuous operating target

Design around a nominal 12 V vehicle system:

- approximately 6–18 V operating input if practical
- normal charging voltage around 13.5–14.8 V
- survive realistic reverse-polarity and transient events

This is not required to be formally ISO 7637 certified, but it should be engineered like an automotive accessory rather than a bench project.

## Required input protection

Include and justify:

- input fuse or resettable PTC
- reverse-polarity protection
  - preferably MOSFET/ideal-diode style if practical
  - Schottky is acceptable only if the loss/thermal penalty is justified
- automotive TVS protection near the power connector
- input bulk capacitance
- local high-frequency decoupling
- optional ferrite/LC/pi filtering if useful
- buck converter with enough input-voltage headroom for a protected automotive rail

Prefer a regulator with:

- **at least 36 V absolute maximum**, preferably **42–60 V**
- at least **2 A output capability**
- good efficiency at the expected load
- practical thermal performance on a 46 mm PCB
- current JLCPCB/LCSC availability

Do not use a regulator that is only barely rated above normal alternator voltage.

Follow the regulator datasheet layout closely. Keep the switch node physically small and keep the switching-power area away from the BMP280, ADC inputs, RTC, and analog connectors.

---

# 4. 5 V power architecture and USB backfeed

This requirement is important.

Waveshare **H2 pin 1 is VBUS**, and the official schematic shows that it is the same USB supply domain used by the display board. A naïve hard connection between the new buck output and H2 VBUS can create reverse-current/backfeed problems when USB-C is connected to a computer for flashing.

Design and explicitly document a safe power-path strategy.

## Preferred behavior

- Vehicle power present, no USB:
  - daughterboard powers the display through H2 VBUS
  - daughterboard powers the ADS1115 and external sensor rail
- Vehicle power absent, USB-C connected:
  - USB can power the Waveshare board
  - it should not reverse-power the vehicle buck converter or 12 V input circuitry
  - ideally the sensor/ADC side may also be bench-powered from VBUS if the power architecture allows it safely
- Vehicle and USB both connected:
  - avoid unsafe source contention/backfeeding

A practical architecture may use:

- a reverse-blocking diode / Schottky / ideal-diode stage between the vehicle buck output and shared VBUS,
- deliberate source priority,
- and/or a clearly documented disconnect jumper if fully automatic power ORing is not practical in the available space.

Do **not** leave the final design as a direct, unprotected buck-output-to-USB-VBUS tie.

If the vehicle-powered H2 rail ends up slightly below 5.00 V because of the power-path device, that is acceptable as long as:

- it remains inside the Waveshare board's valid supply range,
- the GM sensor remains within its useful supply range,
- the ADS1115 remains correctly powered,
- and the actual sensor supply voltage is reflected in the boost-gauge firmware setting/calibration.

The firmware already supports a MAP sensor supply from 4.50 to 5.50 V.

---

# 5. Sensor 5 V rail

Create a clean sensor rail derived from the shared ~5 V supply.

This rail powers:

- ADS1115 VDD
- external 5 V sensor connector pins
- high-voltage side of the I²C translator

Consider a small current-limited load switch or resettable fuse for the external sensor branch so a short in an engine-bay sensor harness does not necessarily reboot the display.

If the sensor rail can be independently powered off while the Waveshare 3.3 V rail remains alive, the I²C translator must support that state without back-powering the 5 V side.

---

# 6. ADS1115

Use a bare ADS1115 IC, not a breakout module.

Required configuration:

- `VDD = sensor ~5 V`
- `GND = common ground`
- address = **0x48**
- `ADDR -> GND`
- A0 = existing GM 3-bar MAP sensor
- expose A1, A2, and A3 for future sensors
- firmware currently samples A0 single-ended at ±6.144 V full scale

Use correct local decoupling immediately adjacent to the device.

Do not power the ADS1115 at 3.3 V unless you intentionally redesign the analog front end and firmware, because the existing project expects approximately 0–5 V automotive sensor signals.

---

# 7. Analog sensor connectors

The daughterboard should expose the ADS1115 analog channels on compact, keyed/locking 3-pin connectors accessible from the outward/back side of the PCB.

Preferred connector assignment:

| Connector | Pin 1 | Pin 2 | Pin 3 |
|---|---|---|---|
| SENSOR_A0 | +5V_SENSOR | GND | ADS A0 |
| SENSOR_A1 | +5V_SENSOR | GND | ADS A1 |
| SENSOR_A2 | +5V_SENSOR | GND | ADS A2 |
| SENSOR_A3 | +5V_SENSOR | GND | ADS A3 |

A0 is mandatory because it is the existing MAP input.

Strongly prefer exposing all four ADS1115 channels. If four 3-pin connectors physically cannot fit without compromising the board, keep A0 and as many additional channels as possible, but **do not silently delete channels**. Document the mechanical tradeoff.

Use the same pin order and orientation on every sensor connector.

Prefer a compact locking connector family that is realistic on a 46 mm board, for example JST-PH/JST-GH or another well-supported equivalent. Choose based on space, current rating, assembly availability, and ease of harness construction.

Clearly label each connector and pin function on silkscreen.

---

# 8. Analog input filtering and fault protection

Each exposed analog channel will leave the PCB and enter an automotive wiring harness.

Add modest input protection without materially changing the expected 0–5 V signal.

For each A0–A3 input, evaluate:

- small series resistor
- small RC low-pass filter
- low-leakage clamps against over/under-voltage
- ESD/transient protection appropriate for a sensor input

A reasonable starting point for the RC network is approximately:

- **1 kΩ series**
- **100 nF to GND**

but verify the resulting bandwidth and ADC settling behavior rather than copying values blindly.

The current gauge updates MAP at 62.5 Hz, so filtering should reduce high-frequency harness noise without noticeably slowing real boost response.

Do not use a default resistor divider that changes normal 0–5 V sensor scaling unless firmware changes are also made.

Protect the ADS1115 against plausible wiring faults such as a signal wire briefly contacting 12 V, while keeping leakage/error low enough for pressure measurements.

---

# 9. I²C level translation

The ESP32 side is 3.3 V and the ADS1115 side is approximately 5 V.

The I²C topology should be:

## 3.3 V / MCU side

Connected to:

- H2 pin 6 / GPIO17 / SDA
- H2 pin 7 / GPIO18 / SCL
- BMP280
- external RTC
- low-voltage side of translator

## 5 V side

Connected to:

- ADS1115 only
- high-voltage side of translator

Preserve the project's **100 kHz** bus speed.

The existing system has proven stable with approximately **4.7 kΩ pull-ups**. Design the effective pull-up resistance in that general range unless the selected translator datasheet requires a different network.

### Translator preference

Prefer a real I²C translator that behaves safely if either supply rail is absent.

A device such as **TCA9406** is attractive because it supports 3.3 V <-> 5 V I²C and provides high-impedance isolation if a supply is at 0 V. Verify current availability and the exact datasheet requirements before selecting it.

A PCA9306 or BSS138-style MOSFET translator is also acceptable, but then explicitly verify:

- pull-up network
- enable/power sequencing
- behavior when one side is unpowered
- no significant back-power path

Do not use a generic push-pull logic translator that is inappropriate for open-drain I²C.

---

# 10. BMP280 ambient sensor

Place a genuine firmware-compatible **BMP280** on the 3.3 V side.

Required electrical setup:

- 3.3 V supply
- I²C address **0x76**
- `SDO -> GND`
- configure CSB appropriately for I²C mode
- local decoupling

Mechanical placement is critical:

- place the pressure sensor near the PCB edge
- keep its pressure opening unobstructed
- keep it away from the buck converter, inductor, TVS, and other heat sources
- leave a silkscreen/keepout note around the vent
- do not place conformal coating, adhesive, foam, or enclosure plastic directly over the pressure port
- design the enclosure interface so the BMP280 sees cabin/ambient air

The BMP280 is the live atmospheric reference used for gauge-pressure calculation and MAP calibration, so thermal/pressure isolation matters.

---

# 11. RTC

The current firmware already supports a **DS3231-compatible RTC at address 0x68** on GPIO17/18.

Preserving that interface is strongly preferred because it avoids a firmware rewrite.

## Preferred implementation

Use:

- DS3231 or a verified register-compatible variant
- 3.3 V main supply if supported by the selected part
- appropriate local decoupling
- backup battery connection
- compact replaceable backup cell or suitable automotive-temperature backup source

If using a primary coin cell:

- **do not implement a charging circuit**
- make polarity extremely clear
- choose a holder/source appropriate for expected vehicle cabin temperature

If the selected DS3231 package is too large, you may propose a smaller I²C RTC, but then:

1. explain exactly why,
2. identify the new address/register model,
3. provide a firmware adaptation plan,
4. do not claim drop-in compatibility.

Note that the Waveshare board itself contains a PCF85063 RTC, but the existing boost-gauge external sensor implementation does not use it. Treat switching to that RTC as a firmware/power architecture change, not as a free PCB simplification.

---

# 12. Mechanical form factor

Create a reusable tscircuit form-factor component for the Waveshare backplane geometry.

## Required outline

- round PCB
- **46.00 mm diameter**
- no copper/components outside the outline

Use a polygonal approximation of the circle if that is the best-supported tscircuit mechanism, but make it visually and dimensionally accurate.

## H2 mating header

Place a 1x8, 2.54 mm mating connector exactly aligned to Waveshare H2.

Critical:

- verify pin 1 orientation from the schematic/dimension drawing
- account for the fact that a back-to-back daughterboard can mirror connector orientation
- render/inspect both board sides so pin mapping is not accidentally reversed

Do not guess the XY position.

Use the official mechanical drawing as the source of truth.

## Mounting holes

Reproduce the Waveshare board's mounting-hole pattern from the official dimension drawing.

Use the matching hole diameters/locations and provide room for practical spacers/standoffs.

The electrical header should not be the only thing resisting vehicle vibration.

## Stack-up and clearance

The daughterboard mounts behind the Waveshare PCB.

Therefore:

- keep the mating/display-facing side as low-profile as possible
- put tall parts, sensor connectors, input power connector, backup battery holder, inductor, and other bulky items on the **outward/back face**
- define keepouts for Waveshare-side components
- make sure the Waveshare USB-C connector can still be accessed for flashing
- preserve access to any buttons/connectors that are needed in the final installation
- confirm that the chosen header/socket height provides enough inter-board clearance

Do not assume two 46 mm discs can simply be placed flush together.

Create a side/3D sanity check of the stack if tscircuit's current tooling supports it.

---

# 13. Connector placement

Required connectors on the outward/back side:

- 2-pin vehicle power input:
  - 12V_IN
  - GND
- A0–A3 three-pin sensor connectors as space allows

Place them for practical harness routing, not merely for autorouter convenience.

Prefer connectors near the perimeter.

Keep the 12 V input region physically separated from the BMP280 and analog signal traces.

---

# 14. Grounding and PCB layout

Target a **2-layer PCB** unless a 4-layer stack is genuinely needed.

Suggested strategy:

- large continuous GND plane wherever possible
- short, wide 12 V and 5 V power paths
- wide copper for the display 5 V feed
- very short regulator hot loop
- keep switching node copper minimal
- keep buck inductor/switch node far from A0–A3 traces and BMP280
- route analog traces away from the switch node
- keep I²C routing short and clean
- place decoupling capacitors adjacent to every IC supply pin
- avoid routing noisy power underneath the BMP280 if possible

Do not split ground into isolated "analog" and "digital" islands unless there is a well-justified return-current plan. A coherent low-impedance ground plane is preferred.

Use trace widths and vias appropriate for the expected current, not generic signal widths on the power rails.

---

# 15. Test points

Provide clearly labeled test points for at least:

- 12V_IN
- protected vehicle input after reverse-polarity stage
- shared/display ~5V rail
- sensor 5V rail
- 3V3
- GND
- SDA_3V3
- SCL_3V3
- SDA_5V
- SCL_5V
- A0
- A1
- A2
- A3

If space permits, also expose:

- GPIO16
- UART TX/RX

These are useful for first-board bring-up and diagnosing the sensor bus without disassembling the whole gauge.

---

# 16. Component sourcing and assembly

This should be a real manufacturable board, not a conceptual schematic.

Use the tscircuit skill and current tscircuit/JLCPCB workflows.

Prefer:

- parts currently stocked by JLCPCB/LCSC
- common passives, preferably 0603 where space permits for easier rework
- manufacturer-supported footprints
- components with real datasheets
- parts suitable for the voltage/current/temperature environment

Use `tsci search --jlcpcb` and/or `tsci import --jlcpcb` extensively where appropriate.

For every nontrivial IC, record:

- manufacturer
- exact MPN
- package
- JLCPCB/LCSC part number if available
- key voltage/current/temp rating
- reason for selection
- at least one substitute where practical

Do not use hobby breakout modules.

The mating header, sensor connectors, backup battery holder, or vehicle connector may be marked DNP/hand-solder if that is more realistic than JLC assembly.

---

# 17. tscircuit workflow

Use the current **tscircuit skill** and follow current tscircuit syntax rather than inventing APIs.

Work in stages.

## Phase 1 — research and block diagram

Before writing the final board:

1. inspect repository firmware assumptions
2. inspect official Waveshare schematic and mechanical drawing
3. decide the exact 12 V protection/buck architecture
4. decide the USB/VBUS power-sharing strategy
5. decide the I²C translator
6. decide RTC package and backup source
7. decide connector family
8. write a short block diagram / design rationale

Do not proceed with unresolved pinout or mechanical ambiguity.

## Phase 2 — schematic/netlist

Create a readable complete schematic/netlist first.

Name rails clearly, e.g.:

- `CAR_12V_RAW`
- `CAR_12V_PROTECTED`
- `BUCK_5V_RAW`
- `VBUS_SHARED`
- `SENSOR_5V`
- `3V3`
- `I2C_SDA_3V3`
- `I2C_SCL_3V3`
- `I2C_SDA_5V`
- `I2C_SCL_5V`

Run the appropriate checks before placement.

## Phase 3 — mechanical form factor and placement

Create the 46 mm round form factor, H2 header, mounting holes, and keepouts first.

Then place functional blocks:

- automotive power region
- ADC / analog region
- level shifter
- BMP280
- RTC
- sensor connectors

Run:

`tsci check placement`

or the equivalent current command provided by the installed tscircuit skill.

Inspect top and bottom PCB renders and correct overlaps/clearance problems before routing.

## Phase 4 — routing

Route critical nets intentionally before relying on generic autorouting:

1. buck converter hot loop
2. high-current 5 V paths
3. ground return
4. analog sensor inputs
5. I²C bus
6. remaining low-current nets

Use copper pours/planes where appropriate.

Then route remaining nets.

## Phase 5 — build and verification

Run the current equivalent of:

`tsci build`

and all available placement/DRC checks.

Resolve:

- overlaps
- unrouted nets
- illegal clearances
- holes outside the outline
- connector courtyard conflicts
- incorrect layer assignments
- copper too close to the board edge

Inspect the final PCB render manually.

Do not consider "the tool built successfully" sufficient proof of correctness.

---

# 18. Mechanical validation requirement

Before declaring completion:

1. verify the generated board is 46.00 mm diameter
2. compare H2 and mounting-hole geometry against the official Waveshare dimension drawing
3. check that the mating header is not mirrored
4. check component height between the two PCBs
5. check USB-C cable access
6. check sensor/power connector access
7. confirm no tall component on the daughterboard collides with the Waveshare board

If possible, export a 1:1 SVG/PDF/image of the backplane mechanical geometry for overlay/printing.

If the current tscircuit exporter has limitations around circular outlines or cutouts, verify the **actual Gerber edge geometry**, not only the browser render.

---

# 19. Electrical validation checklist

Before completion, explicitly review each item:

## Power

- [ ] reverse battery protected
- [ ] input transient/TVS protection present
- [ ] buck input rating justified
- [ ] buck output current and thermal margin justified
- [ ] display VBUS power path defined
- [ ] USB backfeed scenario analyzed
- [ ] sensor rail short behavior analyzed
- [ ] 3.3 V is not overloaded

## I²C

- [ ] GPIO17 = SDA
- [ ] GPIO18 = SCL
- [ ] 100 kHz target
- [ ] ADS1115 = 0x48
- [ ] BMP280 = 0x76
- [ ] RTC = 0x68
- [ ] correct 3.3/5 V level translation
- [ ] powered-off rail behavior checked
- [ ] effective pull-ups appropriate

## Analog

- [ ] A0 is MAP input
- [ ] A1–A3 exposed if feasible
- [ ] 0–5 V sensor scaling preserved
- [ ] input overvoltage protection present
- [ ] RC filtering does not excessively slow MAP response
- [ ] sensor 5 V and ADS supply relationship understood
- [ ] firmware MAP supply value documented

## Mechanical

- [ ] 46.00 mm diameter
- [ ] H2 exact position/orientation
- [ ] matching mounting holes
- [ ] appropriate standoff/header height
- [ ] USB-C remains accessible
- [ ] outward-facing connectors are usable
- [ ] BMP280 vent unobstructed

---

# 20. Bring-up/test procedure to include with the design

Write a short first-board bring-up procedure.

At minimum:

1. Inspect unpowered board for shorts.
2. Power from a current-limited bench supply at 12 V with the Waveshare board disconnected.
3. Verify protected input rail.
4. Verify buck/shared 5 V rail.
5. Verify sensor 5 V rail.
6. Verify no abnormal current or regulator heating.
7. Connect Waveshare board.
8. Verify H2 3.3 V.
9. Run the project's sensor scan endpoint or firmware diagnostic.
10. Expect stable I²C addresses:
    - `0x48` ADS1115
    - `0x68` DS3231
    - `0x76` BMP280
11. Apply known voltages to A0–A3 and verify ADC readings.
12. Connect GM MAP sensor and verify atmosphere reading/calibration.
13. Confirm BMP280 ambient pressure is plausible.
14. Set RTC, remove main power, and verify retention.
15. With vehicle 12 V disconnected, connect USB-C and verify USB does not reverse-power the vehicle input/buck path.
16. Verify USB flashing remains possible with the daughterboard installed.
17. Check A0 noise with the buck converter running.

---

# 21. Required deliverables

Produce a complete tscircuit project, not just screenshots.

Preferred repository location:

`hardware/boost-gauge-backplane/`

Include:

1. tscircuit source files
2. reusable Waveshare 46 mm form-factor component
3. complete schematic
4. PCB layout
5. fabrication-ready Gerbers
6. drill files
7. BOM
8. pick-and-place / centroid file if supported
9. JLCPCB/LCSC sourcing table
10. PCB top render
11. PCB bottom render
12. schematic render
13. 3D render if supported
14. `README.md` explaining:
    - architecture
    - H2 pinout
    - power path
    - USB power caveat/solution
    - sensor connector pinout
    - I²C addresses
    - RTC battery
    - assembly notes
    - bring-up steps
15. `DESIGN_REVIEW.md` containing:
    - assumptions
    - calculations
    - component-selection rationale
    - power/thermal review
    - automotive protection review
    - unresolved risks
16. a concise list of any firmware changes required

If no firmware change is required, explicitly state that the PCB preserves:

- SDA GPIO17
- SCL GPIO18
- ADS1115 0x48
- BMP280 0x76
- DS3231 0x68
- A0 as MAP input

---

# 22. Acceptance criteria

The design is not complete until all of the following are true:

- It physically matches the Waveshare 46 mm PCB form factor.
- It mates with H2 without reversing pin order.
- It can power the Waveshare board from vehicle 12 V.
- It has credible automotive input protection.
- It does not blindly create an unsafe USB-VBUS backfeed path.
- It powers the ADS1115 and external sensors from an appropriate ~5 V rail.
- It translates GPIO17/18 I²C safely between 3.3 V and the ADS1115 5 V domain.
- It includes a firmware-compatible BMP280.
- It includes a battery-backed RTC or clearly justified compatible alternative.
- It exposes A0 and preferably A1–A3 as 3-pin 5V/GND/signal connectors.
- It includes reasonable analog input protection and filtering.
- The BMP280 can breathe ambient air.
- The PCB can actually be fabricated and assembled using the generated files.
- The BOM contains real orderable parts.
- All tscircuit checks/builds complete without unresolved errors.
- The final design has been visually inspected for mechanical and electrical sanity.

---

# 23. Things you must not do

Do not:

- change GPIO17/18 just because another pin pair is easier to route
- move the sensor bus onto the Waveshare BSP I²C pins
- power GPIO17/18 pull-ups from 5 V
- power a bare BMP280 from 5 V
- power the ADS1115 from 3.3 V while still feeding it unscaled 0–5 V sensors
- substitute BME280/BMP3xx and assume the current firmware will accept it
- omit the RTC because Waveshare has an onboard RTC without explaining the firmware and backup-power implications
- directly tie an automotive buck output to USB VBUS without analyzing backfeed
- use a 20–24 V-max buck on raw automotive 12 V and call it protected
- place the BMP280 next to the inductor/regulator
- mirror H2 when creating the rear-mounted daughterboard
- use breakout modules instead of a real PCB design
- invent component footprints when verified library/JLCPCB footprints are available
- silently omit A1–A3 because placement became difficult
- claim fabrication readiness without inspecting the exported Gerber outline and holes

---

# 24. Decision policy

Where this specification gives a goal instead of a specific component, choose the implementation based on:

1. electrical correctness
2. mechanical fit
3. compatibility with existing firmware
4. automotive robustness
5. JLCPCB/LCSC availability
6. reworkability
7. cost

If two requirements conflict, **stop and document the conflict** instead of silently weakening a safety, pinout, or mechanical requirement.

The final result should look like a purpose-built automotive gauge backplane, not a collection of breakout-board circuits copied onto one PCB.
