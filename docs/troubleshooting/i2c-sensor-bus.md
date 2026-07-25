# Troubleshooting the external I2C sensor bus

Bench investigation log for the ADS1115 + BMP280 bus on I2C port 0
(SCL=GPIO18, SDA=GPIO17, 100 kHz). Kept out of the README because it is history,
not architecture; the README states the final design and the regression ledger in
`AGENTS.md` carries the durable lessons.

## Final state

Both devices are discoverable and reading on the **original** GPIO18/17 pins:

```
{"busUp":true,"recoveries":0,"found":["0x48","0x76"]}
"sensors":{"adsPresent":true,"bmpPresent":true,"fault":false,
           "mapVolts":1.5735,"ambientKpa":98.58}
```

The bus was restored by **replacing the ADS1115 assembly**, per the
`2026-07-25-map-atmosphere-calibration-design.md` field finding. GPIO17 and
GPIO18 were not damaged, the RX/TX remap was never needed, and the 400 kHz to
100 kHz reduction was **not** the fix. 100 kHz is retained on its own merits: the
run passes through a MOSFET level shifter with 4.7 kOhm pull-ups, whose rise
times do not comfortably support 400 kHz in a vehicle.

## What the symptoms looked like

The sequence is worth recognising because almost every stage pointed somewhere
misleading.

1. Both sensors undetected. `/api/v1/sensors/scan` returned `found: []`.
2. The board schematic was decoded to confirm GPIO17 is clean: ESP pin 23, TP14,
   and the header, nothing else. The clamp had to be external.
3. SCL measured correctly at the level shifter (3.3 V on LV2, 5.5 V on HV2) while
   SDA measured **0 V** on LV1/HV1. Moving SDA to LV4/HV4 restored it: the scan
   found both addresses and blowing into the MAP sensor moved the gauge.
4. It failed again spontaneously, minutes later, with SDA back at 0 V.
5. Boot-time and runtime bus recovery were implemented and flashed. Both ran and
   the bus stayed empty, which **rules out a stuck slave holding SDA** — a
   clock-out releases that. This is a physical fault, not an I2C lockup.
6. With both sensors and the level shifter removed entirely, an otherwise empty
   bus carrying only pull-ups still returned changing one-off addresses on every
   sweep. That turned out to be a scanner artifact, not a bus fault (below).
7. A BMP280 wired directly at 3.3 V, with the ADS removed, still would not ACK
   reliably: 0-4 successes per 32 probes at `0x76`, against 0-2/32 for the
   deliberately absent `0x48` control.

Stages 6 and 7 are the reason this took so long: a broken measurement instrument
was layered on top of a genuine hardware fault, and each one kept producing
evidence that discredited the other.

## Diagnostic reasoning that held up

- **A disconnected SDA floats high.** With I2C active, SDA is open-drain and
  idles high through the pull-up. A steady 0 V therefore means something is
  actively pulling it down — a short, or a device clamping the line. It does not
  mean "not connected".
- **A voltage-high reading does not prove continuity.** Both the MCU side and the
  sensor module have pull-ups, so each end can read 3.3 V with the wire between
  them broken. Continuity must be tested endpoint-to-endpoint with power off.
- **Clock-out recovery that changes nothing is informative.** It falsifies the
  entire class of stuck-slave explanations in one test.
- **Boot-time presence flags are not evidence of a live sensor.** `adsPresent`
  and `bmpPresent` were both true while the bus was dead, because they record
  what answered once at boot. This is why the calibration path now gates on
  explicit freshness (`ads_age_ms` / `bmp_age_ms` / `bmp_updates`) instead.

## The phantom-address scanner bug

Independently of the hardware fault, `/api/v1/sensors/scan` was inventing
addresses. Three controls isolated it:

| Control | Result | Reading |
|---|---|---|
| Manual bit-banged probe at unused `0x55` | 0/64 ACKs, idle-low counters both zero | The bus is electrically clean and idles correctly |
| Driver `i2c_master_probe()` repeatedly at fixed `0x55` | 64/64 `ESP_ERR_NOT_FOUND` | The driver does not false-ACK at a *fixed* address |
| Driver probe at onboard touch `0x5A` | 8/8 ACK | The driver does detect a device that is genuinely there |

Phantoms appeared only when `i2c_master_probe()` swept through **changing**
addresses. Requiring **four consecutive ACKs** before reporting an address made
20/20 empty-bus scans correctly empty, at 19-38 ms per scan.

This strongly implicates the changing-address probe sequence. Without waveform
capture it does not formally rule out every data-pattern-dependent electrical
mechanism, so the four-ACK filter is retained as the guard rather than treated as
a closed root cause.

## If it happens again

In order, cheapest first:

1. `curl http://<board-ip>/api/v1/sensors/scan` — expect `["0x48","0x76"]`.
   Check `recoveries`: a climbing count means the bus is wedging and self-healing.
2. `curl http://<board-ip>/api/v1/sensors/calibration` — check `mapAgeMs` and
   `bmpAgeMs`. Fresh ages with a stale-looking gauge is a conversion problem, not
   a bus problem. `-1` means the sensor has never read successfully.
3. Power off, then **continuity-test endpoint-to-endpoint**: VCC, GND, SCL, SDA.
   Near-zero ohms on each intended conductor, and no SCL/SDA short to ground.
4. Reseat every jumper at both ends and tug-test. An intermittent that works then
   fails on bench vibration is a connector, not firmware.
5. Only then suspect the level shifter or a device. Swap one part at a time and
   require **sustained valid reads**, not just an address ACK — address presence
   and working device are separate gates, as stage 7 above demonstrates.

Firmware self-heal is already in place and does not need to be re-added: an
in-place `i2c_master_bus_reset()` fires after sustained read failures and once at
boot if nothing answers, serialized against HTTP scans by a bus-admin mutex.
