# Prebuilt firmware v0.5.0 — real MAP conversion and atmosphere calibration

Firmware **`v0.5.0`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Includes the embedded Wi-Fi dashboard, configurable zero-notch scale, the
DMA-safe AMOLED display path (`main/boost_display.c`), and the live GM 12223861
MAP path with BMP280-referenced atmosphere calibration. The same files are
published on the [latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time rather than a literal — the previous releases
all shipped reporting a stale hard-coded `0.3.0-web`.

## Verified on hardware for this release

Measured on the board at `192.168.50.102`, running this exact `boost_gauge.bin`
(SHA-256 `3cb1337b…`, byte-identical to the published asset):

| Gate | Result |
|---|---|
| Boot and network after OTA | `firmwareVersion v0.5.0`, control plane back in ~6 s |
| Physical cadence, 30 s (arc face, demo mode) | **min 57, median 60 FPS** over 104 samples |
| Sensor bus | `busUp true`, `["0x48","0x76"]`, **0 recoveries** |
| Atmosphere calibration | offset **+2.43 kPa** over 40 samples; gauge reads **−0.0 PSI** at atmosphere |
| Calibration durability | survived reboot *and* a firmware update with offset, sample count and supply intact |
| Sensor soak, 120 s | **0 faults, 0 recoveries**, 0.040 PSI total spread |
| Display metrics | `teTimeouts 0`, `worstRenderUs` 19,675 |

**Not re-verified this cycle**, and not claimed: serial-log error absence (the
board runs from 5 V with no serial attached), the media upload/abort/delete
cycle (no GIF is present on the device and this release does not touch the media
path), and WebSocket-transport badge behaviour on hardware (verified against the
host mock in HTTP-fallback mode only).

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
**20 Hz**, browser canvas interpolation runs at **60 FPS** with a short 35 ms
EMA, and HTTP fallback is **4 Hz**. These are separate cadences.


## Operational dashboard and GIF notes

The web server has a fixed pool of **3 WebSocket clients**. A fourth handshake
is rejected/closed only for the newcomer; existing sockets remain connected.
When a browser falls back, it polls `/api/v1/state` at **4 Hz** and retries the
WebSocket every **1 s**. The badge identifies **Live · WebSocket 20 Hz** versus
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
