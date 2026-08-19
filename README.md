# Boost Gauge

An open-source digital boost/vacuum gauge and dashboard for the **[Waveshare ESP32-S3-Touch-AMOLED-1.75](https://www.waveshare.com/esp32-s3-touch-amoled-1.75.htm)** (466×466 CO5300 AMOLED).

It runs an ESP-IDF + LVGL firmware that samples a GM 3-bar MAP sensor through an ADS1115 (with an optional BMP280 for ambient reference), renders a crisp 60 Hz analog-style gauge, and serves a companion web dashboard for telemetry, theming, TPMS, and Wi-Fi/OTA setup.

It swaps straight onto the Waveshare board, replacing the stock factory launcher.

## Screenshots

| Boost / vacuum | Overboost | Neon theme |
|---|---|---|
| ![Boost gauge](preview/sim/gauge_boost.png) | ![Overboost](preview/sim/gauge_over.png) | `tools/sim` generates more |

More rendered previews live in [`preview/sim/`](preview/sim/).

## Features

- **Live gauge at 60 Hz** — a single filled arc that changes climate at zero: teal vacuum, lime boost, red overboost, with a big signed PSI readout and peak hold.
- **Five full gauge faces** (themes), each a distinct layout, not just a palette swap:
  - **Dyno Cell** — the classic dual-climate arc (default)
  - **Vault-Tec** — Fallout-style phosphor needle dial with CRT scanlines
  - **Night City** — cyberpunk targeting HUD with glitch shear
  - **Big Digit** — huge Alvida Fatface numeric readout
  - **Neon** — glowing neon-tube face with three layouts and four color presets
- **Two-finger gestures** on the screen itself: tap to reset peak, swipe up/down to change themes, swipe left/right to flip between the gauge and a TPMS view, hold to dim, two-finger hold to show a join-my-AP QR code.
- **Web dashboard** served by the board — live cockpit, sparkline, settings, Wi-Fi setup, theme editor, and OTA updates over HTTP/WebSocket.
- **OBD-II / BLE TPMS** — reads tire pressure from a Mazda MX-5 ND (and generic mode-01 PIDs + battery voltage on any car) through a BLE ELM327 adapter, plus a built-in simulator when no adapter is connected.
- **Battery-backed clock** via a DS3231 RTC, DST-aware timezones, and dim-schedule control.
- **GIF playback** — load your own full-screen animation onto the `media` partition.

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-1.75
- GM 12223861 (3-bar MAP) sensor through an ADS1115 on the sensor I2C bus
- Optional BMP280 ambient-pressure sensor
- Optional DS3231 RTC for a battery-backed clock
- Optional BLE OBD-II/ELM327 adapter for TPMS / live PIDs

## Quick start (flash the prebuilt image)

The easiest path needs no ESP-IDF toolchain:

```bash
git clone https://github.com/ZiadAbdelati/waveshare-boost-gauge.git
cd waveshare-boost-gauge/release
python -m pip install esptool

# Linux / macOS
./flash.sh /dev/ttyACM0

# Windows PowerShell (replace COM5 with your port)
python -m esptool --chip esp32s3 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 boost_gauge_merged.bin
```

Newest prebuilt firmware (currently **v0.8.0**, ESP-IDF 5.5.1) is in [`release/`](release/) and on the [releases page](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest). See [`release/README.md`](release/README.md) for details.

### Connect to it

- With a saved Wi-Fi network: the dashboard is served at the board's address (printed to serial as `BOOST_WEB_IP=`).
- Otherwise the board runs its own access point **`BoostGauge-XXXX`** / password `boost1234` → `http://192.168.4.1/`.

## Build from source

Requires ESP-IDF **v5.5.1** with the `esp32s3` target. Installation instructions are in the [official ESP-IDF docs](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/).

```bash
git clone https://github.com/ZiadAbdelati/waveshare-boost-gauge.git
cd waveshare-boost-gauge

# point at your ESP-IDF install, then:
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # Linux/macOS — use your serial port
```

First build fetches the `waveshare/esp32_s3_touch_amoled_1_75` and `lvgl/lvgl` components from the ESP-IDF component registry (requires network).

**Troubleshooting a stubborn flash:** hold **BOOT**, tap **RESET**, start the flash, then release **BOOT** when upload begins.

## Themes

Switch faces with a vertical swipe (up = next, down = previous) or from the Settings page in the dashboard:

```
Dyno Cell → Vault-Tec → Night City → Big Digit → Neon → Dyno Cell …
```

Zone colors, arc range, the zero notch angle, and theme-specific options (like Vault-Tec needle color or the Neon layout/preset) are persisted in NVS and editable in the dashboard.

## Project layout

```
boost-gauge/
├── main/            # ESP-IDF firmware (display, gauge, web, sensors, TPMS, OBD)
├── web/             # dashboard sources embedded into the firmware
├── tools/           # asset embedding, mock server, bench/cadence utilities
├── sim/             # host-side LVGL simulator (no board required)
├── release/         # prebuilt firmware images
├── docs/            # development and technical reference
└── preview/         # rendered simulator screenshots
```

## Documentation

The detailed development notes, measurements, and architecture docs moved out of the README into [`docs/`](docs/README.md):

- [Architecture](docs/architecture.md) — scene system, caching, drawing/invalidation rules
- [Display & cadence](docs/display-and-cadence.md) — AMOLED bring-up, DMA buffers, the 60 Hz contract
- [Network & telemetry](docs/network-and-telemetry.md) — WebSocket/HTTP, clock/RTC, GIF upload
- [GIF playback](docs/gif-playback.md) — pipeline, performance contract
- [TPMS & OBD](docs/tpms-obd.md) — BLE adapter, Mazda DID set, simulator
- [Sensors & calibration](docs/sensors-and-calibration.md) — MAP bus, GM curve, calibration
- [GUI guide](docs/gui-guide.md) — gestures, two-page layout, dashboard notes
- [Themes](docs/themes.md) — theme system, Neon details, design tokens
- [Release notes](docs/release-notes.md) — per-version notes
- [Regression ledger](docs/regression-ledger.md) — full measurement history

## Restoring the factory launcher

To get the stock Waveshare app grid back, flash the factory image from the [Waveshare ESP32-S3-Touch-AMOLED-1.75 releases](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/releases).

## License

Third-party code shipped with this project (notably the AnimatedGIF core and the OFL-licensed fonts) is attributed in its sources under `main/gif/` and `main/fonts/`. See each file for its license terms.
