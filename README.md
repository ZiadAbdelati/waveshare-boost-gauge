# Boost Gauge

ESP-IDF + LVGL boost gauge for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 CO5300 AMOLED).

Right now the MAP path is **simulated** (vacuum ↔ boost sweep). Swap `boost_sim.c` for ADS1115 reads when the sensor wiring is ready.

## What you should see

- Full-screen dark cabin gauge (“Pit Lane Night”)
- Dual-climate arc: **teal** vacuum · **amber** boost · **flare red** overboost (≥ 18 psi)
- Big signed PSI, zone label (`VAC` / `ATMO` / `BOOST` / `OVER`)
- Peak hold; **short tap** resets peak
- **Hold ~2s** toggles max/min brightness (100% ↔ 12%)
- Top chip reads `DEMO` until a live sensor path sets `sample.demo = false`

This firmware **replaces** the factory app launcher.

## Layout

```
boost-gauge/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/                  # source
├── release/               # prebuilt firmware (flash this)
└── README.md
```

## Fast path: flash prebuilt (no ESP-IDF)

A verified build (ESP-IDF **5.5.1**, app size ~1.35 MB) is in [`release/`](release/).

```bash
git clone https://github.com/ZiadAbdelati/waveshare-boost-gauge.git
cd waveshare-boost-gauge/release
python -m pip install esptool

# Linux / macOS
./flash.sh /dev/ttyACM0

# Windows PowerShell (replace COM5)
python -m esptool --chip esp32s3 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 boost_gauge_merged.bin
```

Hold **BOOT**, tap **RESET**, start flash, release **BOOT** if download mode fails.

Details and split binaries: [`release/README.md`](release/README.md).

## Web control plane

The firmware starts a local Wi-Fi access point named `BoostGauge-XXXX` (last four hex digits come from the board MAC). Connect with password `boost1234`, then open `http://192.168.4.1/`.

The embedded dashboard mirrors live boost over SSE and provides brightness/dim scheduling, three themes, time sync, bounded logs with CSV export, validated GIF storage/status, and dual-slot OTA upload. GIF playback is explicitly deferred; uploads are stored but not rendered on the gauge.

For host-only UI development:

```bash
python3 tools/mock_server.py --host 127.0.0.1 --port 18080
# open http://127.0.0.1:18080/
```

After editing top-level files in `web/`, regenerate the firmware assets before building:

```bash
python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
```

Do not expose the access point or OTA endpoint beyond a trusted local network; this first local-only control plane has no per-request authentication.

---

## Desktop simulator

Same LVGL UI, no board required:

```bash
cmake -S sim -B sim/build
cmake --build sim/build -j"$(nproc)"
./sim/build/boost_gauge_sim --screenshot preview/sim
python3 sim/raw_to_png.py preview/sim
```

Headless LXC/CI works as-is (memory display + snapshot). Windowed:

```bash
xvfb-run -a ./sim/build/boost_gauge_sim --window
```

See [`sim/README.md`](sim/README.md). Real LVGL screenshots live under [`preview/sim/`](preview/sim/).


## UI design tokens

| Token | Hex | Role |
|------|-----|------|
| VOID | `#050608` | AMOLED background |
| GHOST | `#1A1D24` | soft well |
| STEEL | `#6B7280` | ticks / units |
| ICE | `#E8ECF2` | primary number |
| TEAL | `#2EE6C5` | vacuum |
| AMBER | `#FFB020` | boost |
| FLARE | `#FF3B30` | overboost |

Signature move: one arc that **changes climate** at zero and flares past the overboost tick, instead of a generic multi-color rainbow gauge.

---

## Build from source

Verified on **ESP-IDF v5.5.1** (Waveshare also documents v5.5.x / v6.0.x).

```bash
# Example: official install (Linux/macOS)
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5.1 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
source ./export.sh
```

Windows: use the [Espressif online installer](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html), then open **“ESP-IDF PowerShell”**.

### Get this project

```bash
git clone https://github.com/ZiadAbdelati/waveshare-boost-gauge.git
cd waveshare-boost-gauge
```

### Connect the board

- USB-C data cable (not charge-only)
- Note the serial port:
  - Linux: `/dev/ttyACM0` or `/dev/ttyUSB0` (`ls /dev/ttyACM* /dev/ttyUSB*`)
  - macOS: `/dev/cu.usbmodem*`
  - Windows: `COMx` in Device Manager
If flash fails to enter download mode: hold **BOOT**, tap **RESET**, start flash, release **BOOT** when upload begins.

### 4. Configure, build, flash

```bash
# every new shell
source ~/esp/esp-idf/export.sh   # path from your install

cd /path/to/boost-gauge

idf.py set-target esp32s3
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with your serial device, e.g.:

```bash
idf.py -p /dev/ttyACM0 flash monitor
# Windows
idf.py -p COM5 flash monitor
```

First build downloads `waveshare/esp32_s3_touch_amoled_1_75` and `lvgl/lvgl` via the component manager (needs network).

Monitor quit: `Ctrl+]`.

### 5. One-liner after the first successful build

```bash
idf.py -p /dev/ttyACM0 flash monitor
```

### Optional: flash without rebuilding

```bash
idf.py -p /dev/ttyACM0 flash
```

---

## Restore factory launcher

If you want the phone-style app grid back, flash Waveshare’s release/factory image from:

https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75/releases

or the `firmware/` recovery package in that repo.

---

## Next: real MAP + ADS1115

1. Wire ADS1115 to board I2C: **SDA=GPIO15**, **SCL=GPIO14**, 3.3 V logic.
2. Feed the GM 3-bar MAP through the ADS1115 (respect 5 V sensor / level shifting).
3. Replace `boost_sim_tick()` with ADC conversion → kPa → gauge PSI.
4. Set `sample.demo = false` so the chip shows `LIVE`.

Keep `boost_gauge_update()` as-is; only the sample source changes.
