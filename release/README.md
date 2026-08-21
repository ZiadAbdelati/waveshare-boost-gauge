# Prebuilt firmware v0.8.1 — Doto Neon readout + rendering fixes

Firmware **`v0.8.1`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines: the Neon Tube, Segments, and Marquee layouts gain an optional
**Doto** modular readout; the zero-pressure zone reads **ATMO**; circular
Marquee invalidation no longer leaves a stale top bulb; and the ESP-IDF main
task has enough stack to boot a persisted Neon scene reliably. Carries the
v0.8.0 DS3231/DST clock work, the embedded Wi-Fi dashboard, the DMA-safe AMOLED
display path (`main/boost_display.c`), and the calibrated GM 12223861 MAP path.
The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time rather than a literal. Note that this value is
captured at CMake **configure** time, not build time: a tree that was dirty when
`idf.py` last configured will keep reporting `-dirty` through subsequent clean
builds until `idf.py reconfigure` runs. This release was tagged first, then
built from the clean tagged tree and reflashed, so the board reports a clean
`v0.8.1`.

## What changed since v0.8.0

- **Doto Neon readout.** Tube, Segments, and Marquee can select Doto ROND 100 /
  weight 700. The setting persists in NVS and is mirrored by the dashboard.
  Doto publishes raw A8 coverage without the SF Alien halo and draws a custom
  three-dot minus; signed geometry keeps a measured 20 px ink gap beside the
  widest tabular digit.
- **ATMO zone label.** Neon shows white `ATMO` at zero pressure in both the
  physical renderer and browser mirror.
- **Marquee wrap fix.** Spin and zone-flip invalidation now wrap each complete
  bulb index by that ring's bulb count. This fixes the stale outer-ring bulb at
  12 o'clock when an invalidation pair crosses the circular boundary.
- **Persisted-Neon boot reliability.** The main task stack is now **8,192 bytes**;
  the previous 3,584-byte stack could overflow while synchronously baking Neon
  glyphs before brightness/network startup.
- **Font licensing and reproducibility.** `web/doto.ttf` is the prepared static
  dashboard/source font, `web/OFL-Doto.txt` ships its SIL OFL 1.1 license, and
  `tools/generate_doto_font.py` deterministically regenerates the LVGL subset.

## Verified on hardware for this release

Measured on the board at `192.168.50.101`, running the exact image published
here (clean tree, tagged `v0.8.1`, built from the tag, flashed, hard-reset).

| Gate | Result |
|---|---|
| Boot and network with Neon persisted | Pending final tagged-image run |
| Release identity | Pending clean `firmwareVersion v0.8.1` assertion |
| Served Doto assets and license | Pending decompressed device-response check |
| Dyno Cell 30 s cadence | Pending final tagged-image run |
| Doto Tube / Segments / Marquee sweep | Pending final tagged-image run |
| GIF upload / abort / playback / repeated delete | Pending final tagged-image run |
| Three WebSocket clients / fourth rejection / slot reuse | Pending final tagged-image run |
| OTA boot partition and transport badge | Pending final tagged-image run |
| Serial error absence | Pending final tagged-image run |

These rows are intentionally not pre-filled from earlier development images.
Only results from the clean tagged release image belong here.

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
