# Display path and cadence

This page documents the AMOLED bring-up invariants and the 60 Hz render contract. The currently-actionable rules also appear in condensed form in the top-level `AGENTS.md`; the measurement detail behind each rule is in `docs/regression-ledger.md`.

## AMOLED display path

**Symptom of the bug we fixed:** the panel stuck half-white / half-green with serial spam:

```
E (...) lcd_panel.io.spi: panel_io_spi_tx_color(...): spi transmit (queue) color failed
E (...) co5300_spi: panel_co5300_draw_bitmap(...): send color data failed
E (...) esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_NO_MEM
```

**Root cause:** the stock Waveshare BSP (`bsp_display_start()` → `esp_lv_adapter_register_display`) sets `profile.use_psram = true` for LVGL draw buffers. ESP32-S3 SPI **GDMA cannot stream from PSRAM**, so every flush allocates an internal DMA bounce buffer and `memcpy`s the strip. Under continuous partial refresh that path returns `ESP_ERR_NO_MEM` and the panel never finishes a clean full-frame update.

**Required path in this repo:**

| Piece | Rule |
|------|------|
| Bring-up | Call `boost_display_start()` from `main/main.c` — **not** `bsp_display_start()`. `panel_new()` also replaces `bsp_display_new()` so the QSPI clock is ours |
| Implementation | `main/boost_display.c` / `main/boost_display.h` |
| LVGL buffers | `use_psram = false` (internal DRAM, DMA-capable) |
| Strip height | `BOOST_LVGL_BUF_LINES=20` → 18,640 B per buffer / 37,280 B double-buffer total |
| SPI max transfer | Cap to one strip (`max_transfer_sz = strip bytes`) |
| Queue depth | `CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH=4` in `sdkconfig.defaults` |
| Mutex | `boost_display_lock` / `boost_display_unlock` (thin wrappers over the LVGL adapter) |
| Brightness | `boost_display_set_brightness()` — **not** `bsp_display_brightness_*`, whose static panel handle is unset once we own the panel |

**Do not:** reintroduce `bsp_display_start()` for the gauge UI; set `use_psram = true` on the LVGL draw profile; raise strip height until the double-buffer no longer fits internal DMA heap; or put full-frame LVGL buffers in PSRAM “for speed” on the QSPI CO5300.

`sdkconfig.defaults` also keeps Wi-Fi/LWIP out of the internal DMA pool (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`). That reservation exists so display DMA and Wi-Fi can run **at the same time**. The BT controller (BLE central) draws from the same DMA-internal pool, so the OBD2 build moves the NimBLE host pools to PSRAM (`CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`), turns off the Wi-Fi IRAM opts, and sizes the controller for one connection (`CONFIG_BT_CTRL_BLE_MAX_ACT=1`); region-dbuf uses **two** internal DMA scratch strips, not four.

Hardware check after flash: serial must show `boost_disp: ... internal DMA buffers` and **zero** `ESP_ERR_NO_MEM` / `send color data failed` during a 20–30 s soak while the demo arc animates. Run the 30-second cadence soak after any display-path change:

```bash
python3 tools/check_display_cadence.py --url http://<BOOST_WEB_IP> --seconds 30
```

## Gauge render contract

The live display is intentionally **marker-free**. `boost_gauge_update()` updates the filled `s_arc_value` and center readout on every 16 ms sample; no timer divider is permitted on either path.

| Invariant | Why it matters |
|---|---|
| Move only the changing arc endpoint between zero crossings | Re-setting the stationary endpoint also invalidates it; this creates avoidable multi-strip flushes. |
| Keep `LV_PART_KNOB` removed | The gauge has no marker/knob; avoid restoring a rendered knob. |
| Do not replace the arc with many `lv_line` segments | Tested on hardware: retained segment objects did not improve cadence and add persistent render work. |
| Update labels only when their formatted text/color changes | Avoids unnecessary text rasterization, while preserving 16 ms readout responsiveness. |
| Preserve LVGL partial mode and the even/odd CO5300 rounder | Full refresh turns a local wedge into a 434 KB full-panel transfer. |

The live gauge remains a 16 ms (~60 Hz) physical path. The verified cadence check recorded a minimum of **61 FPS** and a median of **63 FPS** over 112 samples. This guard defends the physical gauge path; it is not a network telemetry or GIF playback benchmark.

**Every selectable theme has two cadence gates.** The `dyno-cell` guard is the reference, not a ceiling. The constant-slew fast-motion sweep (`python3 tools/bench_fast_motion.py sweep --theme <id> [--layout N]`, 9.789 psi/s) is the direct capacity gate: median physical `renderFps` must be at least 60. The organic demo matrix (`python3 tools/bench_theme_matrix.py`) is demand-aware: it compares completed renders with `gaugeDemandPerSecond`, one count for each 16 ms gauge update that creates at least one dirty area. Its gate is median demand coverage of at least 95% in demanded windows; a zero-demand window is idle, not a failure. Demo mode remains the precondition for both checks — a real MAP sensor at constant atmosphere invalidates nothing and is not a cadence measurement.

Do not reintroduce a marker, throttle the filled arc/readout, or alter the invalidation callback without a before/after hardware measurement. No visual compromise buys frames: a visual-vs-performance trade is a proposal to the user first. Demand-aware acceptance is only a measurement correction; it must never be implemented as a display divider or render throttle.

## Theme optimization campaign (2026-08-09)

Four no-visual-change wins are integrated on `main`: dyno-cell peak-label formatting cache + `set_value_arc()` early-out; vault-tec needle invalidation pad following the tapered wedge; vault readout clip-rejection; night-city HUD fill invalidation using the flat stroke box. All four rendered byte-identical pixels and reduced flushed pixels/cycle; the sweep medians moved within run-to-run noise. Six **visual-vs-performance proposals** are recorded in the ledger (big-digit boundary hysteresis; fewer/wider neon segments; fast-motion tenths sample-and-hold on the marquee; slower spin cadence; vault peak-mark shrink; vault needle-gate raise) — none implemented, all awaiting the user's call.

## Neon flip deferral (2026-08-11)

The neon zone flip recolors the whole lit run in one frame — the widest dirty region on the tube/segments faces. The run repaint is now deferred to the next sample (word-first, arc-next-frame): the word/readout/peak flip immediately, and the ring repaints in the new zone colour one frame later. One 16 ms frame of old-colour ring at each crossing is the accepted visual lag — a single-frame transition, not a multi-frame sweep (the banded sweep was rejected by the user on 2026-08-10). Hardware constant-slew sweeps improved segments 29/45 → 56/60 and tube 45/56 → 54/58; marquee was unaffected as designed. A fresh visual tear check on the physical panel is still required.

## Boost↔overboost crossing

The crossing recolors the **entire lit run** in one frame, so it is the widest dirty region on any face. Measured on the board with `tools/bench_fast_motion.py crossings` (linear sweep, teScanline/regionDBuf ON), the crossing-window `worstRenderUs` median is **dyno-cell 41.5 ms / tube 48 ms / segments 40 ms** with 3–5 frames over budget each. The ~85k-px dirty region is the rect-invalidation floor for a ~135° run (30° chunks are the measured knee; 90°/15°/6° chunks and per-segment boxes all land 85–106k), and the S3's masked-blend/arc-mask rate (~0.5–0.75 Mpx/s) makes a ~30k-px recolor inherently ~40 ms. Opaque RGB565 blits (the only faster primitive, ~3.16 Mpx/s) cannot represent a curved ring without painting its bbox corners. The dyno value-arc invalidation was tightened from 90° rounded boxes to 30° flat boxes with end-cap pads (`invalidate_value_arc`), cutting the dyno crossing worst from 52 ms to 41.5 ms. Sub-20 ms at the crossing is not reachable for a single-frame full-run recolor on this hardware; it requires spreading the recolor across frames, the visible two-tone transition rejected for neon on 2026-08-10.

## Measured strip-height boundary

The original 20-line setting is the production configuration. We tested larger internal-DMA strips with Wi-Fi enabled and the full-rate physical gauge running:

| Buffer height | Internal DMA draw-buffer allocation | Result |
|---:|---:|---|
| **20 lines** | **18,640 B per buffer; 37,280 B double-buffer total** | **Selected.** Physical gauge 56–63 FPS; browser-style polling p50 9.5 ms, p95 19.0 ms. |
| 24 lines | 22,368 B per buffer; 44,736 B double-buffer total | Physical gauge 56–63 FPS; polling p50 9.2 ms, p95 16.4 ms. No measured physical-FPS gain. Reverted because the live mirror was reported subjectively laggier. |
| 28 lines | 26,096 B per buffer; 52,192 B double-buffer total | HTTP became unresponsive under load. Rejected. |
| 40 lines | 37,280 B per buffer; 74,560 B double-buffer total | Wi-Fi route disappeared after flash/load. Rejected. |

Treat perceived mirror responsiveness as a web telemetry or canvas-rendering issue, not a display-strip throughput win. Do not raise the strip height without both physical-FPS and browser-mirroring measurements.

## QSPI clock: 80 MHz

`BOOST_LCD_PCLK_HZ` in `main/boost_display.c` sets the CO5300 QSPI clock to **80 MHz**, against the driver's 40 MHz default.

An earlier revision of this section claimed a "60 MHz trial" was active. It was not, and never had been: the clock lives in `CO5300_PANEL_IO_QSPI_CONFIG` inside a managed component, `bsp_display_new()` takes that default with no override hook, and any edit to `managed_components/` is silently reverted by a dependency refresh. Everything measured before this change was running at 40 MHz. Do not reintroduce the clock there.

The bring-up is therefore vendored: `panel_new()` in `boost_display.c` is `bsp_display_new()` with `pclk_hz` under our control, plus a copy of the 14-entry vendor init table. Because `bsp_display_brightness_*` writes through a file-static panel handle that only `bsp_display_new()` assigns, it returns `ESP_ERR_INVALID_STATE` once we own the panel; brightness is `boost_display_set_brightness()`, the same single 0x51 command.

Measured on hardware, worst render cycle:

| | 40 MHz | 80 MHz |
|---|---:|---:|
| Big Digit (full-screen recolour) | 45.1 ms | **37.6 ms** |
| Dyno Cell | 43.2 ms | 41.7 ms |
| Night City | 36.8 ms | 37.3 ms |

The gain lands **only on full-frame pushes**. Partial-update faces are bound by CPU rasterisation with dirty regions far too small for the link to matter, and move 0–3%. Treat this as a fix for one operation, not a general lever. Boot must log `panel up at 80 MHz QSPI` with no `co5300` errors; if the panel ever shows sparkle or torn rows, drop back to 40 MHz first.

## Fast-motion cadence (regionDBuf ON)

The whole-run `dyno-cell`/demo cadence guard averages over a sweep whose slew rate varies continuously, hiding the regime a user actually watches the needle react to: the fast segments. Fast motion is isolated with `tools/bench_fast_motion.py` (a controlled constant-slew triangle sweep at 9.789 psi/s, plus an empirical velocity-correlation against the unmodified organic demo waveform — both committed and reproducible). The direct capacity gate is the constant-slew sweep; the organic gate is demand-aware (`gaugeDemandPerSecond`), never a throttle.

Two structural results stand:

- **`teScanline` (runtime toggle, default OFF)** is the dynamic CO5300 `set_tear_scanline` (0x44) writeback: when a burst cannot prove EARLY/LATE, it programs the tear line just past the dirty region's bottom so the TE edge arrives as soon as the scan clears the band, instead of waiting up to a full ~16.75 ms V-blank period. Set via `PUT /api/v1/themes/config {"teScanline":true}`. It helps the wide neon flip bands but does not by itself reach locked 60 on segments/tube. **A fresh tear check on glass is required before leaving the toggle ON.**
- **Vault needle raster**: exactly two triangles (a redundant third added no geometry but paid LVGL's mask path — same-firmware A/B: 60/59 vs 56/55 FPS median), with three invalidation segments.

See `docs/regression-ledger.md` for the full fast-motion numbers, the per-span TE-wait fix and its first-cut regression, and why a RAMWRC-based per-chunk command reduction is not shipped.
