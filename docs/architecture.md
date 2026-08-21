# Firmware architecture

`main/boost_gauge.c` is a **scene dispatcher**, not a single face.

## Scene model

- `build_scene(style)` / `destroy_scene()` construct and tear down a per-style LVGL object tree. `boost_gauge_update()` dispatches to the matching `update_*()`. `s_built_style` records what is currently on screen.
- **Changing theme rebuilds the scene** rather than recolouring, because each style is a different object tree. `PUT /themes/active` therefore calls `boost_gauge_apply_theme()` under the display lock — without it the picker only took effect after a reboot.
- The `arc` face keeps its original geometry and wedge-invalidation logic verbatim; it is the path the 60 FPS cadence guard was established against.

`app_main()` synchronously builds the persisted scene before it returns. The main task stack is therefore 8,192 bytes; ESP-IDF's 3,584-byte default overflowed while baking a persisted Neon scene before brightness or networking could start. A Neon cold boot must reach brightness and `HTTP API ready`, not merely `display ready`.

## Zero reference

`psi_to_angle()` maps the arc face. `psi_to_sweep(psi, a0, a1)` projects the same zero-referenced scaling into any other sweep, so Vault-Tec and Night City honour the configured `zeroAngle` and scale vacuum/boost independently, exactly like the arc.

## Draw/invalidate must agree

Every moving element draws from a *committed* value that is only advanced when the element is invalidated (`s_vault_needle_deg` / `s_vault_needle_over`, `s_hud_fill_deg` / `s_hud_fill_psi`). Drawing from the live sample while invalidating on a threshold leaves stale pixels — that was the source of the smearing artifacts. The needle also repaints on an overboost **colour** flip, not only on angle change.

## Clip guard

`clip_reaches_radius(layer, r)` reads `layer->_clip_area` (its documented use during draw-task creation) and lets a face skip its outer ring art when only the centre is dirty — the common per-frame case, since digit updates cannot touch a tick ring at r ≥ 194. The Vault needle is deliberately shorter than the tick ring so its dirty rect stays inside the guard.

## Cost model (measured on hardware)

Repainting the full 466×466 face is ~217k pixels; sustained throughput is ~0.9 Mpx/s, so a full-face repaint costs roughly a quarter second. Anything that recolours the whole panel (the `bigdigit` ground) is therefore inherently a hitch and must be quantized. This is also why translucent full-circle overlays were removed: five vignette rings re-blended ~107k px on every needle frame and dropped Vault from 60 to 37 FPS.

## Cached static faces

Vault-Tec, Night City, and Dyno Cell each paint their entire static face **once** into a 434 KB PSRAM `lv_canvas` at scene build (`paint_vault_background`, `paint_hud_face(..., cached=true)`, `paint_arc_background`); every later redraw is a blit rather than re-rasterising vectors. This is the single biggest lever available, because the bottleneck is CPU rasterisation, not the panel link. Measured: Vault median 37 → 61 FPS, min 4 → 54, while *adding* the vignette and scanlines; Night City 32 → 37. Elements that move (needle, fill arc, peak tell-tale, digits) stay as separate objects on top; only genuinely static art belongs in the cache. Vault's completed canvas remains allocated across theme switches because its serial per-pixel error-diffusion vignette is expensive to recreate; range, zero angle, palette, face, and vignette values form its cache key, so a setting change still repaints it once. Other faces free their cache in `destroy_scene()`.

Dyno Cell's cache covers the unfilled track ring, the zero notch, the five scale numerals, and the static "PSI" mark — the value wedge (`s_arc_value_canvas` / `draw_value_arc`), the readout digits, peak, and zone label stay live above it. The value wedge invalidates from its committed smoothed geometry while raw pressure still controls the numeric/color state. Because range and zero-angle move the numerals and notch, `boost_gauge_apply_config()` no longer special-cases arc with incremental patches; it takes the same `destroy_scene()` / `build_scene()` rebuild path as vault/hud/bigdigit on a config change, so the cache repaints rather than keeping stale numerals. Measured hardware, demo mode, 3 fresh boots each, 30 s polling windows of `/api/v1/state`: `worstRenderUs` max 58.4–63.7k µs / mean ~24–25k µs before, 43.8–51.3k µs / ~22.7–23.1k µs after; cadence guard min 56–57 → 58–59, median 60 both. Host audit (`--theme dyno-cell --seconds 25`) is unchanged before/after.

Corollary: effects that would be prohibitive per-frame (vignettes, texture) are essentially free once baked. Prefer baking over per-frame drawing.

**Caching does not help a flat fill.** Big Digit's ground is a solid colour, so there is no vector art to pre-compute — a fill is already the cheapest primitive LVGL has, and 24 cached full-screen grounds would want 10 MB besides. Its cost is the unavoidable 217k-pixel repaint when the colour steps. The fix is to *spread* that repaint, not to precompute it: `BIG_BANDS` full-width bands take the new colour on successive ticks, so one long stall becomes several short ones. This is a direct trade — the shorter the stall, the more the transition reads as a visible wipe:

| `BIG_BANDS` | Worst cycle @40 MHz | Transition |
|---:|---:|---|
| **1** | 45.1 ms (**39 ms** at the current 80 MHz) | **selected** — one clean jump, no wipe |
| 2 | 33.3 ms | a single horizontal split |
| 4 | 25.8 ms | a clearly visible top-to-bottom sweep |

Banding and the clock attack the same stall independently, so `BIG_BANDS = 1` at 80 MHz lands near what 2 bands bought at 40, with no visible transition at all. Total work is conserved, so `renderFps` barely moves between these. A/B any change on `worstRenderUs`.

## RGB565 gradient limits

**RGB565 has too few levels for a gradient over a dark colour.** Vault's face is `#02100a`, whose green sits at **level 4 of 63** in RGB565, so a mathematically exact vignette still resolves to four flat rings on the panel. The web mirror looks smooth only because canvas is 8-bit. The fix is a serpentine Floyd–Steinberg-style **error-diffusion** pass in `paint_vault_background`, with two error rows carrying RGB565 quantisation error into neighbouring pixels. The pattern is baked into the cache so it never shimmers. Any future smooth ramp over a near-black colour needs the same treatment; check the source channel's 565 level before assuming a banding report means the gradient maths is wrong.

## Match the web mirror's method

Vault's CRT treatment is a direct port of `drawVaultGauge`: the vignette is a smooth radial ramp from r=120 to r=233 reaching 60% black, and the scanlines are 1 px rows every 4th line at 16% black, both chord-clipped to the face. The vignette is applied as a **per-pixel pass** over the canvas buffer after the vector art. The scanlines are **not** baked; they are drawn last (`draw_vault_crt`, the topmost child) so they cross the needle and digits as on the web. Their phase comes from absolute screen y, which stops neighbouring dirty regions from disagreeing and producing tearing (an earlier per-region version showed). Cost of the on-top pass: ~1 FPS.

## Invalidate what changed, not the widget

Night City's ghost pass repainted a fixed 310×100 box on every value change; bounding it to the slots that actually changed (usually the tenths alone) took the style from 37 to 42 FPS median and 22 to 31 min. `HUD_GLITCH_DX` is shared between draw and invalidation so the dirty box can never be narrower than the pixels the ghosts touch.

Night City's gradient fill has a measurable but secondary cadence cost. The fill is one quantized solid colour, and each colour-step crossing invalidates and recolours the complete zero-to-value arc. The retained mapping uses one fixed vacuum sentinel plus **24 buckets devoted exclusively to positive pressure**: bucket 1 starts immediately above zero and bucket 24 lands exactly at configured max; firmware, Big Digit, and the web mirror share the same ceiling-based mapping. Reproduce a gradient comparison with `tools/bench_hud_gradient.py`; judge an optimization on render time, pacing, and pixels/s rather than FPS alone.

The ghost's two semi-transparent 96 px label passes were the largest discrete raster cost, so they now pre-blend their colours against the known static face and draw opaque, preserving offsets, glyphs, and dirty regions while avoiding two destination read/blend passes. This intentionally approximates the original alpha composition where the two shifted copies overlap; it is not framebuffer-identical.

The readout cache is one scene-owned, immutable PSRAM block of **34,786 B** containing only digits 0–9, decimal, and minus, reused by the primary readout and ghost pass; surrounding HUD labels keep their source fonts. Draw callbacks publish stable descriptors only; the block is never mutated or republished. Lifecycle is draw-unit safe: drain with `lv_draw_wait_for_finish()` before scene teardown or cache release, and keep the cache alive until all draw units finish. Allocation failure retains the source-font fallback. `BOOST_HUD_READOUT_CACHE=0` provides a compile-time source-font A/B/fallback guard.

Vault-Tec deliberately keeps the same consolidation pattern in a bounded **146×34** six-slot readout object together with `VAULT_NEEDLE_SEGS=3` and the hardware-proven two-triangle needle raster. Two invalidation segments clean up host antialiasing seams but enlarge the dirty regions and worsened the matched hardware tail, so the kept production choice is the bounded consolidated readout plus **three** invalidation segments. Future Vault changes must rerun both the RGB565 stale-pixel audit and interleaved hardware cadence measurements.

The retained Vault renderer removes the **26 px counterweight tail** behind the pivot by default. A persisted `vaultNeedleTail` option in the Vault theme editor and `/themes/config` API restores it when desired. The wedge tip length, 15 px hub, two-triangle raster, three invalidation segments, AA padding, CRT overlay, and 146×34 readout remain unchanged. Draw and both invalidation paths resolve the same runtime tail length; applying the setting rebuilds the scene so old foreground pixels cannot remain. The browser mirror follows the same geometry.

## Geometry animation

Both physical arcs animate geometry through one shared latest-target linear interpolator. Each new sample remains the exact target immediately; the visible endpoint retargets from its currently displayed position over a fixed 40 ms animation, cannot overshoot, and snaps on initialization, scene rebuild, non-finite input, or a gap over one second. There is no adaptive EMA, alpha cap, or small-change suppression. Only geometry is animated: raw pressure still drives numeric readouts, peak/model/API state, and immediate colour/threshold transitions. Draw and invalidation use the same committed visual state.

Two web-mirror fixes closed remaining parity bugs: Night City's chromatic split had become visually negligible at **0.4 alpha / 3 px**; it now uses **0.7 / 6 px**. The Big Digit renderer ignored `bigDigitStaticBg`, `staticColor`, `colorText`, and `textColor`; it now consumes all four settings.

Night City's colour menu also offers a persisted **True black background** checkbox: it replaces the default `#080A08` face with `#000000`, turning unused AMOLED pixels fully off; the cached physical face, screen background, chromatic ghost pre-blend, and browser mirror all consume the same setting. Its gauge track and live fill are **15 px** thick (up from 10 px), with the wider physical invalidation bounds kept in lockstep. The ring is pushed outward from radius 206 to 225; its unchanged 16 px ticks move from radii 198–214 to 217–233 so their outer ends meet the panel edge.

Night City's first sample is the exception to endpoint-only invalidation: after a scene switch it invalidates the complete zero-to-current span, otherwise a rebuild rendered before the first vacuum sample left the fill empty. The host audit deliberately switches into Night City from an already-negative reading to preserve this case. On hardware, the first Vault build took 1077 ms, while three cached returns took 64–101 ms; the Dyno Cell cadence guard after a 3 s settle passed at min 58 / median 61 FPS over 104 samples.

Vault-Tec also seeds its newly-built needle from the last committed pressure on a theme-switch rebuild. Previously the object started at zero for one frame and jumped on the next sample. The simulator audit switches into Vault-Tec at +8.0 PSI before the next gauge update and requires that update to move the needle by 0.000 degrees.

## Reading the metrics

**`render_fps` is not a smoothness metric.** It counts `LV_EVENT_RENDER_READY`, i.e. render cycles actually performed; LVGL only renders when something is invalidated. A face whose content changes less often legitimately reports a lower number while using *less* throughput. Compare `pixelsPerSecond` before concluding a face is too slow. For the organic demo, compare `renderFps` with `gaugeDemandPerSecond`: only a shortfall in a demanded window is a cadence defect. The constant-slew capacity gate remains a direct median `renderFps >= 60` check.

**`worstRenderUs` is the choppiness metric.** It is the longest single `LV_EVENT_RENDER_START` → `LV_EVENT_RENDER_READY` interval in the reporting window: the duration of a cycle, not the gap between cycles. Measuring the gap instead was the first cut of this metric and it was useless, because an idle screen produces long gaps and no stall at all.

## Where the pieces live

- **Web mirror:** `web/app.js` dispatches `drawGauge()` on `activeThemeStyle()` to the active renderer. `tools/mock_server.py` serves the five selectable themes (each with a `style` field). Regenerate embedded assets after any web edit. `setTheme()` drives only the gauge palette, so the dashboard chrome keeps one fixed identity.
- **Simulator:** `sim/` builds the same `boost_gauge.c` on the host and renders headless PNGs, including the generated fonts. SDL2 is optional (only `--window` needs it); `--theme <id>` selects the style, `--neon-layout tube|segments|marquee` and `--neon-preset 0..3` the neon face and colourway, and `--neon-spin` enables the marquee chase (with `--neon-chase DIR` writing a fixed-psi 90 ms/step chase sequence). Both flags exist because the sim does **not** inherit the device's persisted settings: it calls `boost_theme_init()` at startup, and without them it renders the defaults. This is the fastest verification loop; use it before burning a flash cycle.
- **Font:** `main/fonts/alvidafatface-regular.otf` is kept as OTF (CFF master, ~4× smaller, converts cleanly with `lv_font_conv`). The dashboard loads it as an inlined base64 `@font-face` (`"Alvida Fatface"`) prepended to `web/styles.css`.

## Fonts

`main/fonts/` holds generated LVGL fonts; sources live there, under `main/fonts/src/`, or in `web/` when the dashboard serves the same file. All are OFL so they can ship in the image (the web mirror's Bahnschrift/Consolas are Microsoft fonts and cannot be embedded):

| Font | Source | Use |
|---|---|---|
| `alvida_big` | Alvida Fatface | big-digit numeral |
| `font_mono_16/40` | IBM Plex Mono SemiBold | readouts, telemetry |
| `font_cond_14/18/22/32` | Saira Condensed SemiBold | labels |
| `font_cond_96` | IBM Plex Sans Condensed BoldItalic | Night City readout |
| `archivo_black_65` (65 px) | Archivo Black (Google Fonts) | Dyno Cell readout |
| `font_wide_22/32` | Saira SemiCondensed Bold | Big Digit labels |
| `neon_big` (118 px) | SF Alien Encounters Italic | neon readout, all three layouts |
| `doto_big` (126 px) | Doto ROND 100 / weight 700 | optional neon readout, all three layouts |
| `neon_label` (24 px) | SF Alien Encounters regular | neon zone word and `P S I` |

The neon readout is italic; the zone word and `P S I` are the **upright** face, so `main/fonts/` carries both `SFAlienEncounters-Italic.ttf` and `SFAlienEncounters.ttf`. The web mirror inlines both under one family name and selects between them purely by the presence of the `italic` keyword.

The neon fonts carry only the glyphs they use. SF Alien's minus is drawn from bar geometry; Doto's uses three period-sized dots. Their sizes are not free parameters — the readout cell pitch, sprite tile size, and invalidation bounds are derived from the generated `line_height` and widest glyph ink. Regenerate Doto with `python tools/generate_doto_font.py`; its SIL OFL 1.1 license is `web/OFL-Doto.txt`.

Regenerate with `lv_font_conv`, always passing `--lv-include lvgl.h` (the default `lvgl/lvgl.h` does not resolve against the managed component). Glyphs missing from a face can be merged from a second `--font`. Do not try to embed box-drawing/arrow codepoints through shell escapes — draw such marks as shapes instead; escaped UTF-8 has repeatedly been mangled a layer early.
