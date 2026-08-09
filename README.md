# Boost Gauge

ESP-IDF + LVGL boost gauge for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 CO5300 AMOLED).

The live MAP path reads a GM 3-bar sensor through an ADS1115, with an optional BMP280 ambient-pressure reference.

## What you should see

- Full-screen dark cabin gauge (“Pit Lane Night”)
- Dual-climate arc: **teal** vacuum · **amber** boost · **flare red** overboost (default ≥ **8 psi**, configurable)
- Big signed PSI, zone label (`VAC` / `ATMO` / `BOOST` / `OVER`)
- Peak hold; **short tap** resets peak
- **Hold ~1s** toggles the configured max/min brightness levels.
- Vault-Tec supports persisted needle color and counterweight-tail options; red
  changes only the needle body, and the green hub remains unchanged.
- A deliberate vertical swipe cycles themes in dashboard order. Swipe up
  advances (`Dyno Cell` -> `Vault-Tec` -> `Night City` -> `Big Digit` ->
  `Dyno Cell`); swipe down moves backward. Taps and the brightness hold retain
  their behavior.
- The Sport Cluster renderer remains in the source for possible future use but
  is intentionally absent from the selectable theme order.
- Top chip reads `DEMO` until a live sensor path sets `sample.demo = false`
- Samples, the unified-color filled arc, center PSI, and peak hold update every 16 ms (~60 Hz). The physical gauge remains on that cadence while network telemetry is intentionally decoupled from the display loop.

This firmware **replaces** the factory app launcher.

Theme swipes use the ordered `boost_theme_at()` table, which is also the order
emitted by `/api/v1/themes` and consumed by the web picker. The classifier
tracks maximum movement during the press: only movement within the 48 px tap
12 px of movement is the tap slop: only smaller jitter resets peak. Movement
from 12 through 47 px is a rejected drag; a valid predominantly vertical drag
at 48 px or more changes one theme, and horizontal or ambiguous drags do
nothing. Returning to the start after a meaningful drag is still a drag, not a
tap.
Theme changes persist through the active-theme model path and rebuild the LVGL
scene in the existing locked LVGL context.

There is intentionally no crossfade or slide animation. The live gauge uses
partial internal-DMA strips and rich faces cache static art in PSRAM; a smooth
transition would require unsafe full-frame dual-scene storage or an expensive
full-panel repaint that would violate the 16 ms display budget. The bounded
transition is an immediate single-scene swap rather than a misleading effect.

## Layout

```
boost-gauge/
├── CMakeLists.txt
├── partitions.csv
├── sdkconfig.defaults
├── main/                  # firmware source (boost_display.c owns AMOLED bring-up)
├── release/               # prebuilt firmware (flash this)
├── web/                   # dashboard sources embedded into firmware
├── tools/                 # embed_web.py, mock_server.py
└── README.md
```

The merged image resets the complete firmware/partition layout and lays down the
current raw `media` partition. Full flashing migrates old SPIFFS storage to this
layout. For later web OTA updates, upload `boost_gauge.bin`, not the merged image;
app-only OTA does not replace the partition table or media partition.

The `media` partition is at offset `0x820000`, size `0x7E0000`, and is split into
two raw slots. Uploads erase only the required aligned range in the inactive
slot, stream the payload with CRC, and write the committed header last. Boot
scans CRC-valid headers and selects the newest generation; playback uses an LVGL
variable descriptor over the selected slot.
**Symptom of the bug we fixed:** panel stuck half-white / half-green, serial spam:

```
E (...) lcd_panel.io.spi: panel_io_spi_tx_color(...): spi transmit (queue) color failed
E (...) co5300_spi: panel_co5300_draw_bitmap(...): send color data failed
E (...) esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_NO_MEM
```

**Root cause:** the stock Waveshare BSP (`bsp_display_start()` →
`esp_lv_adapter_register_display`) sets `profile.use_psram = true` for LVGL
draw buffers. ESP32-S3 SPI **GDMA cannot stream from PSRAM**, so every flush
allocates an internal DMA bounce buffer and `memcpy`s the strip. Under continuous
partial refresh that path returns `ESP_ERR_NO_MEM` and the panel never finishes a
clean full-frame update.

**Required path in this repo:**

| Piece | Rule |
|------|------|
| Bring-up | Call `boost_display_start()` from `main/main.c` — **not** `bsp_display_start()`. `panel_new()` also replaces `bsp_display_new()` so the QSPI clock is ours |
| Implementation | `main/boost_display.c` / `main/boost_display.h` |
| LVGL buffers | `use_psram = false` (internal DRAM, DMA-capable) |
| Strip height | `BOOST_LVGL_BUF_LINES=20` → 18,640 B per buffer / 37,280 B double-buffer total; selected for responsive web mirroring |
| SPI max transfer | Cap to one strip (`max_transfer_sz = strip bytes`) |
| Queue depth | `CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH=4` in `sdkconfig.defaults` |
| Mutex | `boost_display_lock` / `boost_display_unlock` (thin wrappers over the LVGL adapter) |
| Brightness | `boost_display_set_brightness()` — **not** `bsp_display_brightness_*`, whose static panel handle is unset once we own the panel |

**Do not:**

- reintroduce `bsp_display_start()` for the gauge UI
- set `use_psram = true` on the LVGL draw profile for this board
- raise strip height until double-buffer size no longer fits internal DMA heap
- put full-frame LVGL buffers in PSRAM “for speed” on QSPI CO5300

`sdkconfig.defaults` also keeps Wi‑Fi/LWIP out of the internal DMA pool
(`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`).
That reservation exists so display DMA and Wi‑Fi can run **at the same time**.

Hardware check after flash: serial must show
`boost_disp: ... internal DMA buffers` and **zero** `ESP_ERR_NO_MEM` /
`send color data failed` during a 20–30 s soak while the demo arc animates.

### Gauge render contract (do not regress)

The live display is intentionally **marker-free**. `boost_gauge_update()` updates the
filled `s_arc_value` and center readout on every 16 ms sample; no timer divider is
permitted on either path.

| Invariant | Why it matters |
|---|---|
| Move only the changing arc endpoint between zero crossings | Re-setting the stationary endpoint also invalidates it; this creates avoidable multi-strip flushes. |
| Keep `LV_PART_KNOB` removed | The gauge has no marker/knob; avoid restoring a rendered knob. |
| Do not replace the arc with many `lv_line` segments | Tested on hardware: retained segment objects did not improve cadence and add persistent render work. |
| Update labels only when their formatted text/color changes | Avoids unnecessary text rasterization, while preserving 16 ms readout responsiveness. |
| Preserve LVGL partial mode and the even/odd CO5300 rounder | Full refresh turns a local wedge into a 434 KB full-panel transfer. |

The live gauge remains a 16 ms (~60 Hz) physical path. The verified cadence
check recorded a minimum of **61 FPS** and a median of **63 FPS** over **112
samples**. Use it after every display-path change:

```bash
python3 tools/check_display_cadence.py --url http://<BOOST_WEB_IP> --seconds 30
```

This guard defends the physical gauge path; it is not a network telemetry or
GIF playback benchmark.

Do not reintroduce a marker, throttle the filled arc/readout, or alter the
invalidation callback without a before/after hardware measurement.

### Measured strip-height boundary

The original 20-line setting is the production configuration. We tested larger
internal-DMA strips with Wi-Fi enabled and the full-rate physical gauge running:

| Buffer height | Internal DMA draw-buffer allocation | Result |
|---:|---:|---|
| **20 lines** | **18,640 B per buffer; 37,280 B double-buffer total** | **Selected.** Physical gauge: 56–63 FPS. Sequential browser-style polling: p50 9.5 ms, p95 19.0 ms. |
| 24 lines | 22,368 B per buffer; 44,736 B double-buffer total | Physical gauge remained 56–63 FPS; browser-style polling p50 9.2 ms, p95 16.4 ms. No measured physical-FPS gain. Reverted because the live mirror was reported subjectively laggier. |
| 28 lines | 26,096 B per buffer; 52,192 B double-buffer total | HTTP became unresponsive under load. Rejected. |
| 40 lines | 37,280 B per buffer; 74,560 B double-buffer total | Wi‑Fi route disappeared after flash/load. Rejected. |

The 24-line test did **not** improve measured physical FPS. The browser mirror is
independently limited by its network cadence and render loop. WebSocket telemetry
targets **62.5 Hz**; the browser canvas interpolates between samples.
When WebSockets are unavailable, the HTTP fallback polls at **4 Hz**. Treat
perceived mirror responsiveness as a web telemetry or canvas-rendering issue, not
a display-strip throughput win. Do not raise the strip height without both
physical-FPS and browser-mirroring measurements.

### QSPI clock: 80 MHz, owned by this repo

`BOOST_LCD_PCLK_HZ` in `main/boost_display.c` sets the CO5300 QSPI clock to
**80 MHz**, against the driver's 40 MHz default.

An earlier revision of this section claimed a "60 MHz trial" was active. It was
not, and never had been: the clock lives in `CO5300_PANEL_IO_QSPI_CONFIG` inside
a managed component, `bsp_display_new()` takes that default with no override
hook, and any edit to `managed_components/` is silently reverted by a dependency
refresh. Everything measured before this change was running at 40 MHz. **Do not
reintroduce the clock there** — that is precisely how the repo came to document
a setting that was not in effect.

The bring-up is therefore vendored: `panel_new()` in `boost_display.c` is
`bsp_display_new()` with `pclk_hz` under our control, plus a copy of the
14-entry vendor init table. Consequence to know about: `bsp_display_brightness_*`
writes through a file-static panel handle that only `bsp_display_new()` assigns,
so it returns `ESP_ERR_INVALID_STATE` once we own the panel. Brightness is now
`boost_display_set_brightness()`, the same single 0x51 command.

Measured on hardware, worst render cycle:

| | 40 MHz | 80 MHz |
|---|---:|---:|
| Big Digit (full-screen recolour) | 45.1 ms | **37.6 ms** |
| Dyno Cell | 43.2 ms | 41.7 ms |
| Night City | 36.8 ms | 37.3 ms |

The gain lands **only on full-frame pushes**. Partial-update faces are bound by
CPU rasterisation with dirty regions far too small for the link to matter, and
move 0-3%. Treat this as a fix for one operation, not a general lever. Boot must
log `panel up at 80 MHz QSPI` with no `co5300` errors; if the panel ever shows
sparkle or torn rows, drop back to 40 MHz first.

GIF playback is an exclusive, decoder/renderer-bound path; the live-gauge
cadence guard does not apply while media is active.

## Fast path: flash prebuilt (no ESP-IDF)

A verified **v0.3.2** build (firmware `0.3.0-web`, ESP-IDF **5.5.1**, app size ~1.38 MB) is available in [`release/`](release/) and on the [latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

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
### Cadence and connection contract

The physical gauge samples and updates every **16 ms (~60 Hz)**. Network
telemetry is deliberately lower-rate and independently owned: the WebSocket
server maintains a fixed pool of **3 clients**, rather than a single dashboard
owner. Each client may have at most one in-flight, heap-owned frame; the server
performs a bounded broadcast at **62.5 Hz**. A newly connected dashboard MUST NOT
close or evict an existing socket, because concurrent or stale tabs otherwise
force Live/Fallback churn and can produce out-of-order target jitter.

The browser sends an application heartbeat every **750 ms**; the server consumes
that heartbeat as part of the WebSocket protocol. If the socket is unavailable,
the browser uses HTTP `GET /api/v1/state` fallback at **4 Hz**. The connection
badge exposes the active path as **Live · WebSocket 60 Hz** or
**Live · HTTP 4 Hz**; **Disconnected** means neither transport is delivering
state. The canvas renders every `requestAnimationFrame` and uses a short **35 ms**
EMA to suppress packet-step jitter without adding the previous 90 ms visual lag.
It accepts only targets with a strictly newer `uptimeMs`; after a gap greater
than **1 s**, its timing state resets. These rules keep target ordering and visual
smoothing separate from packet cadence.

The browser canvas interpolates at **60 FPS**, while the sparkline remains
intentionally **4 Hz**. These are separate contracts: a smooth browser canvas
does not imply 60 Hz network packets, and GIF playback is exclusive to the
display path.

Background history logging is a separate **12.5 Hz** producer: the 1,800-sample
RAM ring retains **2 minutes 24 seconds** regardless of whether the dashboard is
receiving 62.5 Hz WebSocket telemetry or 4 Hz HTTP fallback.

The ring is **43,200 bytes and lives in PSRAM**, allocated once in
`boost_model_init()` with `MALLOC_CAP_SPIRAM`. It is written at 12.5 Hz and read
only for export - no DMA, no ISR, not latency-critical - so it has no business in
internal DRAM, which is shared with Wi-Fi and display DMA. A failed allocation
leaves the pointer NULL and disables logging rather than failing boot; every
access stays under the existing `s_lock`.

### Fourth-client behavior

The firmware owns a fixed pool of **3 WebSocket clients**. A fourth handshake is
rejected/closed for that newcomer only; the three existing sockets stay open and
continue receiving telemetry. The browser keeps its fallback poll at **4 Hz**
(`POLL_FRAME_MS = 250`) and retries WebSocket connection every **1 s**. A
successful `/api/v1/state` fallback sample shows **Live · HTTP 4 Hz**;
**Disconnected** means both WebSocket and HTTP state polling are unavailable.
A retry attempt must not downgrade healthy fallback, and a restored socket shows
**Live · WebSocket 60 Hz**.

Slots are returned to the pool through a single `state_ws_release_locked()`,
which clears `fd`/`payload`/`inflight` together and bumps a per-slot generation
counter. The generation is what makes a slot safe to reuse while an async frame
for the previous occupant is still queued in the httpd task: the late completion
no longer matches the slot, so it frees only its own buffers. Never release a
slot by clearing `fd` alone - the completion callback can then never match, and
`inflight` stays set, which permanently removes that slot from the pool.

### Layered GIF pipeline (decoder ownership)

This is a hybrid pipeline, **not a project-written GIF decompressor**:

- **Custom upload/storage:** `web/app.js` uploads to `main/boost_web.c`; the
  request is streamed into the raw dual-slot CRC/committed-header store in
  `main/boost_media_store.c/.h`. The selected payload is mapped with
  `esp_partition_mmap`.
- **Custom gauge integration:** `main/boost_gauge.c/.h` runs under the display
  lock, hides the gauge, creates and centers a black-backed `lv_gif` widget,
  selects RGB565, and gives it an `lv_image_dsc_t` whose data points at the
  mapped GIF bytes. The load path maps before creating/feeding the descriptor.
- **Third-party decode/playback:** LVGL's `lv_gif` wrapper in
  `managed_components/lvgl__lvgl/src/libs/gif/lv_gif.c` calls bundled
  AnimatedGIF (`AnimatedGIF/src/gif.c`) via `GIF_openRAM` and `GIF_playFrame`.
  LVGL's timer supplies frame timing; parsing, LZW decode, and
  delta/disposal composition remain in those components.
- **Intentional local LVGL changes:** `lv_gif.c` zero-initializes the full
  framebuffer to prevent stale AMOLED strips and forces `pTurboBuffer = NULL`
  so the standard decoder composes delta/disposal frames correctly; this does
  not make decompression custom. The framebuffer is the full canvas.

The mapped partition data must remain valid until the widget is destroyed. The
shutdown order is display lock → destroy `lv_gif` widget → unmap partition data.
The flush path remains the custom internal-DMA partial CO5300 path in
`main/boost_display.c`; it is separate from GIF parsing and decoding.

***

### Network modes

| Mode | When | URL |
|------|------|-----|
| **APSTA** | NVS/secrets STA SSID set | SoftAP + LAN STA · serial `BOOST_WEB_IP=` |
| **SoftAP only** | No STA SSID | Join `BoostGauge-XXXX` / `boost1234` → `http://192.168.4.1/` |

**Settings page** (`/settings.html`): Cockpit navigation uses the gear icon; settings is a real document rather than a show/hide panel, so browser back/forward works normally. It owns Wi-Fi controls (mode, SSID, password with blank-keep, scan, reconnect) and gauge fields `psiMin` / `psiMax` / `psiOverboost` (defaults **−15 / 10 / 8**) plus `zeroAngle` (default **236.25°**, allowed **180–315°**). Zero position moves the dial notch without changing sensor pressure; vacuum and boost rescale on their own sides. The optional boost-half midpoint label is omitted when it would overlap the overboost label. Invalid range PUTs are rejected with **400**. Settings persist in NVS (`boost_wifi` for network; boost config blob for gauge scale).

**Seed credentials** (optional, gitignored): `main/boost_wifi_secrets.h` from
`main/boost_wifi_secrets.h.example`. Used only when NVS has no Wi‑Fi blob yet.

```bash
cp main/boost_wifi_secrets.h.example main/boost_wifi_secrets.h
# edit SSID/password once, then:
idf.py build flash monitor   # look for BOOST_WEB_IP=192.168.x.y
```

### Dashboard notes

- Responsive **instrument-cluster** layout: sticky gauge + sparkline on the left; cockpit console cards reflow from one column (mobile) up to three (ultrawide, capped at 2100&nbsp;px). The cockpit’s gear opens the separate `/settings.html` document for Wi-Fi and gauge range; browser history returns to the unchanged live cockpit. No horizontal overflow at any width.
- Mobile: `overflow-x: hidden`, no horizontal rubber-band empty space
- Dim schedule Start/End stay side-by-side; time inputs capped for iOS Safari
- Brightness/theme/schedule apply off the HTTP worker so the UI stays responsive
- Sensor/model/WebSocket publication runs outside the LVGL worker, so GIF playback cannot stall dashboard telemetry. Network telemetry is decoupled from the physical 16 ms gauge loop. Station Wi-Fi modem sleep is disabled while the gauge runs; this favors live-control latency over Wi-Fi power saving.
- GIF playback is exclusive. A native **466×466** clip fills the AMOLED; smaller clips remain at their native dimensions, centered on a pure-black AMOLED background. Upload accepts sources up to **466 × 466 px**.
- Browser rendering caps device pixel ratio (DPR) at **2**; the sparkline is
  limited to **4 Hz**, and the browser GIF preview is disabled. In the verified
  30 s dashboard soak, the main-thread probe peaked at **9 ms** with no freezes
  longer than **500 ms**.
- **Shared error box lifetime.** `#errorBox` is written by two kinds of producer
  and each retracts only what it raised. `showError(msg, source)` /
  `clearError(source)` take `ERR_LIVE` (unattended telemetry: `pollState`,
  WebSocket frames) or `ERR_USER` (the outcome of a gesture). `ERR_USER`
  outranks `ERR_LIVE` in both directions, so a poll can neither overwrite nor
  erase the message an operator is reading; a `showOk()` or a click on the box
  dismisses it. A transport error raised by `pollState` is `ERR_LIVE` and still
  self-clears the moment polling recovers. The connection badge remains the
  designated live-transport indicator and this path never touches it. This is
  the **only** convention for the shared box &mdash; do not add per-panel status
  elements to work around it (`#calStatus` predates the fix and stays for its
  own reason: a two-second measurement's verdict belongs beside its readouts).
### Clock source, persistence, and CSV timestamps

The dashboard's **Sync Time** control is the only time-synchronization action:
its `POST /api/v1/time` supplies browser `Date.now()` plus the configured UTC
offset. Firmware applies the epoch with `settimeofday` and saves the epoch plus
the monotonic checkpoint and configuration in NVS. On reboot it restores that
checkpoint and advances it only by the monotonic delta when applicable. This
path performs no RTC or NTP resynchronization, so merely opening the dashboard
does not sync the device; use **Sync Time** explicitly.

CSV `timestamp_local` is formatted as `%Y-%m-%dT%H:%M:%S` with no timezone
suffix. `utc_offset_minutes` remains a separate CSV field. Hardware CSV
verification produced **1,800 rows**, `badTimestampCount: 0`, and offset
`-300` minutes.
***

### GIF upload behavior

The `media` partition is a raw dual-slot store at offset `0x820000`, size
`0x7E0000`. Each upload targets the inactive slot: firmware erases only the
required aligned range, streams the GIF payload while computing its CRC, and
writes the committed header last. On boot, the store scans CRC-valid headers and
selects the newest generation. Playback uses an LVGL variable file descriptor
over the selected slot; there is no file-copy activation step.

The full **1,379,129-byte** GIF upload completed in **7.504 s** on hardware.
Because the previous slot is not replaced until the new header is committed, an
aborted upload preserves the previous GIF. Two repeated deletes succeeded; after
deletion the physical gauge resumed and PSI changed.

### Animation performance contract

GIF playback is an exclusive LVGL path. A 466×466 RGB565 frame is about
**434 KB**. The transfer cost of a strip is **linear in its size**, measured by
sweeping transfer sizes from 932 B to 37,280 B at three clocks (40 reps each,
completion-to-completion deltas, no chunking boundary anywhere in that range):

| clock | slope | link rate | vs theory | fixed intercept |
|-------|-------|-----------|-----------|-----------------|
| 80 MHz | 0.02539 µs/B | **39.4 MB/s** | 98.5% | 106.3 µs |
| 40 MHz | 0.05041 µs/B | 19.8 MB/s | 99.2% | 108.2 µs |
| 20 MHz | 0.10045 µs/B | 10.0 MB/s | 99.6% | 115.1 µs |

The slope halves exactly with the clock while the intercept does not move, which
is what separates the two costs: the bus itself runs at essentially the full
arithmetic rate, and there is a **clock-independent ~106 µs of software overhead
per transfer** — three separate blocking calls (CASET, RASET, RAMWR), each with
its own bus acquire/release. The raw command bits account for under 1 µs of it.

A full frame is 24 transfers (23 × 20 lines + one 6-line remainder), so the
transfer-only floor is **13.6 ms (≈74 FPS)**, before GIF decoding and LVGL
rendering. Of that, 11.0 ms is pixel data and 2.5 ms is per-transfer overhead.

The clock is confirmed at the requested rate two independent ways:
`spi_device_get_actual_freq()` returns exactly 80000/40000/20000 kHz for the
three requests, and the measured throughput tracks theory to within 1.5%. The
pins are entirely GPIO-matrix routed (PCLK=GPIO38 is not an SPI2 IOMUX pin), but
that costs nothing here — the IOMUX-vs-matrix frequency penalty is **ESP32-only**
and does not apply to the S3 at or below 80 MHz.

> **Three earlier revisions of this paragraph were wrong**, each in a way the
> next one failed to catch. The first two quoted arithmetic as though it were a
> measurement — once for a 60 MHz clock that was never active (see the ledger row
> "Documented a setting that was never in effect"), then for 80 MHz. The third
> over-corrected: it reported a genuinely-taken but unreproducible measurement of
> 999 µs per strip / 18.7 MB/s / 23.3 ms per frame, and built a "the hardware
> does not deliver the arithmetic" conclusion on top of it. That figure does not
> reproduce — the same strip size at the same clock and queue depth measures
> 575.9 µs, consistently, under both quiet-boot and fully-loaded conditions. The
> original measurement was never committed as inspectable code, only described in
> a commit message, so the cause could not be found. **The lesson is not "measure
> instead of calculating" — it is that a measurement nobody can re-run is not
> evidence.** The table above comes from a harness that was committed before its
> numbers were quoted.

This does not change the conclusion that the QSPI clock is not the lever for
partial-update faces — bus utilisation on a needle update is a few percent, far
from the constraint. The ~106 µs intercept is the only part worth attacking, and
only for full-frame work.

The uploaded 466×466 fixture (`IMG_5325-ezgif.com-optimize (2).gif`) is
1,379,129 bytes, 101 frames, 3.37 seconds, and nominally 30 FPS. On the board
it measured **14–20 physical renders/s** (median 20) with no serial display,
memory, panic, or reset errors. That is decoder/renderer-bound rather than a
panel-transfer failure.

Do not use `check_display_cadence.py` while GIF media is active: its ≥60 FPS
threshold defends the live gauge path, not full-frame animation playback.

The upload and delete operations are cancellation-safe. Clicking **Delete**
during an upload aborts the browser XHR, waits for that request to settle, and
then sends `DELETE`. The server rejects overlapping delete/upload operations
with **409 Conflict**, preventing the delete from racing an in-progress slot
commit.

For smoother animation, reduce frame rate, palette complexity, changed-pixel
area, or use a purpose-built low-area LVGL animation. Native 466×466 GIFs avoid
additional scaling work, but still need to fit the decode/render budget.

### Fast-motion cadence on vault-tec (regionDBuf ON)

The whole-run `dyno-cell`/demo cadence guard (min ≥60) averages over a demo
sweep whose slew rate varies continuously, which hides exactly the regime a
user actually watches the needle react to: the fast segments. Isolated with
`tools/bench_fast_motion.py` (two independent methods — a controlled
constant-slew triangle sweep, and empirical velocity correlation against the
unmodified organic demo waveform; both committed, both reproducible):

| Regime | renderFps median | worstRenderUs median | framesOverBudget/s |
|---|---:|---:|---:|
| Controlled sweep, regionDBuf ON, 9.79 psi/s sustained | 59 (min 52) | 31,684 | 1 |
| Controlled sweep, regionDBuf OFF (tears — reference only) | 60 (min 58) | 21,322 | 1 |
| Organic demo, fastest quartile of seconds (~8 psi/s) | 41.5 (min 35) | 35,044 | 21.5 |
| Organic demo, slowest quartile of seconds (~0.7 psi/s) | 55.0 (min 33) | 29,216 | 6.5 |

Root cause: `te_wait_for_region_spans()` (`main/boost_display.c`, was
`te_wait_for_region()`) decides whether a burst can skip its TE wait from the
**union** of `top_row`/`bottom_row` across *every* span in the cycle, not
each span's own range. The vault needle and the six digit-slot labels sit
far apart on screen and both change on almost every 16 ms tick once psi is
moving at all — during fast motion this is true on essentially every cycle,
so the combined row range is wide almost every time, which makes the
early/late proof fail more often and forces a real wait. When that wait
lands after ~13.6 ms of rasterisation has already elapsed, the cycle's total
cost roughly doubles to a second TE period, which reads as a stutter, not a
rate reduction.

**Since fixed and shipped** (branch `spike/fast-motion-cadence`): the wait
proof now scores each span's own row range independently instead of the
whole cycle's union, provably at least as safe as the version it replaces
(see the block comment on `te_wait_for_region_spans()` for the exact
argument, including where it is and isn't a strict superset of the old
proof). Verified with an interleaved hardware A/B (rebuild + reflash between
every arm, 3 rounds): `teSkips` up ~6% on average and `framesOverBudget` down
~3%, a real if modest improvement to the underlying mechanism — but
`renderFps`/`worstRenderUs` did not move outside run-to-run noise in either
isolation method, so this does **not** read as a fix for the perceived
stutter. Most fast-motion cycles are apparently dominated by a single tall
span (the needle alone) or adjacent spans, where the new per-span proof is
mathematically identical to the old union one; the "needle and a far-away
digit label invalidate together with a wide gap between them" case this
targets is real but evidently not the dominant cost. The `dyno-cell`/arc
guard is unaffected (min 56-58/median 60, matching the pre-existing noise
band). See the AGENTS.md ledger for the full numbers, the bug an early cut
of this fix had (using the scan's row time instead of the write's own,
which caused a real measured regression before being corrected), and why a
RAMWRC-based per-chunk command reduction is still not shipped.


**Needle raster cost fixed:** the Vault-Tec wedge was submitted as three
triangles even though two already tile its convex trapezoid exactly; the third
triangle lay wholly inside that union. LVGL's software triangle renderer still
allocated/applied three edge masks and blended every covered scanline for the
redundant task. Removing it preserves the tapered base, blunt tip, colors, hub,
and dirty-region geometry. A same-firmware runtime A/B isolated only that task:
two triangles delivered median **60/59 FPS**, raster p50 **8.25/9.58 ms**, and
**7/17** over-budget frames per 19-second run; restoring the third delivered
**56/55 FPS**, **9.50/10.84 ms**, and **104/116** over-budget frames. After
removing the temporary profiler, the production build measured median **60 FPS**
(min 58), `worstRenderUs` median **17,949 us**, and 8 total over-budget frames
in the controlled sweep. The organic fastest quartile improved from the
historical **41.5 FPS / 21.5 over-budget frames** to **55 FPS / 6** in the final
90-second cross-check, though its single min-21/max-118.9 ms outlier shows the
tail remains noisy. Fixing the hundredths digit at `0` was also measured: it
cut transferred pixels by about 10% but did not improve cadence, because the
tenths digit still changes on nearly every fast-sweep frame. That display
shortcut is not shipped.

### Host-only UI development

```bash
python3 tools/mock_server.py --host 127.0.0.1 --port 18080
python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
```

`mock_server.py` stubs the full dashboard API — including `/api/v1/network`,
`/network/scan`, `/network/reconnect`, and config fields `psiMin`/`psiMax`/
`psiOverboost`/`zeroAngle` — so `refreshAll()` succeeds and cockpit + settings
views render host-only (it has no WebSocket, so the badge falls back to `Live · HTTP 4 Hz`).

Do not expose SoftAP/STA HTTP or OTA beyond a trusted LAN; no per-request auth yet.

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

### Two-page layout and gestures

`main/boost_page.c/.h` owns two persistent pages: page 0 is the boost gauge and
page 1 is the TPMS view. On the boost page, swipe **LEFT** to TPMS; on TPMS,
swipe **RIGHT** to return to boost. There is no wrap-around: outward swipes at
either end are ignored. Vertical theme swipes work only on page 0, and a tap
resets peak only on page 0. A one-second hold toggles brightness on either
page. Pointer-device events own the hold deadline independently of target jitter,
and the brightness state is committed only after the panel command succeeds. The
page coordinator forwards MAP samples only to the boost scene
and TPMS snapshots only to the active TPMS scene.

### TPMS

`main/boost_tpms_ui.c/.h` renders the processed 466x466 powertrain art on black,
with four tire-shaped status capsules (`FL`, `FR`, `RL`, `RR`) aligned to the
80%-scale chassis. The physical readouts use the compiled Saira SemiCondensed
Bold face and show number-only PSI values: green is normal, red is low, amber is
stale, and gray is offline. `STALE` retains its last number; a wheel that has
never reported shows `--.-`. The browser uses the same native-466 geometry and
redraws when the image finishes loading.

The framework is split into `main/boost_tpms.c/.h` (service/model), the
deterministic `main/boost_tpms_mock.c/.h` provider (`NORMAL`, `STALE`, and
`DISCONNECTED` scenarios), and the pure `main/boost_tpms_protocol.c/.h`
protocol/conversion/ISO-TP layer. For the 2019/2020 Mazda MX-5 ND, UDS
`ReadDataByIdentifier` uses ECU/header `0x720`, with DIDs `FL=0x2A05`,
`FR=0x2A07`, `RL=0x2A06`, and `RR=0x2A08`. Conversion is
`pressure_kPa = raw * 1.373 + additive replacement-sensor offset`, then
`pressure_psi = kPa * 0.145037738`.

BLE central support is feature-flagged off by default (`BOOST_TPMS_BLE_ENABLED=0`)
pending the vLinker FD+ adapter. ESP32-S3 is BLE-only, not Classic/SPP, so the
adapter must expose a BLE GATT service. Its UUIDs are unpublished; firmware
must discover services and characteristics at runtime and require a verified
adapter profile before sending vehicle commands.

The simulator snapshots one TPMS scenario with:

```bash
./sim/build/boost_gauge_sim.exe --tpms [normal|stale|disconnected]
```

The default `--screenshot` mode also emits `tpms_*.raw` for all three scenarios.
On hardware, `main/main.c` calls `boost_tpms_init()` / `boost_tpms_start()`, sets
the mock scenario, and runs a 250 ms LVGL timer that ticks the mock provider and
feeds `boost_page_update_tpms()`.


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

## Theme system

A theme is no longer just a palette swap — each theme carries a **`style`** that
selects a **distinct gauge layout**. Selecting a theme changes the whole face,
not only its colors, and the selection persists in NVS across power cycles.

| Theme id | `style` | Face |
|---|---|---|
| `dyno-cell` | `arc` | The classic dual-climate arc (teal vacuum · lime boost · flare overboost). Default. |
| `vault-tec` | `vault` | Fallout-style phosphor **needle dial** with CRT scanlines/vignette, a peak tell-tale marker, and an overboost alert (warm numeral + blinking `OVER-PRESSURE`). |
| `night-city` | `hud` | Cyberpunk **targeting HUD**: hazard chevrons, Kiroshi reticle around a big italic value, glitch-shear on fast spikes, MAP/PEAK telemetry. |
| `big-digit` | `bigdigit` | A huge **Alvida Fatface** PSI number in white on a ground that sweeps cyan → lime → red with the reading. |
| `neon` | `neon` | Neon-tube face in **SF Alien Encounters**: a glowing readout over one of three selectable layouts, in one of four colour presets. |

### Neon: layouts and presets

Unlike the other themes, `neon` is one theme with two extra settings rather
than several theme entries. Both persist in NVS and both are exposed on
`PUT /api/v1/themes/config`.

- **`neonLayout`** (0–2) picks the face: `0` tube (one continuous arc), `1`
  segments (54 discrete segments, the default — smallest dirty region), `2`
  marquee (linear bar with a three-ring bulb border — innermost vacuum,
  middle boost, outermost overboost — and no value ring). The three rings sit
  at 176/200/224 (`NEON_BULB_RING_STEP` 24, 1.5x the first spread) so the
  shared 118 px readout draws scaled to 0.87 (one sprite set, `neon_mq()`
  scaling; see below). The marquee bakes that 0.87-size A8 sprite set ONCE at
  scene build (`neon_bake_scaled_sprites`, through the same LVGL transform
  the per-frame draw used, so the pixels are identical), and the live blits
  are plain - host A/B measured the per-frame transform at ~35-40% of every
  readout repaint, and the marquee audit is now fully clean (0 severe,
  0 stale px). Each ring's bulb count is chosen for UNIFORM chord
  spacing — inner 54, middle 66, outer 72 (`NEON_BULB_N_INNER/MID/OUTER`, all
  divisible by 6 so the 2-lit/4-dark accent pattern wraps seamlessly). The border is a **cumulative stage ladder**: ring z's
  accent bulbs (anchored per ring, `(i + offset(z)) % 6 < 2` with offset
  0/3/0: inner and outer pairs share the top centre, the middle ring's pair
  sits at the bottom centre with bulb N/2 at 6 o'clock and its partner one
  dot left) light once the
  reading has REACHED that zone (vacuum → inner only, boost → inner+middle,
  overboost → all three); dead bulbs stay dim `track`, so the two-tone look
  survives even fully lit. `neonMarqueeSpin` (persisted) makes the accent
  bulbs CHASE around the rings — one ring advances every 90 ms, round-robin,
  inner/outer clockwise and middle counterclockwise, a full 6-phase rotation
  per ring in 1.62 s. The chase only repaints one ring per step (12 small
  boxes) and defers to zone flips, so it stays inside LVGL's 32-slot
  invalidation buffer and the face keeps ~1 Mpx/s. Pre-scaled readout A/B on
  hardware (fresh boot each arm, spin + demo on, pixel shift off, 30 s):
  framesOverBudget/s **33.2 → 21.6 (−35%)**, renderFps median **55 → 57**
  (min 41 → 47), `worstRenderUs` median 43.0 → 42.0 ms / max 63.0 → 55.7 ms.
  The median worst stays ~42 ms because the worst cycles are TE-wait + merged
  full-region bound, not sprite bound; the win shows up as fewer over-budget
  frames and a better tail. Min-FPS isolation on the board: the 40s mins are
  fast-motion seconds (demo sweep peak slew ~9.8 psi/s), not dot cost — a
  120 s spin-OFF capture held min **58** / med 61 (zone-flip seconds never
  below 58), while spin ON (181 s) dropped min to **45** with 10-13
  over-budget cycles/s and 50-61 ms worst cycles. Each spin step is a render
  cycle that pays the regionDBuf TE wait (up to 16.7 ms) when the scan
  crosses the readout band, so the lever is cycle count (spin cadence /
  heavy-tick deferral / the stubbed TE scanline), not per-dot raster —
  caching the accent dots would not help, they are already the cheapest
  primitive. First marquee scene build is ~513 ms (the scaled
  bake adds ~66 ms over the plain glyph bake); cached returns ~172 ms.
  Invalidation is per-glyph, not per-slot: `neon_cell_x_span`/`neon_sign_x_span`
  ask the baked sprite for its own footprint (marquee: the pre-scaled tile's
  `bbox_s` at the scaled anchor; tube/segments: the full-size bbox at `spr_dx`)
  and REPLACE the uniform label box with the tile's exact extent (+1 px AA
  margin), unioning the old and new glyph footprints when a cell's occupant
  changes. The marquee bar invalidates only when its DRAWN pixel extent or the
  zone colour changed, so a static reading goes idle; the live accent scan is
  skipped when the dirty region cannot reach the rings. Host audit, 25 s, all
  four variants: **0 severe / 0 stale px**. Tube/segments flushed
  px/cycle **19,753 → 18,397 (−6.9%)**; marquee unchanged at 14,848 (spin
  16,441). Hardware cadence/fast-motion re-run still pending for the locked-60
  claim.
- **`neonPreset`** (0–3) picks the colourway: `0` Violet, `1` Miami, `2` Toxic,
  `3` Blood Moon. Presets set `track`/`muted` as well as the three zone
  colours, so changing one repaints the cached background.

| Preset | vacuum | boost | overboost |
|---|---|---|---|
| Violet | `#7B00FF` | `#FF2BD6` | `#FF1500` |
| Miami | `#00E5FF` | `#FF2BD6` | `#FF2A00` |
| Toxic | `#39FF14` | `#FFF000` | `#FF00A0` |
| Blood Moon | `#0064FF` | `#C4172E` | `#FF6A00` |

Blood Moon deliberately pairs a crimson boost with an orange overboost —
adjacent warm hues, and the narrowest post-bloom separation of any preset here
(77 units, against 176 for the yellow overboost it replaced). Still separable
by hue, but it is the one preset where overboost has little peripheral-vision
margin; overboost is the knob if it ever reads ambiguous.

**Palette entries are the base the bloom is derived from, not what you see.**
Everything lit goes through `neon_lit()`: saturate ×1.30 about luma, gain
×1.92, then overflow past full scale desaturates toward white
(`NEON_WHITE_LIFT`). Two earlier overflow rules each failed in a way worth not
repeating — clamping per channel pinned every saturated entry to the same
corner of the colour cube and made the three zones converge; scaling the whole
vector back to peak 255 preserved hue but handed back all the gain, so the
bloomed body came out *darker* than the raw palette behind it and the ring's
band structure inverted in 11 of 12 zones. The ring's inner band is a *dimmed*
zone colour (`NEON_HALO_DIM`), which is what guarantees the bands read
dark → bright → white outward regardless of palette.

Readout glyphs, the minus mark and the zone word are baked once at scene build
as **A8 coverage tiles** with a box-blurred glow, then blitted with a recolor.
Because coverage carries no colour, one set of tiles serves every zone and
every preset, and both those tiles and the painted background survive theme
switches (keyed on layout, and on `neon_bg_key_t` respectively) — without that
memoisation, entering `neon` cost ~350 ms against 45–100 ms for other themes.

Shared rules across styles:

- **Any filled arc references the configured zero.** Vacuum and boost scale
  independently and the fill grows from the zero notch, honoring the
  settings-page `zeroAngle` (Dyno Cell, Vault-Tec, and Night City all obey it).
- **Readouts use fixed decimal positions.** The `big-digit` value is fully
  tabular (constant-width digit cells, decimal pinned to face center); the
  decimal, ones, and tenths never move, and higher digits/sign grow leftward.
- **The web dashboard chrome does not re-skin with the gauge.** `setTheme()`
  drives only the gauge palette (`state.palette`); the console keeps one fixed
  identity.

### Where it lives

- **Web mirror:** `web/app.js` dispatches `drawGauge()` on `activeThemeStyle()`
  to the active renderer. `tools/mock_server.py` serves the four selectable
  themes (each with a `style` field); the dormant Sport renderer is retained but
  not advertised. Regenerate embedded assets after any web edit. `setTheme()`
  drives only the gauge palette, so the dashboard chrome keeps one fixed identity.
- **Dormant Sport renderer:** if it is restored to `s_defaults[]`, its static
  ring/ticks remain cached in a PSRAM `lv_canvas` and its live readout remains
  bounded rather than repainting the full face.
- **Simulator:** `sim/` builds the same `boost_gauge.c` on the host and renders
  headless PNGs, including the generated fonts, so a face can be checked without
  flashing. SDL2 is optional (only `--window` needs it); `--theme <id>` selects
  the style, `--neon-layout tube|segments|marquee` and `--neon-preset 0..3` the
  neon face and colourway, and `--neon-spin` enables the marquee chase (and
  `--neon-chase DIR` writes a fixed-psi 90 ms/step chase sequence to DIR for
  preview GIFs). Both of those exist because the sim does **not**
  inherit the device's persisted settings: it calls `boost_theme_init()` at
  startup, and without these flags it renders whatever the defaults say. Before
  `--neon-preset` existed the sim never applied a preset at all
  (`apply_neon_preset()` runs from `boost_theme_init()`, which the sim did not
  call), so every screenshot showed the compiled-in palette regardless of which
  preset was selected — screenshots agreed with the device only by coincidence. This is the fastest verification loop — use it before burning a
  flash cycle.
- **Font:** `main/fonts/alvidafatface-regular.otf` (OTF kept over the TTF — CFF
  master, ~4× smaller, converts cleanly with `lv_font_conv`). The dashboard loads
  it as an inlined base64 `@font-face` (`"Alvida Fatface"`) prepended to
  `web/styles.css`.

### Firmware architecture

`main/boost_gauge.c` is a **scene dispatcher**, not a single face:

- `build_scene(style)` / `destroy_scene()` construct and tear down a per-style
  LVGL object tree. `boost_gauge_update()` dispatches to the matching
  `update_*()`. `s_built_style` records what is currently on screen.
- **Changing theme rebuilds the scene** rather than recolouring, because each
  style is a different object tree. `PUT /themes/active` therefore has to call
  `boost_gauge_apply_theme()` under the display lock — without it the picker
  only took effect after a reboot.
- The `arc` face keeps its original geometry and wedge-invalidation logic
  verbatim; it is the path the 60 FPS cadence guard was established against.

**Zero reference.** `psi_to_angle()` maps the arc face. `psi_to_sweep(psi, a0, a1)`
projects the same zero-referenced scaling into any other sweep, so Vault-Tec and
Night City honour the configured `zeroAngle` and scale vacuum/boost
independently, exactly like the arc.

**Draw/invalidate must agree.** Every moving element draws from a *committed*
value that is only advanced when the element is invalidated
(`s_vault_needle_deg` / `s_vault_needle_over`, `s_hud_fill_deg` /
`s_hud_fill_psi`). Drawing from the live sample while invalidating on a
threshold leaves stale pixels — that was the source of the smearing artifacts.
The needle also repaints on an overboost **colour** flip, not only on angle
change.

**Clip guard.** `clip_reaches_radius(layer, r)` reads `layer->_clip_area` (its
documented use during draw-task creation) and lets a face skip its outer ring
art when only the centre is dirty — the common per-frame case, since digit
updates cannot touch a tick ring at r >= 194. The Vault needle is deliberately
shorter than the tick ring so its dirty rect stays inside the guard.

**Cost model (measured on hardware).** Repainting the full 466x466 face is
~217k pixels; sustained throughput is ~0.9 Mpx/s, so a full-face repaint costs
roughly a quarter second. Anything that recolours the whole panel (the
`bigdigit` ground) is therefore inherently a hitch and must be quantized. This
is also why translucent full-circle overlays were removed: five vignette rings
re-blended ~107k px on every needle frame and dropped Vault from 60 to 37 FPS.

**Cached static faces.** Vault-Tec, Night City and now Dyno Cell each paint
their entire static face **once** into a 434 KB PSRAM `lv_canvas` at scene
build (`paint_vault_background`, `paint_hud_face(..., cached=true)`,
`paint_arc_background`); every later redraw is a blit rather than
re-rasterising vectors. This is the single biggest lever available, because
the bottleneck is CPU rasterisation, not the panel link. Measured: Vault
median 37 -> 61 FPS, min 4 -> 54, while *adding* the vignette and scanlines;
Night City 32 -> 37. Elements that move (needle, fill arc, peak tell-tale,
digits) stay as separate objects on top; only genuinely static art belongs in
the cache. Vault's completed canvas remains allocated across theme switches
because its serial per-pixel error-diffusion vignette is expensive to recreate;
range, zero angle, palette, face, and vignette values form its cache key, so a
setting change still repaints it once. Other faces continue to free their cache
in `destroy_scene()`.

Dyno Cell's cache covers the unfilled track ring, the zero notch, the five
scale numerals and the static "PSI" mark — the value wedge
(`s_arc_value_canvas`/`draw_value_arc`), the readout digits, peak and zone
label stay live above it. The value wedge now invalidates from its committed
smoothed geometry while raw pressure still controls the numeric/color state;
this remains the face the 60 FPS guard was established against. Because range
and zero-angle move the numerals and notch, `boost_gauge_apply_config()` no
longer special-cases arc with an incremental `refresh_zero_notch()`/
`refresh_tick_labels()` patch — those live objects no longer exist — and
instead takes the same `destroy_scene()`/`build_scene()` rebuild path
vault/hud/bigdigit already used for a config change, which is what makes the
cache repaint rather than silently keep stale numerals. Measured hardware,
demo mode, 3 fresh boots each, 30 s polling windows of `/api/v1/state`:
`worstRenderUs` max 58.4-63.7k us / mean ~24-25k us before, 43.8-51.3k us /
~22.7-23.1k us after; cadence guard min 56-57 -> 58-59, median 60 both. Host
audit (`--theme dyno-cell --seconds 25`) is unchanged before/after (flushed
px/cycle mean 10519 max 36488, 0 severe mismatches), confirming the geometry
and wedge invalidation were not touched.

Corollary: effects that would be prohibitive per-frame (vignettes, texture) are
essentially free once baked. Prefer baking over per-frame drawing.

**Caching does not help a flat fill.** Big Digit's ground is a solid colour, so
there is no vector art to pre-compute — a fill is already the cheapest primitive
LVGL has, and 24 cached full-screen grounds would want 10 MB besides. Its cost
is the unavoidable 217k-pixel repaint when the colour steps. The fix is to
*spread* that repaint, not to precompute it: `BIG_BANDS` full-width bands take
the new colour on successive ticks, so one long stall becomes several short
ones. This is a direct trade — the shorter the stall, the more the transition
reads as a visible wipe:

| `BIG_BANDS` | Worst cycle @40 MHz | Transition |
|---:|---:|---|
| **1** | 45.1 ms (**39 ms** at the current 80 MHz) | **selected** — one clean jump, no wipe |
| 2 | 33.3 ms | a single horizontal split |
| 4 | 25.8 ms | a clearly visible top-to-bottom sweep |

Banding and the clock attack the same stall independently, so `BIG_BANDS = 1` at
80 MHz lands near what 2 bands bought at 40, with no visible transition at all.

Total work is conserved, so `renderFps` barely moves between these. A/B any
change on `worstRenderUs`.

**The QSPI clock helps here and nowhere else.** Raising it 40 -> 80 MHz took the
unbanded recolour from 45.1 to 37.6 ms (-17%), because a full 434 KB frame is
about half that operation. The same change moves the partial-update faces 0-3%:
their dirty regions are far too small for the link to be the constraint. See
"QSPI clock" above.

**RGB565 has too few levels for a gradient over a dark colour.** Vault's face is
`#02100a`, whose green sits at **level 4 of 63** in RGB565. Darkening it toward
black therefore has only four values to land on, so a mathematically exact
vignette still resolves to four flat rings on the panel. The web mirror looks
smooth only because canvas is 8-bit (green 16 -> 6). The fix is a serpentine
Floyd-Steinberg-style **error-diffusion** pass in `paint_vault_background`, with
two error rows carrying RGB565 quantisation error into neighbouring pixels. The
pattern is baked into the cache so it never shimmers, and at native panel gamma
it reads as phosphor grain. Any future smooth ramp over a near-black colour
needs the same treatment; check the source channel's 565 level before assuming
a banding report means the gradient maths is wrong.

**Match the web mirror's method, not just its numbers.** Vault's CRT treatment
is a direct port of `drawVaultGauge`: the vignette is a smooth radial ramp from
r=120 to r=233 reaching 60% black, and the scanlines are 1 px rows every 4th
line at 16% black, both chord-clipped to the face. Two details matter. The
vignette is applied as a **per-pixel pass over the canvas buffer** after the
vector art is drawn — an approximation with nine concentric arcs banded visibly.
The scanlines are **not** baked; they are drawn last (`draw_vault_crt`, the
topmost child) so they cross the needle and digits as they do on the web. Their
phase comes from absolute screen y, which is what stops neighbouring dirty
regions from disagreeing and producing the tearing an earlier per-region
version showed. Cost of the on-top pass: ~1 FPS.

**Invalidate what changed, not the widget.** Night City's ghost pass repainted a
fixed 310x100 box on every value change; bounding it to the slots that actually
changed (usually the tenths alone) took the style from 37 to 42 FPS median and
22 to 31 min. `HUD_GLITCH_DX` is shared between the draw and the invalidation so
the dirty box can never be narrower than the pixels the ghosts touch.

Night City's gradient fill has a measurable but secondary cadence cost. A
three-round fresh-boot hardware A/B (`night-city`, demo mode, pixel shift off,
20 s per arm) measured gradient off at min/median **30/40 FPS** and gradient on
at **29/39, 29/39, and 29/38 FPS**. Across-round medians were **31.8 vs 32.0 ms**
`worstRenderUs`, **11 vs 13** over-budget frames/s, and **0.688 vs 0.735 Mpx/s**.
The implementation explains the extra work: the fill is one quantized solid
colour, and each colour-step crossing invalidates and recolours the complete
zero-to-value arc. The original `BIG_STEPS=24` covered the whole -15..+10 psi
range, but every non-positive sample intentionally maps to the same vacuum color;
there were therefore only **11 effective RGB565 colors** on the visible ramp (10
above zero), which explains why the transition looked much coarser than “24
steps.” A 48-slot full-range trial produced 20 effective colors, but still wasted
29 slots on identical vacuum. The retained mapping instead uses one fixed vacuum
sentinel plus **24 buckets devoted exclusively to positive pressure**. Bucket 1
starts immediately above zero and bucket 24 lands exactly at configured max;
firmware, Big Digit, and the web mirror share the same ceiling-based mapping. Endpoint movement between crossings already uses a
tight wedge. Reproduce the comparison with `tools/bench_hud_gradient.py`; judge
an optimization on render time, pacing, and pixels/s rather than FPS alone.

Component-isolation builds preserved Night City's invalidation geometry while
suppressing one raster operation at a time. Against the 39 FPS / 30.6 ms
gradient-on baseline, removing the chromatic ghost draws reached **45 FPS / 19.1
ms**, removing the primary digits **44.5 FPS / 28.8 ms**, removing the fill arc
**43 FPS / 27.6 ms**, and suppressing the cached background blit **41 FPS / 28.0
ms**. The ghost's two semi-transparent 96 px label passes were therefore the
largest discrete raster cost. They now pre-blend their colours against the
known static face and draw opaque, preserving the same offsets, glyphs and dirty
regions while avoiding two destination read/blend passes. This intentionally
approximates the original alpha composition where the two shifted copies overlap
or cross other art; it is not framebuffer-identical. A production hardware run
with gradient on improved median cadence **39 -> 44 FPS**; gradient off measured
**46 FPS**. This is a substantial win, but the face still does not reach 60 FPS
and the remaining costs are distributed across fill, primary glyphs, background
compositing, TE waits, and other live labels.

The tenths-only glyph-cache prototype exposed two independent LVGL contract
violations: `lv_font_get_glyph_bitmap()` returns an `lv_draw_buf_t *`, not raw
pixel bytes, and mutable PSRAM-backed A8 image sources outlived their draw-task
assumptions. The completed readout cache is one scene-owned, immutable PSRAM
block of **34,786 B** containing only digits 0-9, decimal, and minus, reused by
the primary readout and ghost pass; surrounding HUD labels keep their source
fonts. Draw callbacks publish stable descriptors only; the block
is never mutated or republished. The lifecycle is draw-unit safe: drain with
`lv_draw_wait_for_finish()` before scene teardown or cache release, and keep the
cache alive until all draw units finish. Allocation failure retains the source
font fallback. Host Night City audit: **517 comparisons / 0 mismatches**.
Hardware confirmation: 60 rapid four-theme cycles plus a 60-second Night City
soak completed with live rendering, no freeze, panic, `ESP_ERR_NO_MEM`,
send-color error, or TE timeout. `BOOST_HUD_READOUT_CACHE=0` now provides a
compile-time source-font A/B/fallback guard; cache-on and source-font host audits
both completed with zero stale or severe mismatches. Cache construction also
checks the second glyph lookup, descriptor consistency, integer overflow,
stride/format/index identity, and every packed-buffer bound before publication.

The consolidated-readout trial removes four large slot labels, the separate sign
label, and the full-screen ghost object. One styleless LVGL object submits the two
pre-blended ghost passes and then the authoritative primary glyphs, with an early
clip rejection for outer-ring-only dirty regions. Its changed-slot union reduced
the 30-second host audit from **11,254 to 7,297 flushed pixels/cycle** and **0.635
to 0.445 Mpx/s**, while both Night City and Dyno Cell completed **620 snapshot
comparisons with zero stale or severe mismatches**. On COM3, three interleaved
60-second rounds measured Night City gradient-off median `worstRenderUs`
**17,063.5-17,117 us** after the clip guard (down from **18,938-19,077.5 us** in
the matched pre-change baseline) and cut median panel work from roughly
**0.75-0.78 to 0.47-0.50 Mpx/s**. Gradient-on arms measured **17,398.5-17,628 us**
and **0.52-0.54 Mpx/s**. Median `renderFps` remained **41-44.5**, so this is a
real raster/traffic reduction but not a 60 FPS result. The remaining render tail
is still just over the 16 ms budget; disabling region DBUF or TE did not improve
it, and the 276x79 precomposed RGB565 tile prototype increased host flush work to
**21,175 pixels/cycle**, so it was rejected rather than flashed.

Vault-Tec now deliberately keeps the same consolidation pattern in a bounded
**146x34** six-slot readout object together with `VAULT_NEEDLE_SEGS=3` and the
hardware-proven two-triangle needle raster. The controlled consolidated-readout
comparison separated host correctness from panel cadence: 3 segments averaged
**11,460 px/cycle** with 3 severe / 8 total isolated needle-edge audit pixels;
2 segments averaged **12,858 px/cycle** with 0 severe / 1 total. On hardware the
best matched 3-segment fast-sweep round held **60/60 min/median FPS**, **17.162 ms**
median / **20.194 ms** maximum render time, and **1** over-budget frame. The
2-segment arm measured **58/60 FPS**, **18.222/33.558 ms**, and **9** over-budget
frames. A later 3-segment confirmation was noisy (**55/59 FPS**, **19.564/33.623
ms**, 28 over-budget), so it is recorded as run-to-run spread rather than proof of
a regression. Two segments clean up host antialiasing seams but enlarge the dirty
regions and worsened the matched hardware tail; the kept production choice is
the bounded consolidated readout plus **three** invalidation segments. Future
Vault changes must rerun both the RGB565 stale-pixel audit and interleaved hardware
cadence measurements.

The retained Vault renderer removes the **26 px counterweight tail** behind the
pivot by default. A persisted `vaultNeedleTail` option in the Vault theme editor
and `/themes/config` API restores it when desired. The wedge tip length, 15 px
hub, two-triangle raster, three invalidation segments, AA padding, CRT overlay,
and 146x34 readout remain unchanged. Draw and both invalidation paths resolve the
same runtime tail length; applying the setting rebuilds the scene, so old
foreground pixels cannot remain. The browser mirror follows the same geometry.
The final 25-second Vault audit rendered 1,517 cycles at **10,786 mean / 30,114
max flushed pixels per cycle** and **0.660 Mpx/s**. It found **zero severe RGB565
mismatches** and six isolated one-channel, one-step AA seams across 517
comparisons (worst one pixel), with no broad stale trail.

Both physical arcs now animate geometry through one shared latest-target linear
interpolator. Each new sample remains the exact target immediately; the visible
endpoint retargets from its currently displayed position over a fixed 40 ms
animation, cannot overshoot, and snaps on initialization, scene rebuild,
non-finite input, or a gap over one second. There is no adaptive EMA, alpha cap,
or small-change suppression. Only geometry is animated: raw pressure still
drives numeric readouts, peak/model/API state, and immediate Dyno/Night City
colour and threshold transitions. Draw and invalidation use the same committed
visual state. The Dyno Cell hardware cadence regression guard remained **min 58 /
median 61 FPS** over 103 post-warmup samples; 30-second host audits for both faces
completed 620 comparisons with zero stale or severe mismatches.

The full-cache hardware benchmark is **3 interleaved rounds** (6 arms, 336
samples total), versus **1 prior round** for the tenths-only benchmark (2 arms,
112 samples). Full-cache Night City gradient-off arms measured median FPS
**40-42.5** (aggregate median **42.0**), median `worstRenderUs`
**18,985.5-19,894 us** (aggregate **19,378.25 us**), and median
`pixelsPerSecond` **691,490-795,718** (aggregate **741,639**), with zero TE
timeouts. The prior tenths-only arms measured **40-41.5 FPS** (aggregate
**40.75**), **27,824-29,056 us** (aggregate **28,440 us**), and **690,014-
766,462 pixels/s** (aggregate **728,238**). These are honest directional
figures, not a clean causal A/B: the old result is one round, while the full
cache is three.

Two web-mirror fixes closed remaining parity bugs. Night City's chromatic split
had become visually negligible at **0.4 alpha / 3 px**; it now uses **0.7 / 6
px**. The Big Digit renderer ignored `bigDigitStaticBg`, `staticColor`,
`colorText`, and `textColor`; it now consumes all four settings.

Night City's colour menu also offers a persisted **True black background**
checkbox. It replaces the default `#080A08` face with `#000000`, turning unused
AMOLED pixels fully off; the cached physical face, screen background, chromatic
ghost pre-blend, and browser mirror all consume the same setting. Its gauge
track and live fill are **15 px** thick, up from 10 px (1.5x), with the wider
physical invalidation bounds kept in lockstep so arc edges clear correctly.
The ring is also pushed outward from radius 206 to 225; its unchanged 16 px
ticks move from radii 198-214 to 217-233 so their outer ends meet the panel
edge. The physical renderer, invalidation bounds, zero notch, and web mirror
share that geometry. A 60-second-per-arm hardware run with demo mode, pixel
shift off, TE sync and region double-buffering measured gradient off at
**min 30 / median 41 FPS**, median `worstRenderUs` **19,187.5 us**, and median
`pixelsPerSecond` **762,064**; gradient on measured **min 29 / median 39 FPS**,
**19,889.5 us**, and **808,324 pixels/s**. Both arms had zero TE timeouts. The
outward ring therefore adds some pixel throughput without a material latency
regression against the prior full-cache result (gradient-off aggregate median
42 FPS / 19,378.25 us / 741,639 pixels/s).
The host Night City audit retained **517 comparisons / 0 stale mismatches**;
the firmware build was flashed to COM3, the setting survived a hardware restart,
and the standard Dyno Cell cadence guard remained **min 58 / median 60 FPS**.

Night City's first sample is the exception to endpoint-only invalidation: after
a scene switch it invalidates the complete zero-to-current span. Without that
one-time dirty region, a rebuild rendered before the first vacuum sample left
the fill empty, and later endpoint wedges clipped the full arc into a moving
blob until the reading crossed zero. The host audit deliberately switches into
Night City from an already-negative reading to preserve this regression case.
Host verification reported 104 comparisons with zero stale pixels. On hardware,
the first Vault build took 1077 ms, while three cached returns took 64-101 ms;
the Dyno Cell cadence guard after a 3 s settle passed at min 58 / median 61 FPS
over 104 samples.

Vault-Tec also seeds its newly-built needle from the last committed pressure
when a theme switch rebuilds the scene. Previously the object started at zero
for one frame and jumped on the next sample. The simulator audit switches into
Vault-Tec at +8.0 PSI before the next gauge update and requires that update to
move the needle by 0.000 degrees.

**`render_fps` is not a smoothness metric.** It counts `LV_EVENT_RENDER_READY`,
i.e. render cycles actually performed, and LVGL only renders when something is
invalidated. A face whose content changes less often legitimately reports a
lower number while using *less* throughput. Compare `pixelsPerSecond` before
concluding a face is too slow.

**`worstRenderUs` is the choppiness metric.** It is the longest single
`LV_EVENT_RENDER_START` -> `LV_EVENT_RENDER_READY` interval in the reporting
window: the duration of a cycle, not the gap between cycles. Measuring the gap
instead was the first cut of this metric and it was useless, because an idle
screen produces long gaps and no stall at all.

Measured medians / worst cycle at 80 MHz: `arc` 63 / 42 ms, `vault` 60 / 18 ms,
`hud` 43 / 37 ms, `bigdigit` 31 / 39 ms (pre-cache figures). `arc`'s static
furniture (unfilled track, zero notch, scale numerals) is now cached the same
way vault/hud's is — see "Cached static faces" above for the measured
before/after `worstRenderUs`.

### Fonts

`main/fonts/` holds generated LVGL fonts plus their sources in
`main/fonts/src/`. All are OFL so they can ship in the image (the web mirror's
Bahnschrift/Consolas are Microsoft fonts and cannot be embedded):

| Font | Source | Use |
|---|---|---|
| `alvida_big` | Alvida Fatface (user supplied) | big-digit numeral |
| `font_mono_16/40` | IBM Plex Mono SemiBold | readouts, telemetry |
| `font_cond_14/18/22/32` | Saira Condensed SemiBold | labels |
| `font_cond_96` | IBM Plex Sans Condensed BoldItalic | Night City readout |
| `font_wide_22/32` | Saira SemiCondensed Bold | Big Digit labels |
| `neon_big` (118 px) | SF Alien Encounters Italic (user supplied) | neon readout, all three layouts |
| `neon_label` (24 px) | SF Alien Encounters **regular** | neon zone word and `P S I` |

The readout is italic; the zone word and `P S I` are the **upright** face, so
`main/fonts/` carries both `SFAlienEncounters-Italic.ttf` and
`SFAlienEncounters.ttf`. The web mirror inlines both under one family name, and
selects between them purely by the presence of the `italic` keyword — a family
with a single face would silently serve that face for either request.

The three neon fonts carry only `0x30-0x39,0x2E` (plus `0x20,0x41-0x5A` for
`neon_label`); the face has no `-` contour, so the minus mark is drawn from
bar geometry rather than a glyph. Their sizes are not free parameters — the
readout cell pitch, the sprite tile size and the invalidation bounds are all
derived from the generated `line_height` and widest glyph ink, so changing a
size means re-deriving those (see the block comments around `NEON_SLOT_W`).

Regenerate with `lv_font_conv`, always passing `--lv-include lvgl.h` (the
default `lvgl/lvgl.h` does not resolve against the managed component). Glyphs
missing from a face can be merged from a second `--font`. **Do not** try to
embed box-drawing/arrow codepoints through shell escapes — draw such marks as
shapes instead; escaped UTF-8 has repeatedly been mangled a layer early.

### Theme colours and the Big Digit ground

`PUT /api/v1/themes/config` edits the three zone colours (`vacuum`, `boost`,
`overboost`) of any theme and persists them in NVS, matched by theme id rather
than table index so a firmware update that adds or removes a theme cannot paint
one theme with another's colours. Face, track, text, muted and zero stay fixed:
a theme whose face and text are both user-settable is a theme that can be made
unreadable. Note for `neon`: its cached background is painted from `track`,
which is *not* user-editable but *is* changed by `neonPreset` — so a preset
change repaints it. The marquee border's accent bulbs are LIVE (drawn each
update in the ring's zone colour once that stage is reached), so a zone-colour
edit reaches them via the scene rebuild `apply_theme()` performs — the
background cache itself only bakes the neutral `track` bulbs, and `neon_bg_key_t`
carries no zone fields. If `track` ever
becomes editable, `neon_bg_key_t` already covers it, but that pairing is worth
re-checking rather than assuming. `{"id":"...","reset":true}` restores the built-in palette, and each
theme reports `customized` in `GET /themes`.

For `neon` both of those follow the **selected preset**, not the compiled-in
entry. The table has to hold one palette as the initial one (Violet), and
comparing/restoring against it meant selecting any other preset immediately
reported the theme as customized, while reset painted Violet over the selected
preset and left the selector pointing elsewhere — the two disagreed until the
selector was cycled away and back. The preset is the baseline: selecting one is
not a customization, and reset returns to that preset's colours.

The same endpoint carries `bigDigitStaticBg`. Big Digit normally sweeps its
whole ground through the zone colours, which is the only full-screen repaint in
any face. Turning it off gives white numerals on the theme's face colour and
removes that repaint entirely: **worst render cycle 39 ms -> 7 ms**, taking Big
Digit from the worst-stalling face to the best.

### Finishing an OTA

`POST /api/v1/ota` streams the image, validates it and selects the boot
partition, but the new firmware does not run until the device reboots.
`POST /api/v1/restart` does that (400 ms deferred, so the response is delivered
first); the dashboard calls it automatically after a successful upload. A
1.6 MB image uploads in ~9 s (~190 KB/s).

To confirm an OTA actually took effect, check the boot log for the partition
offset: `Loaded app from partition at offset 0x420000` is ota_1, i.e. the
uploaded image. Still booting `0x20000` means the serial-flashed image is
running and the OTA proved nothing.

### Debug snapshot endpoint

`GET /api/v1/debug/snapshot` re-renders the live screen into a PSRAM buffer and
streams it as raw little-endian RGB565 (466x466). `tools/fetch_panel_snapshot.py`
turns that into a PNG. This is how the physical face is verified without
photographing the panel. Requires `CONFIG_LV_USE_SNAPSHOT=y`.

The Dyno Cell zero notch is intentionally a live overlay above its moving value
arc. The track, numerals, and unit label remain cached, but baking the notch into
that lower canvas lets the colored wedge cover the zero reference.

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

Display bring-up is **not** `bsp_display_start()`. The app must link
`main/boost_display.c` and call `boost_display_start()` so LVGL strips stay in
internal DMA RAM. See “Critical: AMOLED display path” above.

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

## Real MAP + ambient sensor bus

- The dedicated sensor bus is I2C port 0: **SDA=GPIO17**, **SCL=GPIO18**. It remains separate from the Waveshare BSP bus on GPIO15/14 and avoids the UART-default GPIO43/44 pads.
- ADS1115 is `0x48` with ADDR grounded; the 5 V GM 12223861 three-bar MAP signal is read on A0.
- BMP280 is `0x76` with SDO grounded and must remain on 3.3 V logic.
- `SENS_I2C_HZ` in `main/boost_sensors.c` is **100 kHz**. The run passes through a MOSFET level shifter with 4.7 kOhm pull-ups, whose rise times do not comfortably support 400 kHz in a vehicle. It is not an API, UI, NVS, or Kconfig setting.
- The sensor task runs at **16 ms**, matching the cadence contract. ADS1115 is read every loop (62.5 Hz MAP), BMP280 every tenth (6.25 Hz ambient).
- `GET /api/v1/sensors/scan` uses ESP-IDF `i2c_master_probe()`, which probes at 100 kHz independently of `SENS_I2C_HZ`, and requires four consecutive ACKs before reporting an address. That filter exists because a sweep through *changing* addresses was observed to invent one-off phantom addresses on an electrically empty bus.

### MAP conversion and atmosphere calibration

The GM 12223861 curve is defined at a **5.00 V** supply by 0.619 V → 40 kPa and
4.818 V → 304 kPa. The sensor is ratiometric, so a supply that is not 5.00 V is
normalized out before the transfer function is applied:

```text
normalized_volts = map_volts * 5.00 / supply_volts
nominal_kpa      = 62.8721124 * normalized_volts + 1.08216242
corrected_kpa    = nominal_kpa + calibration.offset_kpa
gauge_kpa        = corrected_kpa - ambient_kpa      (BMP280, live)
```

The configured supply defaults to **5.20 V** and is editable in Settings. Because
gauge pressure subtracts the *current* BMP280 reading, weather and altitude drift
correct themselves; only sensor error needs calibrating.

**Calibration is manual and never automatic.** With the engine off and both the
MAP port and the BMP280 open to the same atmosphere, Settings → *Calibrate MAP to
ATM* observes ~2 s of samples and stores `offset_kpa = mean(bmp) - mean(nominal)`.
It refuses stale, unstable, implausible, or over-limit readings, and a failure
never disturbs the existing calibration. Recalibration **replaces** the offset —
nominal is always recomputed from raw volts, so offsets cannot compound.

One known pressure identifies zero error but cannot independently identify sensor
gain, so the correction is deliberately **additive in kPa**: exact at atmosphere,
which is the reading that must be right, and degrading gracefully toward full
boost. `ref_map_volts` is stored so a second calibration point could be added
later without a schema migration. Do not convert this to a multiplicative fit on
one point.

Supply voltage and the calibration record live under their own NVS keys
(`map_vsup`, `map_cal`) in the `boost` namespace rather than inside
`boost_config_t`, so growing them never risks discarding existing gauge settings.
Editing the supply after calibrating re-derives the offset from the stored raw
reference instead of stacking a correction on the old normalization. Note that a
badly wrong supply entry is absorbed into a correspondingly large offset — the
gauge still reads zero at atmosphere but is mis-scaled under boost, so check the
displayed offset after changing supply, and recalibrate.

Calibration diagnostics live on `GET /api/v1/sensors/calibration`, deliberately
*not* on `/state` or the WebSocket payload: that path runs at 62.5 Hz into a smaller
JSON buffer, and Settings must be able to show real sensor state even while demo
mode is driving the gauge.

Bench history for the bus failures that preceded this is in
[docs/troubleshooting/i2c-sensor-bus.md](docs/troubleshooting/i2c-sensor-bus.md).
The short version: the bus was restored by replacing the ADS1115 assembly.
GPIO17/18 were never damaged, the RX/TX remap was never needed, and the
400 kHz → 100 kHz reduction was **not** the fix.
