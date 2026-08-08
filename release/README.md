# Prebuilt firmware v0.7.1 — neon marquee refinement

Firmware **`v0.7.1`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Refines the `neon` marquee face added in v0.7.0: the three-ring bulb
border is realigned so every ring shares a top/bottom axis, the accent pairs
are re-anchored (middle ring's pair at the bottom centre, outer ring shifted
off-axis to match the inner ring's pattern), and the marquee readout is
pre-scaled once at scene build so the live blits are plain stride-copies.
Carries everything from v0.7.0 — tube/segments/marquee faces, four colourways,
the embedded Wi-Fi dashboard, the DMA-safe AMOLED display path
(`main/boost_display.c`), and the calibrated GM 12223861 MAP path. The same
files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time rather than a literal. Note that this value is
captured at CMake **configure** time, not build time: a tree that was dirty when
`idf.py` last configured will keep reporting `-dirty` through subsequent clean
builds until `idf.py reconfigure` runs. This release was tagged first, then
built from the clean tagged tree and reflashed, so the board reports a clean
`v0.7.1`.

## What changed since v0.7.0

The marquee border (layout 2) is now three concentric bulb rings at 176/200/224
(`NEON_BULB_RING_STEP` 24): innermost vacuum, middle boost, outermost
overboost. Each ring has its own bulb count (54/66/72, all divisible by 6) so
the CHORD spacing is uniform across rings even though the radii differ. Bulb 0
sits at 12 o'clock and bulb N/2 at 6 o'clock on every ring (`-90°` rotation in
`neon_bulb_pos`), so the top and bottom are radially aligned. Lighting is a
**cumulative stage ladder**: ring z's accent pairs light once the reading has
REACHED that zone (vacuum → inner only, boost → inner+middle, overboost → all
three), anchored per ring at `(i + offset(z)) % 6 < 2` with offset **0/3/2** —
inner pairs at the top centre, the middle ring's pair at the bottom centre
(bulb N/2 at 6 o'clock, partner one dot left), and the OUTER ring's pair two
bulbs off the vertical axis so it flanks top/bottom like the inner ring's
pattern, as close as the different bulb counts allow. Dead bulbs stay dim
`track`, so the two-tone look survives even fully lit. `neonMarqueeSpin`
(persisted) makes the accent pairs CHASE around the rings — one ring advances
every 90 ms round-robin, inner/outer clockwise and middle counterclockwise, a
full 6-phase rotation per ring in 1.62 s; only one ring repaints per step (12
small boxes), deferred on zone-flip frames, inside LVGL's 32-slot invalidation
buffer.

Readout performance: the shared 118 px A8 sprite set is baked ONCE per scene
build at the 0.87 marquee scale (`neon_bake_scaled_sprites`, through the same
LVGL transform the per-frame draw used, so the pixels are identical), and live
blits are plain stride-copy blends. Host A/B measured the per-frame transform
at ~35-40% of every readout repaint; the marquee audit is fully clean (0
severe, 0 stale px). The pre-scaled readout A/B on hardware (fresh boot each
arm, spin + demo on, pixel shift off, 30 s) cut framesOverBudget/s from 33.2 →
21.6 (−35%) and lifted renderFps min 41 → 47.

Min-FPS isolation on the board: the remaining 40s mins are fast-motion seconds
of the demo sweep (peak slew ~9.8 psi/s), not dot cost. A 120 s spin-OFF
capture held min 58 / med 61 (zone-flip seconds never below 58); spin ON
(181 s) dropped min to 45 with 10-13 over-budget cycles/s and 50-61 ms worst
cycles. Each spin step is a render cycle that pays the regionDBuf TE wait (up
to 16.7 ms) when the scan crosses the readout band — the lever is cycle count,
not per-dot raster.

## Verified on hardware for this release

Measured on the board at `192.168.50.102`, running the exact image published
here (clean tree, tagged `v0.7.1`, built from the tag, flashed, hard-reset).

| Gate | Result |
|---|---|
| Boot and network after serial flash | control plane reachable at `192.168.50.102`, clean boot log (`HTTP API ready`, no `task_wdt`/panic/`ESP_ERR_NO_MEM`) |
| Release identity | published app image reports **`firmwareVersion v0.7.1`** (asserted, not eyeballed) |
| Neon cadence, `tools/check_neon_hw.py` | **segments 57, tube 57, marquee 58 FPS median**; counters live, no watchdog output |
| Marquee accent pattern on glass | device debug snapshot at overboost (psi 8.5, Miami preset): all three rings lit, accent residues match the committed macro (ring0 `{0,1}` = vacuum cyan, ring1 `{3,4}` = boost pink, ring2 `{4,5}` = overboost red-orange), dead bulbs dim `track` |
| Serial error absence | no `task_wdt`, panic, or reset markers while the face was driven |

**Not verified this cycle, and not claimed:** the media upload/abort/delete
cycle, WebSocket pool and transport badge behavior, sensor calibration/soak, and
the physical swipe/needle appearance on glass. Those paths are unchanged by this
release, but they were not re-exercised for it.

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
