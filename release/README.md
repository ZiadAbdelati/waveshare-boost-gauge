# Prebuilt firmware v0.3.2 — configurable dial zero

Firmware **`0.3.0-web`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Includes the embedded Wi-Fi dashboard, configurable zero-notch scale, and the DMA-safe AMOLED display path (`main/boost_display.c`). The same files are published on the [latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

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
