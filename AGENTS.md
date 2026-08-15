# Agent guidance

This repository is an ESP-IDF 5.5.1 firmware/dashboard for an ESP32-S3 AMOLED boost gauge. These rules are load-bearing. A fresh agent must be able to resume from this file alone; do not rely on chat history.

Current verified release is **`v0.7.1`** (ESP-IDF 5.5.1, app image ~1.5 MB in `release/`). Preserve that identity in hardware and release notes for current measurements. The full historical regression ledger lives in [`docs/regression-ledger.md`](docs/regression-ledger.md); the condensed guard rails below are the currently-actionable invariants, grouped by area. When a change touches one of these areas, re-read the relevant ledger rows for the measurement detail behind the rule.

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
- `main/boost_model.c/.h`: sensor/model state and publication.
- `web/`: dashboard source (cockpit + settings views). `main/generated_web_assets.c/.h` are generated outputs only.
- `tools/embed_web.py`, `web.mk`: web asset regeneration. `tools/mock_server.py` mirrors config/network APIs. `release/`: explicitly produced release artifacts only.

## Cadence contract

Keep these rates distinct; never use one as a substitute for another:

- Sensor sampling and the physical gauge render/update path: every **16 ms**, approximately **60 Hz** (the physical gauge is the hardware gate).
- **Every selectable theme has two cadence gates.** The constant-slew fast-motion sweep (`tools/bench_fast_motion.py sweep --theme <id> [--layout N]`, 9.789 psi/s) is the direct capacity gate: each theme must sustain median physical `renderFps` ≥60; dyno-cell is the reference guard, not a ceiling. The organic demo waveform is demand-aware: `tools/bench_theme_matrix.py` compares `renderFps` with `gaugeDemandPerSecond` (one count per 16 ms gauge update that creates at least one dirty area). PASS requires median demand coverage ≥95% in demanded windows; zero-demand windows are idle, not failures. Firmware without the metric retains the legacy median-60 gate. A real MAP sensor at constant atmosphere invalidates nothing and is not a cadence measurement; demo mode is the precondition for every check. This is a measurement correction, never a throttle. No visual compromise buys frames: a visual-vs-performance trade is a proposal to the user first.
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
- GIF decompression is not project-written. `main/boost_gauge.c` supplies the custom locked widget/descriptor integration; `main/boost_media_store.c` owns raw dual-slot CRC publication and `esp_partition_mmap`; LVGL `managed_components/lvgl__lvgl/src/libs/gif/lv_gif.c` and bundled `AnimatedGIF/src/gif.c` perform parsing, LZW, frame timing, and composition (`GIF_openRAM`/`GIF_playFrame`). Local LVGL edits zero the full framebuffer and disable turbo (`pTurboBuffer = NULL`) because turbo failed delta/disposal composition. Keep mapped bytes alive through widget destruction: lock display, destroy widget, then unmap. Do not describe this as a custom decoder.

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
- Hardware release verification must cover boot, network access, the live physical cadence gate, transport-specific badge behavior, media upload/abort/delete behavior, and serial error absence. Record measured outputs in the README and this file before committing.

## Theme system and physical input

`boost_theme.c:s_defaults[]` is the single authoritative theme order. The web picker and physical swipes consume that order through `/api/v1/themes` and `boost_theme_at()`. The selectable order is **Dyno Cell, Vault-Tec, Night City, Big Digit, Neon** (a vertical swipe up advances to the next entry, wrapping; swipe down moves backward). Sport Cluster was removed outright (renderer, enum member, `"sport"` style token and web mirror) in the 2026-08-10 repo audit; its design history lives in git if it is ever revived. If it is restored, keep its static ring/ticks in the PSRAM `lv_canvas` and its live readout bounded rather than repainting the full face. Theme changes rebuild one LVGL scene under the existing display lock; no full-frame transition buffer or animation is used because it would threaten the 16 ms partial-refresh path.

Gesture classification tracks the greatest signed excursion during `LV_EVENT_PRESSING`, not only the release coordinate. Only movement within the **12 px tap slop** resets peak; movement from 12 through 47 px is a rejected drag; a valid predominantly vertical drag changes one theme at **48 px**; a horizontal drag (≥48 px, 4:5 ratio) pages between boost and TPMS. GIF playback suppresses tap-reset, theme-swipes and page switches, but the one-second hold-to-dim still works over media (only tap/theme/page actions are gated on `media_active()`). A two-finger hold (raw CST9217 point count ≥2 via `boost_display_touch_point_count()`) for 4 s shows the AP-join QR overlay (`WIFI:T:WPA;S:<ap_ssid>;P:boost1234;;`); the 1 s hold-to-dim is suppressed while both fingers are down, and the QR is dismissed by any fresh tap. The screen object survives theme rebuilds, so synchronous `boost_gauge_apply_theme()` from `LV_EVENT_RELEASED` deletes children but not the event target.

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
| Zero notch is a live overlay above the moving value arc, never baked into the cached background | 2026-08-12 |
| Readout invalidation is per-glyph ink box (`arc_readout_ink_box()`/pen math) shared with the draw; re-run BOTH the host audit and a screenshot diff vs the prior build | 2026-08-13 |
| Readout font is 65 px Archivo Black; any size change re-derives pitch/slot from the generated glyph advance | 2026-08-13 |
| Do not reintroduce `transform_scale_x` for glyph widening (AA-seam failure) | 2026-08-12 |

### vault-tec

| Guard | Since |
|---|---|
| Needle seeds from `s_display_psi` on scene rebuild (no jump-to-zero frame) | 2026-08-12 |
| `vaultNeedleTail` defaults off, persisted through theme store/API/web; draw and invalidation share the runtime extent | 2026-08-12 |
| Red needle color changes the body only; hub stays green; both settings rebuild the full needle geometry | 2026-08-10 |
| CRT scanlines are the topmost live overlay derived from absolute screen y (not region-relative); vignette is a per-pixel post-pass baked in the cache | 2026-07-28 |

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
| Scene-build caches: background keyed on `neon_bg_key_t` (layout/track/zero), glyph sprites keyed on layout alone; exercise the MUST-rebuild paths, not just the fast path | 2026-08-11 |
| Readout invalidation uses the baked sprite footprint, never the label box; marquee scaled anchor + `bbox_s`, full-size `spr_dx` + bbox | 2026-08-09 |

### Media / GIF

| Guard | Since |
|---|---|
| Raw dual-slot store only; committed-header-last atomicity; 409 on overlap; browser aborts then deletes | 2026-07-28 |
| Keep the mmap alive through widget destruction (display lock → destroy widget → unmap) | 2026-07-28 |
| GIF parsing/LZW/timing is third-party (LVGL + AnimatedGIF); only storage, widget integration, and the full-canvas zero/turbo-off flush are project code | 2026-07-28 |

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
| The cadence guard is only meaningful in demo mode; a real sensor at constant atmosphere legitimately reports single-digit `renderFps` | 2026-08-06 |

### OBD2 / BLE / TPMS

| Guard | Since |
|---|---|
| `tpmsBle` default off; a fresh boot never touches the radio; flipping it starts/stops the central live | 2026-08-12 |
| `obd.valid`/`ageMs` track ELM link liveness (any `>`-terminated reply, including `NO DATA`), not decode success; DID/PID timeouts ≥2 s sized above the adapter's worst-case search delay | 2026-08-12 |
| Do not re-attempt a slower BLE connection interval or `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)`: both were hardware-measured as net-negative; the lag is the inherent cost of a chatty BLE central on one 2.4 GHz radio | 2026-08-13 |
| GATT: discover services first, chars within the service's bounded range, pass the VALUE handle (not `rx+1`) to `disc_all_dscs`, track `s_cccd_found`, and re-run ELM init on reconnect (`init_idx=0`) | 2026-08-12 |
| `OBD_BLE_MIN_DMA_BLOCK` (40,960 B) pre-init guard: never lower it, never let BLE init panic a RAM-starved board into a boot loop | 2026-08-12 |
| BLE and region-dbuf draw from one DMA-internal pool; any display-path change that grows internal DMA must re-check BLE init AND the cadence guard together | 2026-08-12 |

### Boot / NVS / clock / RAM

| Guard | Since |
|---|---|
| The dim schedule must never trust a frozen NVS-restored clock: `s_clock_trusted` is set only by a browser Sync or a monotonic-preserving soft reset; unknown clock → boot bright | 2026-08-14 |
| Panel boots at 0% and ramps to `boost_model_boot_brightness()` after a 100 ms settle — no bright/white flash; hold-to-dim re-applies only on a schedule desired-state transition, never on a fixed cadence | 2026-08-13/14 |
| Two-finger QR depends on the vendored CST9217 two-point read (15-byte read + ACK write, count at byte [5]); never restore the single-point cap | 2026-08-14 |
| No module's persistence may depend on another module having initialised NVS; test persistence with an actual reboot | 2026-07-25 |
| Internal DRAM is shared with Wi-Fi and display DMA: measure free internal at peak, keep a hard reserve, and anything that can brick the boot path needs a serial recovery plan before it is flashed | 2026-07-26 |
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` (82 KB DIRAM) buys nothing measurable — it is OFF; the RAM log ring lives in PSRAM, never internal `.bss` | 2026-07-26 |
| A Kconfig symbol existing is not evidence it is being read; verify in the generated `sdkconfig` and on hardware | 2026-08-03 |
| Never configure hardware from `managed_components/` (reverted by any dependency refresh); if a doc claims a hardware setting, there must be a line of code and a boot log to confirm it | 2026-07-26 |

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

## Commit hygiene

Keep commits narrow and reviewable: source, generated web output, documentation/ledger, and release artifacts should be separable when practical. Never mix drive-by formatting or unrelated refactors with a regression fix. A commit that changes web sources must include regenerated embedded assets; a commit that changes architecture or a regression must include the README and these guard rails in the same change. Before handoff, report exact files changed, commands actually run, hardware versus host-only evidence, and any unverified risk.
