# Boost Gauge

ESP-IDF + LVGL boost gauge for the **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 CO5300 AMOLED).

Right now the MAP path is **simulated** (vacuum ↔ boost sweep). Swap `boost_sim.c` for ADS1115 reads when the sensor wiring is ready.

## What you should see

- Full-screen dark cabin gauge (“Pit Lane Night”)
- Dual-climate arc: **teal** vacuum · **amber** boost · **flare red** overboost (default ≥ **8 psi**, configurable)
- Big signed PSI, zone label (`VAC` / `ATMO` / `BOOST` / `OVER`)
- Peak hold; **short tap** resets peak
- **Hold ~2s** toggles max/min brightness (100% ↔ 12%)
- Top chip reads `DEMO` until a live sensor path sets `sample.demo = false`
- Samples, the unified-color filled arc, center PSI, and peak hold update every 16 ms (~60 Hz). The physical gauge remains on that cadence while network telemetry is intentionally decoupled from the display loop.

This firmware **replaces** the factory app launcher.

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
targets **20 Hz**; the browser canvas interpolates at **60 FPS** between samples.
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
performs a bounded broadcast at **20 Hz**. A newly connected dashboard MUST NOT
close or evict an existing socket, because concurrent or stale tabs otherwise
force Live/Fallback churn and can produce out-of-order target jitter.

The browser sends an application heartbeat every **750 ms**; the server consumes
that heartbeat as part of the WebSocket protocol. If the socket is unavailable,
the browser uses HTTP `GET /api/v1/state` fallback at **4 Hz**. The connection
badge exposes the active path as **Live · WebSocket 20 Hz** or
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
receiving 20 Hz WebSocket telemetry or 4 Hz HTTP fallback.

### Fourth-client behavior

The firmware owns a fixed pool of **3 WebSocket clients**. A fourth handshake is
rejected/closed for that newcomer only; the three existing sockets stay open and
continue receiving telemetry. The browser keeps its fallback poll at **4 Hz**
(`POLL_FRAME_MS = 250`) and retries WebSocket connection every **1 s**. A
successful `/api/v1/state` fallback sample shows **Live · HTTP 4 Hz**;
**Disconnected** means both WebSocket and HTTP state polling are unavailable.
A retry attempt must not downgrade healthy fallback, and a restored socket shows
**Live · WebSocket 20 Hz**.

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
**434 KB**; the active 60 MHz CO5300 QSPI trial has a transfer-only floor of
about 14.5 ms (69.1 FPS), before GIF decoding and LVGL rendering.

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
  to `drawArcGauge` / `drawVaultGauge` / `drawHudGauge` / `drawBigDigitGauge`.
  `tools/mock_server.py` serves the four themes (each with a `style` field).
  Regenerate embedded assets after any web edit. `setTheme()` drives only the
  gauge palette, so the dashboard chrome keeps one fixed identity.
- **Simulator:** `sim/` builds the same `boost_gauge.c` on the host and renders
  headless PNGs, including the generated fonts, so a face can be checked without
  flashing. SDL2 is optional (only `--window` needs it); `--theme <id>` selects
  the style. This is the fastest verification loop — use it before burning a
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

**Cached static faces.** Vault-Tec and Night City each paint their entire static
face **once** into a 434 KB PSRAM `lv_canvas` at scene build (`paint_vault_
background`, `paint_hud_face(..., cached=true)`); every later redraw is a blit
rather than re-rasterising vectors. This is the single biggest lever available,
because the bottleneck is CPU rasterisation, not the panel link. Measured: Vault
median 37 -> 61 FPS, min 4 -> 54, while *adding* the vignette and scanlines;
Night City 32 -> 37. Elements that move (needle, fill arc, peak tell-tale,
digits) stay as separate objects on top; only genuinely static art belongs in
the cache. Free the buffer in `destroy_scene()`.

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
smooth only because canvas is 8-bit (green 16 -> 6). The fix is **ordered
dithering** — an 8x8 Bayer threshold added before truncation in
`paint_vault_background`, trading spatial noise for tonal resolution. Because
the pattern is baked into the cache it never shimmers, and at native panel gamma
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
`hud` 43 / 37 ms, `bigdigit` 31 / 39 ms. `arc` is now the worst-stalling face and is the
next candidate for a cached ground.

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

Regenerate with `lv_font_conv`, always passing `--lv-include lvgl.h` (the
default `lvgl/lvgl.h` does not resolve against the managed component). Glyphs
missing from a face can be merged from a second `--font`. **Do not** try to
embed box-drawing/arrow codepoints through shell escapes — draw such marks as
shapes instead; escaped UTF-8 has repeatedly been mangled a layer early.

### Debug snapshot endpoint

`GET /api/v1/debug/snapshot` re-renders the live screen into a PSRAM buffer and
streams it as raw little-endian RGB565 (466x466). `tools/fetch_panel_snapshot.py`
turns that into a PNG. This is how the physical face is verified without
photographing the panel. Requires `CONFIG_LV_USE_SNAPSHOT=y`.

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

## Next: real MAP + ADS1115

1. Wire ADS1115 to board I2C: **SDA=GPIO15**, **SCL=GPIO14**, 3.3 V logic.
2. Feed the GM 3-bar MAP through the ADS1115 (respect 5 V sensor / level shifting).
3. Replace `boost_sim_tick()` with ADC conversion → kPa → gauge PSI.
4. Set `sample.demo = false` so the chip shows `LIVE`.

Keep `boost_gauge_update()` as-is; only the sample source changes.
