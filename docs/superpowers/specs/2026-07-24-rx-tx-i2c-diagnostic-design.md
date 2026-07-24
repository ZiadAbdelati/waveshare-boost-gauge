# RX/TX I2C Diagnostic Design

## Purpose

Determine whether the sensor-bus failures are caused by damaged GPIO17/GPIO18
after their brief exposure to the level shifter's 5 V side.

## Design

Build and OTA-flash a temporary firmware image that keeps I2C port 0 and the
existing 100 kHz bus rate, but changes the sensor pins to:

- SCL: exposed TX pad, GPIO43
- SDA: exposed RX pad, GPIO44

GPIO43 is deliberately used for SCL. UART0 may emit early boot text on its TX
pin before application initialization; with SDA held high, those transitions
are clocks rather than I2C START conditions. `i2c_new_master_bus()` then assigns
GPIO43/GPIO44 to the I2C peripheral before sensor probing.

No display, web, sensor-conversion, OTA, or persistent-setting behavior changes.

## Hardware Procedure

1. OTA-flash and restart the diagnostic firmware while the existing wiring
   remains on GPIO17/GPIO18.
2. Confirm the HTTP API returns after restart.
3. Remove power.
4. Move BMP280 SCL and its 4.7 kOhm pull-up from GPIO18 to TX/GPIO43.
5. Move BMP280 SDA and its 4.7 kOhm pull-up from GPIO17 to RX/GPIO44.
6. Keep BMP280 VDD at 3.3 V, all grounds common, SDO grounded, and CS/CSB at
   3.3 V when that pin exists.
7. Restore power and test for stable address `0x76` and valid ambient pressure.

## Interpretation

- Stable `0x76` and valid BMP280 reads on GPIO43/GPIO44 implicate GPIO17/GPIO18.
- Continued random addresses on GPIO43/GPIO44 implicate wiring, the BMP280
  breakout/configuration, or a broader firmware/measurement issue rather than
  only the original GPIO pair.

## Verification and Recovery

- Before editing, a source assertion must fail because the existing mapping is
  GPIO18/GPIO17.
- After editing, the same assertion must pass for GPIO43/GPIO44.
- The full ESP-IDF application build must succeed and produce
  `build/boost_gauge.bin`.
- OTA must report a validated image and selected boot partition.
- After restart, `/api/v1/state` must return before rewiring.
- The prior OTA slot remains the rollback path if the diagnostic image cannot
  bring up the HTTP control plane.

