# Prebuilt firmware v0.9.1 — Display/Demo reorg, About versions, web companion toggle

Firmware **`v0.9.1`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines: **Display** settings are now 3 sub-groups (Brightness / Dim
schedule / Panel with rotation, regionDBuf, teSync, teScanline, pixelShift);
**Theme & demo → Demo mode** (demoMode + waveform only); **About** footer in
Settings shows App `0.9.1 (2)` + Gauge firmware (or Not connected); **web UI**
gains a **Companion app BLE** toggle in Settings → Display (`PUT /config`
`appBle`); **BLE binding** fixes remove the “connected on Settings but Not
connected on Status/Themes/Logs” split (all tabs rebind on `transportID`);
**scan dedup** prevents the saved gauge appearing twice. Carries v0.9.0’s DMA-safe
AMOLED path, DS3231/DST clock, and calibrated GM 12223861 MAP path.
The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time rather than a literal. Note that this value is
captured at CMake **configure** time, not build time: a tree that was dirty when
`idf.py` last configured will keep reporting `-dirty` through subsequent clean
builds until `idf.py reconfigure` runs. This release was tagged first, then
built from the clean tagged tree and reflashed, so the board reports a clean
`v0.9.1`.

## What changed since v0.9.0

- **Display / Demo reorg.** Display now has 3 grouped sections — Brightness,
  Dim schedule, Panel (rotation, regionDBuf, teSync, teScanline, pixelShift +
  interval) — with a single **Save display settings** (`PUT /config` +
  `PUT /themes/config`). **Theme & demo → Demo mode** (demoMode + Demo
  waveform dropdown + Save demo settings). `appBle` is no longer in the phone
  apps (disconnect trap while on BLE); the web UI now owns it.
- **About footer.** Settings root shows **App `0.9.1 (2)` + Gauge firmware**
  (or Not connected) on both iOS and Android, so the build on the phone is
  always visible.
- **Web Companion app BLE.** Settings → Display gains a **Companion app BLE**
  toggle (`PUT /api/v1/config {"appBle":bool}`), auto-saved, immediate effect.
  Help text notes that disabling while a companion is connected drops that link.
- **Header / scan dedup.** Settings sub-pages no longer repeat the page title as
  a small sub-header. Connection scan filters the saved peer from live results
  so the same gauge doesn’t appear twice.
- **Transport binding.** Status/Themes/Logs/Calibration now listen to
  `transportID` so the “Connected on Settings but Not connected elsewhere”
  split can’t recur after the BLE auto-reconnect wins the link.

## Verified on hardware for this release

Re-measured on the board at `192.168.50.102`, running the exact image published
here (clean tree, tagged `v0.9.1`, built from the tag, flashed, hard-reset).

| Gate | Result |
|---|---|
| Boot and network | Board reached `HTTP API ready` on `192.168.50.102`; boot log shows clean `v0.9.1`; `GET /api/v1/state` reports `firmwareVersion v0.9.1` |
| Web Companion toggle | `PUT /api/v1/config {"appBle":true/false}` round-trips; Settings → Display toggle reflects `config.appBle`; disabling while BLE-connected drops the link as documented |
| Display / Demo reorg | Display shows Brightness / Dim schedule / Panel subgroups; Demo mode shows waveform dropdown; single Save display does both PUTs |
| About footer | iOS and Android Settings root show App `0.9.1 (2)` and Gauge firmware when connected, “Not connected” otherwise |
| Scan dedup + header | Saved gauge not duplicated in scan results; no redundant small headers under big nav titles |
| Companion BLE binding | All tabs rebind on `transportID`; Status/Themes/Logs no longer stay “Not connected” after auto-reconnect |
| Cadence (reference) | Prior `dyno-cell` 30 s soak reference held at `v0.9.0`; re-measure `python3 tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30` in demo mode if display path was touched |

GIF, WebSocket, and OTA gates were verified at `v0.9.0` and are unchanged in this
release (no display/media/transport path touched beyond the binding fix).

## Display path (do not regress)

This image must **not** use stock `bsp_display_start()` for LVGL. Draw buffers
live in internal DMA RAM (`use_psram = false`, 20-line strips). Putting LVGL
buffers in PSRAM on ESP32-S3 QSPI causes `ESP_ERR_NO_MEM` flush failures and a
half-white / half-green panel. See root `README.md` → “Critical: AMOLED display
path”.

## Files

| File | Purpose |
|------|---------|
| `boost_gauge_merged.bin` | Full image at offset `0x0` (easiest) |
| `bootloader.bin` | Bootloader @ `0x0` |
| `partition-table.bin` | Partition table @ `0x8000` |
| `boost_gauge.bin` | App/OTA image @ `0x20000` |
| `ota_data_initial.bin` | Initial OTA selection data @ `0xf000` |
| `BoostGauge-android-debug.apk` | Android companion app (0.9.1) |
| `BoostGauge-ios-app.zip` | iOS companion app archive (0.9.1) |
| `flash.sh` | Linux/macOS flash helper |
| `SHA256SUMS` | Checksums |

After flashing with empty STA secrets, join SoftAP `BoostGauge-XXXX` /
`boost1234` → `http://192.168.4.1/`. With `main/boost_wifi_secrets.h` present at
build time, the image also joins your LAN (APSTA) and serial logs
`BOOST_WEB_IP=<dhcp>` for agent access. Secrets are never shipped in this
prebuilt tree — rebuild from source for STA.

The merged image resets the complete firmware/partition layout and lays down the
current raw `media` partition. Full flashing migrates old SPIFFS storage to this
layout. For later web OTA updates, upload `boost_gauge.bin`, not the merged image;
app-only OTA does not replace the partition table or media partition.

The `media` partition is at offset `0x820000`, size `0x7E0000`, and is split into
two raw slots. Uploads erase only the required aligned range in the inactive
slot, stream the payload with CRC, and write the committed header last. Boot
scans CRC-valid headers and selects the newest generation; playback uses an LVGL
variable descriptor over the selected slot. An aborted upload preserves the
previous GIF.

The full **1,379,129-byte** GIF upload completed in **7.504 s** on hardware.
Clicking Delete during upload aborts the browser XHR, waits for settlement, then
sends `DELETE`; overlapping delete/upload requests are rejected with **409**.
Two repeated deletes succeeded, and the physical gauge resumed with PSI changing.
The physical gauge remains a 16 ms (~60 Hz) path. WebSocket telemetry targets
**62.5 Hz**, browser canvas interpolation runs at **60 FPS** with a short 35 ms
EMA, and HTTP fallback is **4 Hz**. These are separate cadences.


## Operational dashboard and GIF notes

The web server has a fixed pool of **3 WebSocket clients**. A fourth handshake
is rejected/closed only for the newcomer; existing sockets remain connected.
When a browser falls back, it polls `/api/v1/state` at **4 Hz** and retries the
WebSocket every **1 s**. The badge identifies **Live · WebSocket 60 Hz** versus
**Live · HTTP 4 Hz**; **Disconnected** means both transports failed.

GIF playback is hybrid, not a custom decoder: custom web upload and raw
dual-slot CRC/committed-header storage feed `esp_partition_mmap`; custom gauge
integration creates an RGB565 `lv_gif` over an `lv_image_dsc_t` pointing at the
mapping. LVGL `lv_gif` plus bundled AnimatedGIF (`GIF_openRAM`, `GIF_playFrame`)
perform parsing, LZW, timing, and composition. The repository's LVGL changes
zero the full canvas and disable the turbo buffer so standard decoding handles
delta/disposal frames. Keep the mapping valid until widget destruction; use
display lock → destroy widget → unmap. The internal-DMA partial CO5300 flush is
custom display plumbing, separate from GIF decompression.

## Flash (merged image)

```bash
# install esptool once
python -m pip install esptool

# Linux / macOS
./flash.sh /dev/ttyACM0

# or manually
python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 boost_gauge_merged.bin
```

Windows (PowerShell, COM port from Device Manager):

```powershell
python -m esptool --chip esp32s3 -p COM5 -b 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0 boost_gauge_merged.bin
```

If the board does not enter download mode: hold **BOOT**, tap **RESET**, start the flash command, then release **BOOT**.
