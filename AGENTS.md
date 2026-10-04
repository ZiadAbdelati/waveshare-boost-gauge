# Agent guidance

This repository is an ESP-IDF 5.5.1 firmware/dashboard for an ESP32-S3 AMOLED boost gauge. These rules are load-bearing. A fresh agent must be able to resume from this file alone; do not rely on chat history.

Current release is **`v1.0.0`** (ESP-IDF 5.5.1, app image ~2.5 MB in `release/`, released 2026-10-02 with selectable pressure units). The last **hardware-verified** release is **`v0.9.7`** — 1.0.0's firmware is host-built and simulator-verified only, so its on-glass cadence and overlay unit-cycle behaviour are unmeasured; quote v0.9.7 (or a fresh board run) for physical measurements, never 1.0.0. The release version has exactly ONE source, `version.txt`, and `tools/tests/test_version_consistency.py --release` is the gate that stops the firmware image and both apps from disagreeing with it. The full historical regression ledger lives in [`docs/regression-ledger.md`](docs/regression-ledger.md); the condensed guard rails below are the currently-actionable invariants, grouped by area. When a change touches one of these areas, re-read the relevant ledger rows for the measurement detail behind the rule.

Regression tooling: run `python3 tools/test_suite.py` (host suite, before every commit), `python3 tools/tests/test_version_consistency.py --release` (version gate, before publishing any release) and `python3 tools/check_hardware_gates.py` (hardware release gates, before every release/flash-to-car); see `tools/tests/README.md`.

Gauge UI verification, before flashing anything that touches layout: `./sim/build/boost_gauge_sim --stream` + `python3 tools/sim_panel.py` (http://127.0.0.1:8787/) is the live interactive simulator — theme, pressure unit, neon layout/font/preset, pressure slider and the organic/fast-sweep demo waveforms, all driving the REAL firmware renderer (no SDL2 required). For a still, `./sim/build/boost_gauge_sim --screenshot DIR --theme <id> --unit <unit>` + `python3 sim/raw_to_png.py DIR`. **Render the whole theme x unit matrix, not just psi** — the pressure-unit feature shipped with bar/kPa numerals overlapping the dyno arc, the neon rings and the Night City right edge, none of which psi showed.

## Working agreements

- For every non-trivial change, the coordinator MUST use `Task` subagents heavily: delegate independent research, implementation slices, and verification/testing slices in parallel where possible. The coordinator owns the top-level contract, integration, and final acceptance; do not delegate away the architecture decision.
- Before assigning work, map the affected files and symbols. Announce ownership through the coordination channel. One agent owns a file at a time; agents MUST NOT overwrite unexpected work. Re-read after another agent edits, and integrate only the intended diff.
- No unverified edit is acceptable. First inspect the existing implementation and callers, then make the smallest source change, then exercise the affected path. Never invent measurements, claim a test was run when it was not, or replace a failing guard with a weaker one.
- Every subagent must return observed evidence, decisions, files touched, and remaining risks. Do not keep critical reasoning only in the session — update the relevant README section and these guard rails in the same change that alters the architecture or a regression.
- Do not edit generated C assets or release binaries by hand. Do not add compatibility shims, alternate implementations, or a second convention beside an existing one.

### Source ownership

- `main/boost_display.c/.h`: AMOLED bring-up, LVGL/DMA buffers, panel transfer and display lock.
- `main/boost_gauge.c/.h`: gauge rendering and exclusive GIF playback lifecycle.
- `main/boost_media_store.c/.h`: raw `media` partition format, upload transaction, CRC, mapping, and deletion.
- `main/boost_web.c`: HTTP/WebSocket API and upload/delete request serialization.
- `main/boost_json.c/.h`: single owner of every JSON shape served by BOTH transports (HTTP and BLE Control). Parity is structural, not copy-synced. The web timezone dropdown and both companion apps derive their zone lists from `web/tz.json` (canonical POSIX table).
- `main/boost_model.c/.h`: model state and publication; owns the wall clock (`settimeofday`, `s_clock_trusted`) and the DS3231 seed/calibration paths.
- `main/boost_sensors.c/.h`: I2C bus (port 0, GPIO18/17), ADS1115/BMP280 sampling, calibration, and the DS3231 RTC device (`boost_sensors_rtc_read/write`).
- `web/`: dashboard source (cockpit + settings views). `main/generated_web_assets.c/.h` are generated outputs only.
- `tools/embed_web.py`: web asset regeneration. `tools/mock_server.py` mirrors config/network APIs. `release/`: explicitly produced release artifacts only.

## Cadence contract

Keep these rates distinct; never use one as a substitute for another:

- Sensor sampling and the physical gauge render/update path: every **16 ms**, approximately **60 Hz** (the physical gauge is the hardware gate).
- **Every selectable theme has two cadence gates.** The constant-slew fast-motion sweep (`tools/bench_fast_motion.py sweep --theme <id> [--layout N]`, 9.789 psi/s) is the direct capacity gate: each theme must sustain median physical `renderFps` ≥60; dyno-cell is the reference guard, not a ceiling. The organic demo waveform is demand-aware: `tools/bench_theme_matrix.py` compares `renderFps` with `gaugeDemandPerSecond` (one count per 16 ms gauge update that creates at least one dirty area). PASS requires median demand coverage ≥95% in demanded windows; zero-demand windows are idle, not failures. Firmware without the metric retains the legacy median-60 gate. A real MAP sensor at constant atmosphere invalidates nothing and is not a cadence measurement; demo mode is the precondition for every check. This is a measurement correction, never a throttle. No visual compromise buys frames: a visual-vs-performance trade is a proposal to the user first.
- `demoFastSweep` (the constant-slew 9.789 psi/s triangle) is persisted like `demoMode`: NVS key `demo_fast_sweep`, loaded and applied by `boost_theme_init()` before the sim ticks, and `boost_sim_init()` must never reset it (the boot value comes from the theme store load). Still a SEPARATE flag from `demoMode` — it only matters while demo mode is on, but store/toggle the two independently. Bench tools restore the organic waveform when they finish; a reboot keeps the last selected sweep.
- Network telemetry: fixed pool of **3 WebSocket clients**, each with at most one in-flight heap-owned frame; bounded broadcast is notification-driven at the sample rate, **62.5 Hz** (`STATE_WS_PUSH_DECIMATION 1`, one frame per 16 ms sample). `STATE_WS_FRAME_MS 50` is the idle *fallback* timeout for a stalled producer, not the period.
- Browser application heartbeat: **750 ms**, consumed by the server.
- Browser live canvas: renders on every `requestAnimationFrame`, uses a **35 ms** EMA, and accepts only strictly newer `uptimeMs` targets; timing resets after a gap greater than **1 s**.
- HTTP fallback state polling: **4 Hz**. Sparkline: **4 Hz**.
- Calibration diagnostics (`GET /api/v1/sensors/calibration`): **2 Hz** while the Settings panel is visible, **1 Hz** on the cockpit for the Night City ATM readout. Deliberately off `/state` and the WebSocket so the 62.5 Hz payload is untouched, and so Settings can show real sensor state while demo mode drives the gauge. The cockpit ages the reported `bmpAgeMs` against the browser clock between polls.
- Background RAM logging: **5 Hz** (`BOOST_LOG_INTERVAL_MS` 200, `BOOST_LOG_CAPACITY` 18,000 → **1 hour**).
- Browser connection badge: **Live · WebSocket 60 Hz**, **Live · HTTP 4 Hz**, or **Disconnected**; it MUST expose the active transport.
- Browser device-pixel ratio: cap at **2**. Browser GIF preview: disabled.

Do not add display timers/dividers, throttle the 16 ms gauge readout, or judge WebSocket/canvas cadence with the physical-display guard. Demand-aware organic acceptance is a measurement correction, never a display divider or render throttle. GIF playback is an exclusive full-frame path and is not expected to satisfy the live-gauge FPS threshold. A new dashboard MUST NOT evict an existing WebSocket client: single-owner behavior caused concurrent/stale tabs to force Live/Fallback churn and out-of-order target jitter.

### Fourth-client and GIF regression invariants

- The WebSocket pool is exactly three clients. A fourth handshake may be rejected/closed for the newcomer, but MUST NOT close or disturb any existing client. Browser fallback remains `POLL_FRAME_MS=250` (4 Hz), while WebSocket retries are 1 s. A successful HTTP state sample MUST show `Live · HTTP 4 Hz`; a retry attempt MUST NOT mark it `Disconnected`. Restored WebSocket delivery MUST show `Live · WebSocket 60 Hz`. `Disconnected` requires both transports to fail.
- GIF decompression is not project-written. The parsing/LZW/frame-timing core is AnimatedGIF (`main/gif/boost_gif_dec.c` is a project-owned copy with turbo LZW enabled); `main/boost_gauge.c` supplies the custom locked widget/descriptor integration; `main/boost_media_store.c` owns raw dual-slot CRC publication and `esp_partition_mmap`. The project's direct-push pipeline (dual-core decode + `boost_display_push_bitmap`) is the playback path; the LVGL software fallback renders the little-endian RGB565 framebuffer (the bridge byte-swaps it). Keep mapped bytes alive through widget destruction: lock display, destroy widget, then unmap. Do not describe the decode core as a custom decoder.

## AMOLED display and DMA invariants

- Start the gauge with `boost_display_start()`, never stock `bsp_display_start()`.
- LVGL draw buffers MUST stay in internal DMA-capable memory (`use_psram = false`). Production strips are 20 lines: 18,640 bytes per buffer / 37,280 bytes double-buffered. Keep transfers capped to one strip and queue depth at 4 (`CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH=4`). Preserve the display lock and internal-memory reservation needed for Wi-Fi and DMA to coexist.
- region-dbuf uses **two** internal DMA scratch strips (`BOOST_REGION_DBUF_QUEUE_DEPTH 2`), not the SPI queue depth. Its `esp_lcd_panel_draw_bitmap()` calls are functionally blocking (`tx_param()` drains in-flight transactions), so transfers never pipeline beyond depth 1; two buffers give correct double-buffering. The original four held ~37 KB of DMA-capable internal RAM that the BLE controller needs — do not restore four without re-verifying BLE init and the cadence guard together.
- Preserve LVGL partial refresh and the CO5300 even/odd rounder. Do not move full-frame buffers to PSRAM, enlarge strips for an unmeasured speed claim, restore a marker/knob, replace the arc with retained line segments, or invalidate stationary arc endpoints without before/after evidence.
- The QSPI clock is `BOOST_LCD_PCLK_HZ` = **80 MHz**, owned by this repo (vendored `panel_new()`). Do not raise it; do not configure hardware from `managed_components/`.
- After any display-path change, flash hardware and run the cadence guard for a 30-second live-gauge soak:
  `python3 tools/check_display_cadence.py --url http://<board-ip> --seconds 30`
  Accept only a sustained median of at least 60 physical FPS. **Run this in demo mode on the `dyno-cell` (arc) face** — the conditions the 60 FPS threshold was established under. Each theme must also hold sustained median physical `renderFps` ≥60 on the constant-slew sweep (`tools/bench_fast_motion.py sweep`). Serial must show the internal-DMA path and no `ESP_ERR_NO_MEM` or `send color data failed`. A hardware result is required before declaring the change complete. Switch back to the real sensor afterwards.

## Raw media is authoritative (never SPIFFS)

The media store MUST remain a raw dual-slot partition; do not reintroduce SPIFFS, a second staging file, or direct replacement of `active.gif`.

- Partition label is `media`, offset `0x820000`, size `0x7E0000`, split into two slots. Boot scans headers and accepts only CRC-valid headers, selecting the newest generation.
- Upload targets the inactive slot. Erase only the required aligned range, stream payload and CRC, and write the committed header last. A header is not publication: commit is atomic only after payload and validation are complete.
- Playback maps the committed payload with the raw partition mmap API and gives LVGL a variable image descriptor. Do not free mapped storage until LVGL has been destroyed/unmapped.
- Abort or any failed upload MUST preserve the previously committed GIF. Delete removes the committed slot only after playback is stopped; repeated deletes must remain harmless (two repeated deletes are verified).
- The verified hardware benchmark is a complete **1,379,129-byte** GIF upload through the raw dual-slot store in **7.504 s**. The raw store is the architectural fix, not an optimization invitation to restore SPIFFS.

### Upload/delete state machine

`Idle → Uploading → Validating/Committing → Published → Playback` is the only publication path. `Uploading` is serialized; an overlapping upload or delete is rejected by the server with **409**. On failure or cancellation, transition through abort/cleanup back to `Idle` while retaining the prior committed slot. In the browser, clicking Delete during upload MUST abort the XHR, wait for its settlement, then issue `DELETE`; never fire concurrent replacement/delete requests. Preserve this ordering and the server-side 409 guard.

## Web assets and releases

- Edit `web/index.html`, `web/app.js`, and `web/styles.css`, then regenerate embedded assets with:
  `python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h`
  Review the generated diff; never hand-edit `main/generated_web_assets.c/.h`.
- **Regeneration is not part of the build.** A web edit that is committed, built and flashed without this step ships a dashboard that silently keeps its previous behaviour (this happened: three consecutive commits of neon web-mirror work reached the board with none of it live).
- **Verify against the device, and decompress first.** Assets are served gzipped, so grepping the raw HTTP response for a token you just added finds nothing whether the asset is current or stale. Check with:
  `python -c "import urllib.request,gzip;r=urllib.request.urlopen('http://<ip>/app.js');print('TOKEN' in gzip.decompress(r.read()).decode())"`
- A release is not complete until the verified ESP-IDF build produces the app image and the release directory contains the bootloader, partition table, OTA data, app image, merged full-flash image, flash helper, and refreshed `SHA256SUMS`. The merged image is for resetting the complete layout; later web OTA uses the app image, not the merged image. Tag the release commit, publish every file in `release/` except documentation-only metadata as GitHub release assets, mark the new release latest, and verify the published asset checksums. Do not publish artifacts from an unverified build or claim hardware behavior from a host-only run.
- Hardware release verification must cover boot, network access, the live physical cadence gate, transport-specific badge behavior, media upload/abort/delete behavior, and serial error absence. Record measured outputs in `docs/regression-ledger.md` (and summarise in `release/README.md`/this file) before committing.

## Theme system and physical input

`boost_theme.c:s_defaults[]` is the single authoritative theme order. The web picker and physical swipes consume that order through `/api/v1/themes` and `boost_theme_at()`. The selectable order is **Dyno Cell, Vault-Tec, Night City, Big Digit, Neon** (a vertical swipe up advances to the next entry, wrapping; swipe down moves backward). Sport Cluster was removed outright (renderer, enum member, `"sport"` style token and web mirror) in the 2026-08-10 repo audit; its design history lives in git if it is ever revived. If it is restored, keep its static ring/ticks in the PSRAM `lv_canvas` and its live readout bounded rather than repainting the full face. Theme changes rebuild one LVGL scene under the existing display lock; no full-frame transition buffer or animation is used because it would threaten the 16 ms partial-refresh path.

Gesture classification tracks the greatest signed excursion during `LV_EVENT_PRESSING`, not only the release coordinate. Only movement within the **12 px tap slop** resets peak; movement from 12 through 47 px is a rejected drag; a valid predominantly vertical drag changes one theme at **48 px**; a horizontal drag (≥48 px, 4:5 ratio) pages between boost and TPMS. GIF playback suppresses tap-reset, theme-swipes and page switches, but the one-second hold-to-dim still works over media (only tap/theme/page actions are gated on `media_active()`). A two-finger hold (raw CST9217 point count ≥2 via `boost_display_touch_point_count()`) for 3 s shows the AP-join QR overlay (`WIFI:T:WPA;S:<ap_ssid>;P:boost1234;;`); the 1 s hold-to-dim is suppressed while both fingers are down, and the QR is dismissed by any fresh tap. The screen object survives theme rebuilds, so synchronous `boost_gauge_apply_theme()` from `LV_EVENT_RELEASED` deletes children but not the event target.

`boost_page.c/.h` owns page 0 (boost) and page 1 (TPMS): LEFT enters TPMS and RIGHT returns to boost, with no wrap-around. Vertical theme swipes and tap peak reset are page-0-only; the one-second brightness hold works on either page and commits brightness only after a successful panel command. TPMS BLE central is compiled in by default (`BOOST_TPMS_BLE_ENABLED=y` in `main/Kconfig.projbuild`) but the link is runtime-disabled until the persisted `tpmsBle` setting is flipped (default **off**, so a fresh boot never touches the radio).

Vault needle color and counterweight tail are persisted theme settings. Red recolors only the body, the hub remains green, and changing either setting must rebuild or invalidate the complete old/new needle geometry.

## Guard rails by area

These are the currently-actionable invariants distilled from the regression ledger. Each carries the date the guard was established; see `docs/regression-ledger.md` for the full row. When in doubt, do not weaken a guard without a hardware measurement and a ledger row.

### Display, cadence, and TE

| Guard | Since |
|---|---|
| 16 ms physical gauge path; no timer dividers, throttles, or demo-mode demand reduction | 2026-08-10 |
| Every theme passes constant-slew median `renderFps` ≥60 and organic demand coverage ≥95%; dyno-cell is the reference, not a ceiling | 2026-08-09 |
| `teWaits == renderFps` by construction (one increment per writeback pass); `teSkips` ⊂ `teWaits`; only `teTimeouts` is an independent error signal | 2026-08-09 |
| `teSync` and `teScanline` are runtime toggles (default OFF). A fresh visual tear check on the physical panel is required before leaving either ON; framebuffer snapshots cannot contain a tear | 2026-08-10/11 |
| `regionDBuf` ON is the user-confirmed tear-free baseline; OFF tears and is reference-only | 2026-08-12 |
| A zone flip recolors the entire lit run in one frame and is at the hardware floor (~40-100 ms worst); sub-20 ms requires spreading the recolor across frames, a visible transition the user rejected (banded sweep reverted 2026-08-10) | 2026-08-12 |
| Never scale glyph sprites per frame (LVGL transform leaves AA seams); marquee uses pre-scaled A8 sprites | 2026-08-11 |
| Draw and invalidation must share the same committed geometry/constants; never draw from the live sample while invalidating on a threshold | 2026-07-28 |
| Vault needle: exactly two triangles (a third adds no geometry but pays the mask path), three invalidation segments; do not reintroduce overlap to hide a seam without same-firmware A/B | 2026-08-12 |
| Vault static canvas is retained across theme switches, keyed on range/zero/palette/face/vignette; never put moving elements in a cache | 2026-08-11 |

### dyno-cell (arc)

| Guard | Since |
|---|---|
| Arc face geometry and wedge invalidation are the path the 60 FPS guard was established against; keep byte-for-byte | 2026-07-28 |
| Zero dead zone: below the gap edge the value arc draws NOTHING (atmosphere, like neon's half-segment threshold); never reintroduce a min-sweep stub (rounded caps made it a ~14 deg blob that flashed at zero crossings) | 2026-09-01 |
| Gap edges are rounded OUTWARD (ceil boost start / floor vac end) in `value_arc_angles()` because LVGL truncates arc angles to whole degrees; nominal gap 3.6 deg + 26 px zero marker stay a pair — any change to either re-derives the cap-reach/marker-half constraint at EVERY user zeroAngle, and needs sim renders at a fractional (236.25) and integer (220) zeroAngle | 2026-09-01 |
| Zero notch is a live overlay above the moving value arc, never baked into the cached background | 2026-08-12 |
| `dynoTrueBlack` uses `#000000` for the arc face (default `#090A0D`) so unused AMOLED pixels are physically off; persisted via NVS `dyno_black`, served on `/themes`, and mirrored in the web/app theme editors like Night City's `hudTrueBlack` | 2026-10-03 |
| Readout invalidation is per-glyph ink box (`arc_readout_ink_box()`/pen math) shared with the draw; re-run BOTH the host audit and a screenshot diff vs the prior build | 2026-08-13 |
| Readout font is 65 px Archivo Black; any size change re-derives pitch/slot from the generated glyph advance | 2026-08-13 |
| Do not reintroduce `transform_scale_x` for glyph widening (AA-seam failure) | 2026-08-12 |
| Readout dead zone (ALL themes except vault-tec, 2026-09-06): values within ±0.1 psi fold to 0.0 through the shared `boost_readout_display_psi()`/`BOOST_READOUT_DEADBAND_PSI` in `boost_neon_geom.h` — arc (`format_value_slots`), HUD (`update_hud` digits + both sign checks), big-digit (`update_bigdigit` digits + minus), neon (`boost_neon_layout_readout`, the draw/invalidation choke point). Outside the band the raw value passes through — a one-band shift breaks the sign at the edge (raw -0.15 rendered positive "0.1"). Zone colours: dyno-cell's arc/wedge and vault-tec keep RAW psi (the arc draws nothing at atmosphere by the zero-gap guard); the NEON zone-colour decision FOLDS through the same helper (2026-09-09 user override — engine-off noise straddled raw 0.05, flipping vacuum/boost every sample and re-firing the marquee's deferred zone-flip repaint as visible second-ring flashing) inside `neon_zone_rgb()`/`neon_zone_id()`, so draw and flip detection share the folded decision. Web mirror: `arcReadoutDisplayPsi()` at the four readout sites and `neonZoneDisplayPsi()` (which delegates to it) at the neon zone colour/id sites; `drawVaultGauge`/`splitNum(psi, 2)` stay RAW (user request). Never add a second band definition — every fold delegates (contract test `tools/tests/test_readout_deadzone.py`) | 2026-09-05/06, neon zone override 2026-09-09 |

### vault-tec

| Guard | Since |
|---|---|
| Needle seeds from `s_display_psi` on scene rebuild (no jump-to-zero frame) | 2026-08-12 |
| `vaultNeedleTail` defaults off, persisted through theme store/API/web; draw and invalidation share the runtime extent | 2026-08-12 |
| Red needle color changes the body only; hub stays green; both settings rebuild the full needle geometry | 2026-08-10 |
| CRT scanlines are the topmost live overlay derived from absolute screen y (not region-relative); vignette is a per-pixel post-pass baked in the cache | 2026-07-28 |
| Vault readout is the DELIBERATE dead-zone exception: `update_vault()` (raw hundredths slots + raw sign) and web `drawVaultGauge`/`splitNum(psi, 2)` keep the RAW psi — the ±0.1 fold is user-requested OFF for this theme. Do not "fix" Vault-Tec to fold | 2026-09-06 |

### night-city

| Guard | Since |
|---|---|
| First sample after a scene switch invalidates the complete zero-to-current span (host audit keeps this ordering) | 2026-08-11 |
| `hudTrueBlack` uses `#000000`; track/fill are 15 px at radius 225; tick ends clip at the panel edge; all radii shared | 2026-08-12 |
| Chromatic ghost passes are pre-blended against the face at LV_OPA_COVER (0.7 alpha / 6 px) — do not regress below that visibility contract | 2026-08-11 |
| Readout cache is immutable PSRAM, drained with `lv_draw_wait_for_finish()` before teardown; never republish mutable descriptors | 2026-08-11 |
| `hudGradient` is a quantized whole-fill recolour, not a per-pixel gradient; optimize `worstRenderUs`/pixels/s, never animate the numeric/model value | 2026-08-12 |

### big-digit

| Guard | Since |
|---|---|
| Ground is quantized (24 positive buckets) and recolored in `BIG_BANDS=1` full-width band (one clean jump, no wipe); A/B on `worstRenderUs`, not `renderFps` | 2026-07-28 |
| `bigDigitStaticBg` removes the only full-screen repaint (worst cycle 39 → 7 ms); all four big-digit settings are consumed by the web renderer | 2026-08-10 |

### neon

| Guard | Since |
|---|---|
| Glyphs and marks are A8 coverage tiles blitted with recolor — never opaque tiles for overlapping art | 2026-08-11 |
| Selected preset is the baseline for reset/customized (not the compiled-in Violet entry) | 2026-08-11 |
| Ring bands read dark → bright → white via `NEON_HALO_DIM` (dimmed zone colour) and `NEON_WHITE_LIFT`; web reference derives from firmware constants | 2026-08-11 |
| Tube zero marker is full band depth; peak marker stays track-width (`NEON_TUBE_TRACK_W`) and matches the 0 marker's angular width (`NEON_TUBE_PEAK_DEG == NEON_TUBE_ZERO_DEG`) | 2026-08-12 |
| Segments: 45 × 6-degree slots (4° lit wedges, 2° gaps); lit bands baked as colour-independent A8 tiles | 2026-08-11 |
| Marquee: rings at 176/200/224 (`NEON_BULB_RING_STEP` 24), bulb counts 54/66/72 for uniform chord spacing, cumulative stage ladder, `neonMarqueeSpin` chase; zone flip defers the run repaint one frame (word-first, arc-next-frame) | 2026-08-11 |
| Zone-flip recolor deferral is one frame of old-colour ring, never a multi-frame sweep (user rejected the banded sweep) | 2026-08-11 |
| The zone colour/id decision folds through the readout dead band BEFORE its thresholds (`boost_readout_display_psi` inside `neon_zone_rgb()`/`neon_zone_id()`; web `neonZoneDisplayPsi()` delegating to `arcReadoutDisplayPsi()`): engine-off noise inside ±0.1 psi holds the vacuum colour with zero flicker instead of flipping vacuum/boost every sample and re-firing the deferred zone-flip repaint as second-ring flashing; outside the band raw psi passes through. One band definition — never hysteresis or a second threshold | 2026-09-09 |
| Scene-build caches: background keyed on `neon_bg_key_t` (layout/track/zero), glyph sprites keyed on layout + font; exercise the MUST-rebuild paths, not just the fast path | 2026-08-11/20 |
| Readout invalidation uses the baked sprite footprint, never the label box; marquee scaled anchor + `bbox_s`, full-size `spr_dx` + bbox | 2026-08-09 |
| Doto's signed readout shifts digits 16 px right and keeps 20 px from the custom three-dot minus to the first digit ink; test the tabular widest-digit case and keep firmware/web geometry identical | 2026-08-20 |
| Doto readout glyphs/sign are raw A8 coverage with no baked glow; SF Alien retains the two-pass halo. Keep the established aligned sprite crop for both fonts; shrinking Doto's crop to ink exposed unaligned marquee descriptors | 2026-08-20 |
| Marquee spin and zone-flip invalidation wrap every complete bulb index by `NEON_BULB_N(z)`; wrapping only the residue or leaving `base+1` unbounded strands outer-ring bulb 0 at 12 o'clock for several cycles | 2026-08-20 |

### Units (PSI / bar / kPa)

|Guard|Since|
|---|---|
|A single global display unit (`psi`/`bar`/`kPa`) is persisted in the theme store (NVS key `unit`), served by `GET /api/v1/themes` as `pressureUnit`, and written by `PUT /api/v1/themes/config {"pressureUnit":...}`. It is PRESENTATION ONLY: every canonical value stays PSI (`/config` psiMin/psiMax/psiOverboost, `/state` psi/peakPsi/tpms.*, `/logs`) or kPa (sensor/calibration diagnostics); clients convert at display/input boundaries (`main/boost_units.[ch]` is the firmware's one definition)|2026-10-02|
|Factors: bar = psi x 0.0689475729, kPa = psi x 6.89475729 (the reciprocal of the firmware's 0.145037738 psi/kPa). Decimals: psi 1, bar 2, kPa 0. Gauge geometry (range, `psiToAngle`/`psiToSweep`, zero marker) is ALWAYS in PSI and never rescaled|2026-10-02|
|Readout dead-band fold is defined in PSI and applied BEFORE conversion; vault-tec stays raw in every unit. Sensor/calibration kPa diagnostics are never converted or relabelled|2026-10-02|
|Every face's psi readout path is byte-for-byte the pre-units path (fixed slots, per-glyph ink invalidation). bar/kPa have their OWN fixed-slot grid on arc/vault/big-digit (see the odometer row below); night-city and neon keep a single advances-centred string. Never route psi through the converted layout|2026-10-02, revised 2026-10-03|
|TPMS panel values + app/web TPMS thresholds follow the unit; the low-pressure comparison stays in kPa. A unit change rebuilds the gauge scene (baked unit marks/numerals) and persists|2026-10-02|
| Physical two-finger overlay: the connections page is three square buttons (OBD BLE, APP BLE, UNITS; 2 up + 1 down). Tap toggles/cycles; on-state is a glow + status LED + ON label; the UNITS button's second line is the current unit |2026-10-02|
| Button-origin gestures: the square buttons MUST register `LV_EVENT_PRESSING` into the same shared drag classifier the overlay uses (`qr_pressing_cb`), and a classified drag MUST suppress the trailing `CLICKED` (`s_qr_swipe_suppress`, consumed by both the button tap callback and `qr_click_cb`). Registering only PRESSED/RELEASED silently swallows every drag that starts on a button as a toggle — the pre-round-2 regression, where the documented swipe never reached the tracker because LVGL keeps delivering PRESSING to the active button while the finger stays in its bounds |2026-10-02|
| A unit-cycle rebuild from the overlay MUST re-raise the overlay above `s_media_gif` (`qr_reassert_overlay()`): `boost_gauge_apply_theme()` ends by moving the GIF to the foreground, so "GIF playing → open Connections → tap UNITS" otherwise replaces the open overlay with the paused GIF while `s_qr_active` stays true |2026-10-02|
| App input controls convert to the unit AND round to its contract decimals — an unformatted SwiftUI `TextField(value:format:.number)` renders the raw binary tail (10 psi → `0.689476` bar). iOS uses `PressureUnit.displayRounded(fromPsi:unit:)` for the Range psi fields, the TPMS low stepper binding and its 14.5…58 psi bounds; web `readRangeForm` and Android's field formatting do the same. Rounding is display-only — the model keeps PSI and `PUT /config` always sends PSI|2026-10-02|
| An editable range field MUST retain its canonical PSI value and send it back UNCHANGED when untouched; only a field the user actually edited may convert display→PSI. Reconstructing PSI from the rounded display string is a silent data corruption (`10.0` psi → `0.69` bar → `10.0076`, and `-30.0` → `-2.07` → `-30.0228`, which `boost_model.c` rejects as `psiMin < -30`, so an untouched Save returns 400). All three clients implement it: iOS/Android keep the canonical per field, web keeps `{psi, text}` per field and only reuses the PSI when the field still holds the text it rendered (`writeRangeField`/`retainedRangePsi`). Verified end-to-end: an untouched bar Save PUTs the exact `-15 / 10 / 8`|2026-10-02|
| The decimal contract (psi 1 / bar 2 / kPa 0) is enforced at EVERY client display site, not only at converted ones: a helper that preserves the caller's precision (iOS `Format.pressure`'s `psiDecimals`, web `pressureText`'s `psiDecimals`) leaves psi wrong while bar/kPa look right. Deliberate exceptions are the canonical `/logs.csv` columns (2 dp, wire format) and integer psi chart tick numerals|2026-10-02|
| **A unit-dependent string must be bounded by the geometry around it, and share the psi readout's anchor.** The psi layouts are fixed slots; bar/kPa are longer or differently-shaped, and every theme had a bespoke "centred" fallback that ignored both. Fixed after a board-flash report (bar digits over the neon rings, bar tick numerals over the dyno arc, night-city not right-aligned in kPa): (a) dial numerals (`paint_arc_background`, vault `build_vault`) go through `place_radial_label()`, which pulls a label inward until its far corner is inside `DIAL_NUM_LIMIT` / `VAULT_TICK_MAJOR_IN - 2` — measured BEFORE, the bar arc numerals reached r=183.4 against the band's inner edge at r=177, and AFTER they stop at 172.0; (b) night-city/hud and big-digit right-align the bar/kPa string to the edge the psi odometer grows left from (hud +109, big-digit `BIG_TENTHS_X + BIG_DIGIT_INK_HALF`), so the value no longer jumps: night-city's digits ended at x=339/295/256 for psi/bar/kPa BEFORE and at 339 for all three AFTER. The bound is scene-build only — the 16 ms per-frame path and the arc/wedge geometry the 60 FPS guard was measured against are untouched; psi's own `-15`/`10` numerals move 5.9/2.9 px inward (they were grazing), everything else psi is byte-identical|2026-10-03|
| `pressureUnit` ships in `GET /api/v1/state` (not only `/themes`) and clients adopt it from EVERY state sample: the panel's UNITS button has no other way to reach an already-open dashboard, and a client whose initial `/themes` fetch raced its transport setup (Android cold start) otherwise stays on the stale unit until a manual refresh. `/themes` keeps carrying it for initial config; it is never the live path|2026-10-02|
| A present-but-wrong-typed or non-exact `pressureUnit` is a 400 `invalid_pressure_unit` on BOTH transports (aliases `kpa`/`KPA` are rejected — the wire vocabulary is exactly `psi|bar|kPa`), and the setter is applied only AFTER every other field in the same `PUT /themes/config` has validated, so a later 400 cannot leave the unit persisted|2026-10-02|
| The per-sample big-digit bar/kPa readout formats into a static buffer with integer math and publishes via `lv_label_set_text_static` — never `lv_label_set_text` (lv_malloc/lv_free per value change) and never float `snprintf` on the 16 ms path|2026-10-02|
| Converted Big Digit values keep the custom `s_big_minus` widget: `alvida_big`'s cmap holds only `.` and digits, so a `-` inside the formatted string is painted as nothing and the sign is silently lost (`-1.0` psi → `0.07` bar). Always split sign from magnitude|2026-10-02|
| Big Digit's bar/kPa readout is one label cell PER CHARACTER with a static buffer (`BIG_VAL_CELLS`, `lv_label_set_text_static`). A single `DISP_SIZE`-wide label invalidates its whole 466×120 box on every text change — 0.01 bar steps arrive ~every 15 ms on the 9.789 psi/s sweep, which measured 3.56× the psi flush per cycle (bar 21.2k → 75.7k px/cycle; per-cell cells bring it to 1.81×, kPa 1.11×). Never reintroduce a full-width value label|2026-10-02|
| Peak / max-hold readouts stay RAW: all four peak sites pass `fold_deadband = false` (arc, HUD `PK`, big-digit, neon; vault already false). The ±0.1 psi fold belongs to the LIVE readout only — folding the peak silently changed psi behaviour (`0.05` peak rendered `0.0` instead of `0.1`)|2026-10-02|
| **A converted readout is an ODOMETER: a slot position is a function of a character's PLACE IN THE NUMBER, never of the string's width.** psi is stable only because it prints a fixed-width field of hand-tuned slots; bar/kPa printed variable-width strings laid out from the font's per-glyph advances, so adding a digit, adding the sign, or (on Alvida's proportional digits, '1' 45 px vs '0' 81 px) changing any single digit moved EVERY glyph. Board-reported. Each theme now derives a fixed-slot grid anchored on one of its own psi slots — dyno-cell `VALUE_ONES_X` (-27, pitch `VALUE_DIGIT_PITCH` 43, point `VALUE_DECIMAL_X`, `VALUE_FRAC_PITCH` 36, sign leading digit - 32 = psi's own -59/-102), vault-tec `VAULT_ONES_X` (-12, uniform `VAULT_PITCH` 24 — the mono advance — point at ones + pitch = psi slot 3, sign one cell left), big-digit fixed `BIG_VAL_DIGIT_CELL` 81 / `BIG_VAL_DOT_CELL` 50 right-anchored on `BIG_TENTHS_X + BIG_DIGIT_INK_HALF`. Big-digit is anchored right, not on the units digit, because psi's own worst case minus already reaches face x -205 of 466 and a kPa `-103` at psi's pitch lands at -286 (off-screen); its cells must be `LV_TEXT_ALIGN_CENTER` or a narrow glyph sits at the left of its fixed cell. Verified over 7 pressures x 3 themes x 2 units: every slot stationary (ink centres wobble ≤2.5 px, which is glyph asymmetry inside an advance-centred slot — psi does the same). All 5 themes x 4 psi states are 0-differing-pixel identical to the pre-change binary|2026-10-03|
| **A converted readout must be CENTRED, not merely stable.** Stability alone let the odometer pass anchor kPa's units digit on psi's ones slot — a slot that exists to leave room for psi's `.0`. A fractionless format has nothing to the right of its units digit, so the block sat 27–61 px (arc) and 12–36 px (vault) LEFT of the dial centre (measured) and read as "super far from centered" (board report). A format with no decimal point now anchors on `VALUE_KPA_ONES_X` 27 / `VAULT_KPA_ONES_X` 18 / `BIG_VAL_KPA_RIGHT_X` (psi's edge − 21) — the anchor that puts the block's MEAN optical centre over the kPa range on psi's own mean optical centre (dyno psi measured −3.9 px, vault +0.5, big-digit +2.1; after: +3.6 / +3.1 / +12.4). bar KEEPS psi's anchor because it has a decimal point, and sharing psi's point/ones slots is what stops its digits jumping on a unit change. Any new unit or decimal change must re-derive the anchor, not inherit psi's|2026-10-04|
| night-city and neon are the two themes still on a string layout, deliberately: night-city's psi slots are 52/40 px apart, so bar's hundredths would land at +122 while the readout object's right edge is `HUD_READOUT_OBJ_X2` 117 (LVGL clips a widget to its coords) and the reticle bracket sits at `HUD_BRACKET_X` 126 — pinning the point collides with the bracket, and right-anchoring instead moves the number 52 px on a unit change. Its residual defect is ~8.6 px ('.' 435 and '-' 551 against a digit's 829). neon's block is centred by design. Both need ONE design pass with the panel open; do not "fix" either unilaterally|2026-10-03|
| TPMS unit marks are content-sized labels anchored to the VALUE label's own `lv_obj_get_coords()`; `TPMS_UNIT_DY` is a true pixel gap below the value's measured box, not a hardcoded 24 px that assumed the value font's line height. A fixed-width (`lv_obj_set_width(66)`) label with LVGL's default `LV_LABEL_LONG_WRAP` is a latent clip: the left wheels' right-aligned ink ended exactly on the box's own right edge (x=120) with a host-derived height. PSI keeps the mark hidden and its render is byte-identical|2026-10-03|
| Dyno Cell's `dynoTrueBlack` mirrors Night City's `hudTrueBlack` exactly (NVS `dyno_black`, `/themes` + both `/themes/config` transports, web, iOS, Android) and is read through `arc_face_color()` at the arc canvas fill and the scene root. Like Night City's it is read at scene build, so it lands through the scene rebuild a `/themes/config` PUT already performs. Any new theme option of this kind must cover BOTH the cached face canvas and the scene root, or the rank shows through during a rebuild|2026-10-03|
| `boost_units_format_scaled()` in `main/boost_units.c` is the ONLY integer magnitude formatter (the 16 ms path may not use float `snprintf`); `main/boost_gauge.c` calls it. Pinned by `tools/test_units_format.c` (`test_units_format` CMake target) against `boost_units_format` over psi ∈ [−30, 40] × bar/kPa — never add a second magnitude implementation|2026-10-02|
| A converted readout's SIGN is derived from the PSI value, never from a converted magnitude (`web/app.js:displayIsNegative`, mirroring `boost_units_format`): a `-0.05` threshold applied in bar is really −0.73 psi, so shallow vacuum painted `+0.03` instead of `−0.03`. A value that ROUNDS TO ZERO is not negative (no `−0` at atmosphere)|2026-10-02|
| Tick numerals are a documented cosmetic exception to the decimal contract: trailing zeros are trimmed (bar `1.0998` → `1.1`) and psi ticks stay integer (`5`, `−10`). Both firmware `boost_units_format_tick` and the web tick renderer do this|2026-10-02|

### Media / GIF

| Guard | Since |
|---|---|
| Raw dual-slot store only; committed-header-last atomicity; 409 on overlap; browser aborts then deletes | 2026-07-28 |
| Keep the mmap alive through widget destruction (display lock → destroy widget → unmap) | 2026-07-28 |
| In the dual-buffer pipeline `gif->pFrameBuffer` ALIASES one of the two framebuffers: the destructor must snapshot the ownership pointers before freeing them (else the NULLed `framebuffers[]` pointers defeat the alias check and the buffer is `lv_free`'d twice — Guru Meditation on `DELETE /api/v1/media`), and it must wait out a mid-frame decode (`decode_done` flag, not a fixed 15 ms delay) before freeing anything | 2026-08-16 |
| GIF parsing/LZW/timing is third-party (AnimatedGIF, project-owned copy at `main/gif/boost_gif_dec.c`); only storage, widget integration, the fused LZW-to-RGB565 write path, and the direct push are project code | 2026-07-28 |
| The GIF framebuffer is little-endian RGB565; `boost_display_push_bitmap` byte-swaps to the CO5300 big-endian wire format in the in-place swap on the internal DMA strip (never via the `gif_u32_alias_t` `__aligned__(1)` fused loop — Xtensa has no unaligned word load, so it measures ~10x slower than memcpy, 16.2 ms vs 5-7 ms for a 434 KB stream). The LVGL software fallback renders the little-endian buffer (bridge pre-swaps), so the two paths agree ONLY if the decoder emits little-endian — never pre-swap the palette to big-endian without re-adding a swap to the fallback | 2026-08-16 |
| The frame-rect snapshot (`gifobj->last_fx/fy/fw/fh`, published by the decode task before `sem_frame_ready`) is what the push reads: never let `invalidate_frame` read `gif->iX/iY/iWidth/iHeight` directly on the pipelined path (the Core-1 decoder overwrites them for frame N+2 before the Core-0 push), and the canvas copy before each decode is unconditional (an `is_full_opaque` skip keyed on the previous frame's rect corrupts partial/transparent frames) | 2026-08-16 |
| Framebuffers carry a 16-pixel pad (`GIF_FB_PAD_BYTES`) because the fused 8-pixel-wide LZW stores can run past the last pixel; keep `imgdsc.data_size`/stride at the exact frame size | 2026-08-16 |
| GIF playback is deadline-paced: the next push starts no earlier than `end_of_this_push + D` (timer period `D + last_push`), so a frame is shown for at least its authored delay and playback NEVER exceeds the authored rate. A speed multiplier (`lv_gif_set_speed`, default 1.0) scales only the on-screen hold below the authored delay (0.5 = 2x) — it never starts a frame before the preceding one's scaled window elapsed, and decode/push still run unthrottled. Do not reintroduce `D - last_push` compensation (it overshoots when a heavy full-frame push precedes a delta push), and capture the frame delay before waking the decode task (it overwrites `ms_delay_next` for frame N+2) | 2026-08-16 |
| Direct panel push (`boost_display_push_bitmap()`) is valid only under: display lock held (LVGL task), rotation 0, 1:1 bounded placement, region-dbuf scratch strips allocated; x1/y1 are END-EXCLUSIVE and MUST be rounded to CO5300 2-pixel even/odd boundaries (`dirty.x1` even, `dirty.x2` odd / exclusive `x1` even) before pushing; any refusal falls back to the bounded LVGL invalidation, never a weaker guard | 2026-08-16 |
| Any direct `esp_lcd_panel_draw_bitmap()` source MUST be big-endian RGB565 (byte-swap it; the bridge swaps for LVGL/region-dbuf paths before the hook, direct sources arrive little-endian). When bypassing the adapter bridge, mirror its flush transforms (stride compaction, byte swap) — the custom hook receives pre-transformed data | 2026-08-16 |
| GIF playback is decode-bound (~37 ms decode vs ~16 ms push on the 98%-full-frame fixture); the `perf:` serial line is the measurement harness for any playback work — keep it and quote it | 2026-08-16 |

### WebSocket / telemetry

| Guard | Since |
|---|---|
| Pool is exactly three clients; `state_ws_release_locked()` clears `fd`/`payload`/`inflight` together and bumps `gen` — never release by clearing `fd` alone (permanent slot leak) | 2026-08-01 |
| A fourth client is rejected for itself only; existing clients are never disturbed | 2026-08-05 |
| Per-client 62.5 Hz; pool total 187.5 f/s; never infer pool health from aggregate frames/s (foreign dashboards and undrained sockets confound it) | 2026-08-01 |
| Reboot between WebSocket measurements and verify how many clients actually receive, or the comparison is invalid | 2026-08-01 |
| Badge must expose the active transport; `Disconnected` requires both transports to fail | 2026-07-28 |

### Sensors and calibration

| Guard | Since |
|---|---|
| GM 12223861 transfer function: two-point fit (0.619 V → 40 kPa, 4.818 V → 304 kPa) with ratiometric normalization plus a one-point atmospheric offset; recalibration replaces, never accumulates (`tools/test_map_conversion.py`) | 2026-08-06 |
| Presence flags are never liveness; gate on `ads_age_ms`/`bmp_age_ms`/`ambient_is_fallback`; `UINT32_MAX` means never-read | 2026-08-06 |
| Bus is 100 kHz (MOSFET shifter + 4.7 kΩ pull-ups), in-place `i2c_master_bus_reset()`, bus-admin mutex, four-ACK filter; `/sensors/scan` returns `{busUp,recoveries,found}` — do not reintroduce per-request fixed probes | 2026-08-05 |
| `i2c_master_bus_reset()` takes no internal lock, so EVERY I2C path outside the reader must hold the bus-admin mutex — including the DS3231 read/write (`boost_sensors_rtc_read/write`, bounded 500 ms wait). A live scan is a weak witness (a real device has measured 0-4 of 32 ACKs) so it is capped to `SCAN_TIME_BUDGET_US` (5 s) so a hung bus cannot wedge the httpd task. Recovery (`bus_recover`) must honor the ADS/BMP re-config returns (a failed ADS reconfig silently reads 0 V "successfully") and re-probe devices that were absent at boot — presence is not frozen forever | 2026-08-17 |
| The cadence guard is only meaningful in demo mode; a real sensor at constant atmosphere legitimately reports single-digit `renderFps` | 2026-08-06 |

### OBD2 / BLE / TPMS

| Guard | Since |
|---|---|
| `tpmsBle` default off; a fresh boot never touches the radio; flipping it starts/stops the central live; EVERY toggle surface (panel overlay, web, BLE route) persists via `boost_theme_set_tpms_ble()` before `boost_obd_set_enabled()` — the bare gate call is RAM-only and loses state at reboot (panel-overlay regression fixed 2026-08-28) | 2026-08-12/28 |
| `obd.valid`/`ageMs` track ELM link liveness (any `>`-terminated reply, including `NO DATA`), not decode success — AND require `s_enabled && BLE READY` with the disabled park path republishing every 200 ms, or a runtime disable freezes `valid:true/ageMs:0` (app green "link" forever); DID/PID timeouts ≥2 s sized above the adapter's worst-case search delay | 2026-08-12/28 |
| Do not re-attempt a slower BLE connection interval or `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)`: both were hardware-measured as net-negative; the lag is the inherent cost of a chatty BLE central on one 2.4 GHz radio | 2026-08-13 |
| GATT: discover services first, chars within the service's bounded range, pass the VALUE handle (not `rx+1`) to `disc_all_dscs`, track `s_cccd_found`, and re-run ELM init on reconnect (`init_idx=0`) | 2026-08-12 |
| `OBD_BLE_MIN_DMA_BLOCK` (40,960 B) pre-init guard: never lower it, never let BLE init panic a RAM-starved board into a boot loop | 2026-08-12 |
| BLE and region-dbuf draw from one DMA-internal pool; any display-path change that grows internal DMA must re-check BLE init AND the cadence guard together | 2026-08-12 |
| MX-5 ND TPMS pressure DIDs (0x2A05–0x2A08) answer with a SINGLE data byte (a 4-byte `0x62 DID-hi DID-lo value` response); the UDS parser must accept `length==4` (and may accept `length==5`) — never hard-require a two-byte raw. Mapping: FL=0x2A08, FR=0x2A06, RL=0x2A07, RR=0x2A05 | 2026-08-15 |
| TPMS alert threshold (lowPsi) and staleness (staleAfterMs) are persisted in `boost_tpms` NVS (defaults 220 kPa ≈ 32 psi / 15 s); default staleness is sized above the ~4.5 s poll rotation so a single missed DID never flips the page amber | 2026-08-15 |
| TPMS drawn capsules inflate +2 px (`TPMS_CAPSULE_GROW 2`) beyond the art's tire bounds so anti-aliased white edge pixels cannot peek through | 2026-08-15 |
| Companion `boost_app_ble` GATT surface is the full HTTP API mirror: Control responses up to `APP_BLE_CTRL_RESP_MAX` 4096 B (fragmented, reassembled by clients), `/themes` and `/state` carry the same JSON as HTTP, `/themes/config` PUT handles `neonFont` and echoes the full themes payload, and device-info JSON carries the STA `ip` (`boost_app_ble_set_sta_ip` from `IP_EVENT_STA_GOT_IP`) so BLE-only clients can derive the HTTP host. The iOS/Android apps poll Control `/state` at 1 Hz; the Status char is **read-only** (the no-subscriber 1 Hz notify broadcast was removed 2026-08-28 — do not restore a Notify flag without a consumer). BLE Log is an 8-sample diagnostic window by ATT design — the one-hour ring is HTTP-only. Any further Control expansion requires re-running the physical iOS BLE gate 3/3 | 2026-08-25/28 |
| BLE Control routes for blocking work run on the driver task via APP_EV_* events with HEAP buffers — never inline on the NimBLE host task, never on the driver stack. Blocking routes today: `/sensors/calibration` POST (APP_EV_CALIBRATE), `/sensors/supply` PUT (APP_EV_SUPPLY — NVS commit; an inline route-table entry shipped 2026-08-30 and wedged phone links, and a 4 KB stack body in the first refactor boot-looped the board), `/logs` GET (APP_EV_LOGS), `/network/scan` GET (APP_EV_SCAN). `APP_BLE_CTRL_RESP_MAX` is 4096 — raising it to 8192 panicked twice (LoadProhibited at host-sync; driver-task stack overflow from a 4 KB stack buffer): internal heap is shared with Wi-Fi + LVGL DMA and is not measured-safe above 4096 | 2026-08-28/31 |
| BLE `/logs` returns a compact bounded window `{"tMs","psi"}` decimated EVENLY across the requested `?limit=` (128 pts = cap/32; verbose 42-pt aliased the 9.789 psi/s sweep into staircase pulses at 0.7 pts/triangle). `/network*` routes mirror the HTTP shapes (scan async). Do not return a truncation-wipe or an empty array on overflow — size the budget (worst line 96 B verbose / 32 B compact) so the loop cannot overflow | 2026-08-28 |
| iOS BleTransport resubscribes Control notifications on EVERY write/timeout failure (even retries=0): a board reboot resets the firmware's RAM CCCD while CoreBluetooth keeps its subscription "active" and never re-issues setNotifyValue on auto-reconnect — responses silently vanish (hardware-proven: request reached the board, response fragmented, phone timed out). First request after a board reboot eats one timeout; that is the recovery working. `/logs` uses per-call timeout 20 s + 1 retry (staggered retry ladders re-sent requests whose late responses fed a dead pending). LogsViewModel publishes the cached target window before fetching (stale-while-revalidate) | 2026-08-28 |
| Android BleTransport verifies the encrypted link before the CCCD write and on insufficiency/failure removes the stale bond (reflection), re-bonds fresh, retries CCCD once: the phone caches bond keys across reflashes and BT toggles (`encryption changed status 13` loop — dumpsys shows no bond yet encryption is attempted with the dead LTK). Never reduce connect() back to pre-gatt attemptBond-only | 2026-08-28 |
| The OBD central's scan-retry loop defers new scan bursts while `boost_app_ble_connected()`: 3 s active-scan bursts starved the peripheral notification path mid-fragment (Logs timeouts only with OBD on). Stored-peer connects and real scan hits are NOT gated — the in-car adapter link is unaffected. Do not extend the gate to established OBD links without measurement. A 60 s post-enable fast-retry window re-runs the stored-peer **directed connect** every 2 s (the ELM boots with the ignition; the 10→120 s backoff made cold-start pairing 1-3 min) — never fast-retry a promiscuous scan inside the window, and re-run the 2026-08-15 absent-peer latency soak before release | 2026-08-28 |
| Companion TPMS cards render staleness from the aggregate `tpms.status` (1 = keep last psi, amber #FFB020), never from per-wheel `valid` alone — `--.-` is only for never-received data; mirror the web `app.js` capsule contract on both platforms | 2026-08-28 |

### Boot / NVS / clock / RAM

| Guard | Since |
|---|---|
| The dim schedule must never trust a frozen NVS-restored clock: `s_clock_trusted` is set only by a browser Sync, a monotonic-preserving soft reset, or a valid boot read of the DS3231 RTC; unknown clock → boot bright | 2026-08-14 |
| The DS3231 RTC (0x68, sensor I2C bus) is the battery-backed clock authority when present: `boost_sensors_rtc_read()` rejects oscillator-stop-flag/garbage/implausible time so a fresh RTC never seeds `2000-01-01`; `boost_model_seed_clock_from_rtc()` runs BEFORE boot brightness is decided so a night boot comes up dim with no Wi-Fi; a browser Sync writes the RTC (`boost_sensors_rtc_write`) as calibration and the seed refreshes the NVS epoch checkpoint as a warm fallback | 2026-08-17 |
| `boost_sensors_rtc_write()` MUST clear the DS3231 OSF bit explicitly: unlike some RTCs, writing the time registers does NOT auto-clear OSF (status 0x0F bit 7), so a write that never clears it leaves every read "time never set" and the RTC permanently falls back to NVS. Clear it (read-modify-write under the same bus-admin lock) or the RTC is never the authority | 2026-08-17 |
| `/state.timezoneOffsetMinutes` is owned by `boost_model_refresh_status()` as the DST-EFFECTIVE value; `boost_model_publish_sample()` (16 ms hot path) MUST NOT clobber it with the stored standard offset, or `/state` flickers by one hour across DST (observed 17:44<->18:44). `boost_model_set_time()` and the config-load path report the effective offset too | 2026-08-17 |
| A valid DS3231 is the WRITE authority too: `boost_model_set_time()` rejects a `POST /api/v1/time` epoch more than 5 minutes from the RTC with 409 `clock_rejected` (before settimeofday/NVS/RTC, so a wrong client corrupts nothing); OSF/unreadable/absent RTC accepts any plausible epoch (first seed, battery change). The RTC drifts ~63 s/year, so a >5-min client gap means the client clock is wrong. Corruption within the window self-heals on the next correct sync; beyond it, pull the DS3231 battery to reset OSF | 2026-08-17 |
| The timezone is a POSIX TZ string (`timezoneTz`, e.g. `EST5EDT,M3.2.0/2,M11.1.0/2`) applied via `setenv("TZ")+tzset()`; the dim schedule and CSV use `localtime()` (newlib has no `tm_gmtoff`/`timegm`, so the effective offset is derived by comparing localtime vs gmtime via days-from-civil in `boost_model_utc_offset_minutes_at`). DST is automatic - never re-pick per season. `timezoneOffsetMinutes` stays the standard offset (config/dropdown); `/state` reports the effective offset | 2026-08-17 |
| Panel boots at 0% and ramps to `boost_model_boot_brightness()` after a 100 ms settle — no bright/white flash; hold-to-dim re-applies only on a schedule desired-state transition, never on a fixed cadence | 2026-08-13/14 |
| Two-finger QR (2.2 s hold) depends on the vendored CST9217 two-point read (15-byte read + ACK write, count at byte [5]); displays connected STA IP when associated | 2026-08-14/15 |
| No module's persistence may depend on another module having initialised NVS; test persistence with an actual reboot | 2026-07-25 |
| The main task stack is 8,192 bytes because `app_main()` synchronously builds the persisted LVGL scene; 3,584 bytes overflows during the Neon glyph bake before brightness/network startup. Reboot with Neon persisted and require brightness + `HTTP API ready`, not merely `display ready` | 2026-08-20 |
| Internal DRAM is shared with Wi-Fi and display DMA: measure free internal at peak, keep a hard reserve, and anything that can brick the boot path needs a serial recovery plan before it is flashed | 2026-07-26 |
| Wi-Fi STA scans before connecting across up to 5 saved NVS networks; scans and reconnects are suspended while SoftAP clients are connected to preserve airtime | 2026-08-15 |
| BLE scan runs 25% duty cycle (80 ms interval, 20 ms window, 3 s burst) while disconnected, with an **exponential inter-scan backoff (10 s → 120 s cap) that grows while the stored peer is unreachable and resets on any connect** — an absent adapter must not hammer the shared radio (web p95 191 → 118 ms, max 291 → 169 ms, no timeouts) | 2026-08-15 |
| The shared NimBLE host has exactly one valid bring-up order: **mount (`boost_obd_ble_init`, task start deferred) → register GATT services/host config → `boost_obd_ble_host_start()`**. Never call host APIs before a successful mount, never register after the task starts, and NEVER call `ble_gatts_start()` from app code (the host's own `ble_hs_start` does it; a second call faults — all three hardware-verified as panics 2026-08-23). `appBle` gates advertising only; the legacy adv payload is 31 bytes, so the name lives in the scan response | 2026-08-23 |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` (82 KB DIRAM) buys nothing measurable — it is OFF; the RAM log ring lives in PSRAM, never internal `.bss` | 2026-07-26 |
| A Kconfig symbol existing is not evidence it is being read; verify in the generated `sdkconfig` and on hardware | 2026-08-03 |
| Never configure hardware from `managed_components/` (reverted by any dependency refresh); if a doc claims a hardware setting, there must be a line of code and a boot log to confirm it | 2026-07-26 |
| A timezone whose TZ string is a FIXED offset (`UTC5`) instead of a DST-carrying rule (`EST5EDT,...`) during DST season IS the dim-schedule regression signature: the schedule math was right, the board's local time was an hour off. Fixed-offset TZ strings get written by offset-derived client writes; re-sync with a full POSIX rule and the DS3231 recalibrates in the same write | 2026-09-05 |
| With NimBLE coexistence, `esp_wifi_scan_start()` MUST run with the default scan dwell (leave `scan_time` zeroed): a custom 40/80 ms active time is rejected (`wifi: Should use default active scan time parameter for WiFi scan when Bluetooth is enabled`) and the BLE `/network/scan` route surfaces scan failures/phone timeouts. `boost_network_scan()` retries once (1.5 s) when the background saved-network scan task holds the radio | 2026-09-05 |

### Web / settings / release process

| Guard | Since |
|---|---|
| Web edits are dead until `tools/embed_web.py` is re-run; verify served assets by decompressing, never by grepping the raw response | 2026-08-09 |
| One shared `#errorBox` convention: `showError(msg, source)`/`clearError(source)` with `ERR_USER` outranking `ERR_LIVE`; do not add per-panel status elements | 2026-08-07 |
| Debounced writes that re-render from local state must fold the whole response back into state first, or the render races the save; guard rebuilds over focused stateful controls (colour pickers) | 2026-08-11 |
| OTA verification: boot log must read `Loaded app from partition at offset 0x420000` (ota_1); still booting `0x20000` proves the OTA never ran | 2026-07-25 |
| A documented rate is a claim to verify against the producer, not evidence; treat a timeout passed to `ulTaskNotifyTake` as a ceiling, never a period | 2026-08-03 |
| The sim must run the same init the firmware does (`boost_theme_init()`); when a host harness and the device disagree, suspect harness init before rendering | 2026-08-11 |
| Commit the harness before quoting its numbers — a measurement nobody can re-run is worse than arithmetic | 2026-08-01 |
| Draw escaped marks as shapes, never shell-escaped glyph codepoints; grep for non-ASCII bytes outside comments and for NUL bytes before building | 2026-07-24 |
| Version drift has ONE source: `version.txt` (repo root, bare `MAJOR.MINOR.PATCH`, committed). ESP-IDF bakes it into `esp_app_desc.version` (`project.cmake` `__project_get_revision_from_version_file`), the sim reads it, the mock reads it, and the iOS `project.yml`/generated pbxproj and Android `versionName` mirror it. `tools/tests/test_version_consistency.py` runs in the host suite on the SOURCE surfaces and with `--release` before publishing, where it parses the shipped `boost_gauge.bin` and merged-image app descriptors, the IPA's built `Info.plist`, the APK via `aapt2`, and re-hashes every `SHA256SUMS` entry against the bytes on disk. Never reintroduce a version literal anywhere in code, and never let `project.yml` and the generated pbxproj disagree — `xcodegen generate` from a stale spec silently DOWNGRADES the shipped app (they sat at 0.9.2 vs 0.9.7) | 2026-10-02 |

## Commit hygiene

Keep commits narrow and reviewable: source, generated web output, documentation/ledger, and release artifacts should be separable when practical. Never mix drive-by formatting or unrelated refactors with a regression fix. A commit that changes web sources must include regenerated embedded assets; a commit that changes architecture or a regression must include the README and these guard rails in the same change. Before handoff, report exact files changed, commands actually run, hardware versus host-only evidence, and any unverified risk.

## Cross-platform parity (2026-08-25)

`apps/PARITY.md` is the canonical settings IA + preview spec. Both companion
apps must match it exactly; any UI change lands on both platforms in the same
change-set. Subagents MUST read it before view work and end reports with a
PARITY conformance line.

iOS Forms with Buttons: keyboard dismissal is `@FocusState` (Save drops focus
before sending), the keyboard-toolbar Done button, `.scrollDismissesKeyboard
(.interactively)`, and the window-level cancel-free tap recognizer only —
never SwiftUI tap gestures or `.immediately` scroll dismissal (they eat
button taps; serial-proven 2026-08-31). Android's equivalents (IME Done +
outside-tap `clearFocus`) are the same contract.
