# Prebuilt firmware v0.8.0 — battery-backed clock (DS3231 RTC) + DST

Firmware **`v0.8.0`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines: the wall clock is now authoritative from a **DS3231 RTC on the
sensor I2C bus** (survives power-off with no Wi-Fi, the dim schedule stays
correct, and a wrong browser clock can no longer corrupt it), timezones are
**DST-aware** via a POSIX TZ string, and a pair of latent RTC/`/state` bugs were
fixed on hardware. Carries everything from v0.7.1 — the tube/segments/marquee
neon faces, four colourways, the embedded Wi-Fi dashboard, the DMA-safe AMOLED
display path (`main/boost_display.c`), and the calibrated GM 12223861 MAP path.
The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time rather than a literal. Note that this value is
captured at CMake **configure** time, not build time: a tree that was dirty when
`idf.py` last configured will keep reporting `-dirty` through subsequent clean
builds until `idf.py reconfigure` runs. This release was tagged first, then
built from the clean tagged tree and reflashed, so the board reports a clean
`v0.8.0`.

## What changed since v0.7.1

- **DS3231 RTC as boot-time clock authority.** `boost_sensors_rtc_read/write`
  (probe `0x68` on the shared sensor bus) reject oscillator-stop-flag / garbage /
  implausible time so a fresh part never seeds `2000-01-01`. The seed runs before
  boot brightness is decided, so a night boot with a set RTC comes up dim from
  the first frame with no Wi-Fi. A browser Sync writes the RTC as calibration,
  and the seed refreshes the NVS epoch checkpoint as a warm fallback. Hardware-
  verified across a soft reset **and** a full ~2-minute power-off (clock came up
  exact, `deltaSec=0`).
- **RTC is the write authority too.** `POST /api/v1/time` more than 5 minutes
  from a valid DS3231 is rejected with **`409 clock_rejected`** *before* the
  system clock/NVS/RTC are touched, so a slow client cannot corrupt the
  battery-backed clock. OSF/unreadable/absent RTC accepts any plausible epoch
  (first seed / battery change).
- **OSF cleared on write.** The DS3231 does *not* auto-clear its oscillator-stop
  flag when the time registers are written; `boost_sensors_rtc_write()` now
  clears it explicitly (status `0x0F` bit 7) under the bus-admin lock. Without
  this every read reported "time never set" and the RTC fell back to NVS forever.
- **DST-aware timezone (POSIX TZ string).** Config stores `timezoneTz` (e.g.
  `EST5EDT,M3.2.0/2,M11.1.0/2`), applied via `setenv("TZ")+tzset()`; the dim
  schedule, CSV and the effective offset use `localtime()`, so DST is automatic
  and the zone is set once, never re-picked per season. `/state` reports the
  DST-effective offset; `/config` keeps the stored standard offset for the
  dropdown. The web dropdown carries the POSIX TZ string per option (US/CA/
  Europe/AU/NZ rules verified against GNU date; the `UTC-04:00` entry is relabeled
  **Atlantic Time**, not "Eastern DST").
- **`/state` offset stability.** `boost_model_publish_sample()` no longer clobbers
  the DST-effective `timezoneOffsetMinutes` with the stored standard offset,
  which previously made the dashboard clock flicker one hour across DST.
- **I2C bus hardening.** RTC read/write now hold the bus-admin mutex (the reset
  takes no lock); recovery honors ADS/BMP re-config returns (no silent 0 V
  false-good) and re-probes devices absent at boot; the live scan is time-capped
  (5 s) so a hung bus cannot wedge httpd.
- **Web fixes.** The timezone dropdown no longer reverts to the old zone when
  saving (a stale-state reset in `syncDeviceClock` was stomping fresh
  selections); the Sync button is renamed **Save**.

## Verified on hardware for this release

Measured on the board at `192.168.50.101`, running the exact image published
here (clean tree, tagged `v0.8.0`, built from the tag, flashed, hard-reset).

| Gate | Result |
|---|---|
| Boot and network after serial flash | control plane reachable at `192.168.50.101`, clean boot log (`HTTP API ready`, no panic / `ESP_ERR_NO_MEM`), LAN + SoftAP |
| Release identity | published app image reports **`firmwareVersion v0.8.0`** (asserted, not eyeballed) |
| RTC authority across soft reset | boot log `clock seeded from DS3231 (...)`, no OSF, no NVS fallback |
| RTC authority across full power-off | unplugged ~2 min, repowered: `/state` `deltaSec=0` (exact), bus `found:["0x48","0x68","0x76"]` |
| OSF clearing | after seed + reboot no `time never set - clock not trusted`; the write clears status `0x0F` bit 7 |
| DST / `/state` offset stability | 30 rapid `/state` polls all reported the single DST-effective offset (−180); no 1-hour 17:44<->18:44 flicker |
| Bus scan / httpd responsiveness | repeated `/sensors/scan` return all three devices quickly and httpd stays responsive (the earlier rapid-scan wedge is fixed by the 5 s scan cap) |
| Served web assets | decompressed `app.js`/`index.html` confirm the Atlantic relabel, the removed dropdown-revert line, and the Save button |

**Not re-demonstrated this cycle, and not claimed:** the live physical cadence
gate (display path is unchanged from v0.7.1), the media upload/abort/delete
cycle, WebSocket pool and transport badge behavior, sensor calibration/soak, the
`clock_rejected` 409 response after the OSF fix, and the physical swipe/needle
appearance on glass. Those paths are unchanged by this release; this release
touches clock, I2C and web only.

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
