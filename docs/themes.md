# Theme system

A theme is no longer just a palette swap — each theme carries a **`style`** that selects a **distinct gauge layout**. Selecting a theme changes the whole face, not only its colors, and the selection persists in NVS across power cycles. The selectable order is **Dyno Cell, Vault-Tec, Night City, Big Digit, Neon** (a vertical swipe up advances, wrapping; swipe down moves backward). Sport Cluster was removed outright in the 2026-08-10 repo audit; its design history lives in git if it is ever revived.

| Theme id | `style` | Face |
|---|---|---|
| `dyno-cell` | `arc` | The classic dual-climate arc (teal vacuum · lime boost · flare overboost). Default. |
| `vault-tec` | `vault` | Fallout-style phosphor **needle dial** with CRT scanlines/vignette, a peak tell-tale marker, and an overboost alert (warm numeral + blinking `OVER-PRESSURE`). |
| `night-city` | `hud` | Cyberpunk **targeting HUD**: hazard chevrons, Kiroshi reticle around a big italic value, glitch-shear on fast spikes, MAP/PEAK telemetry. |
| `big-digit` | `bigdigit` | A huge **Alvida Fatface** PSI number in white on a ground that sweeps cyan → lime → red with the reading. |
| `neon` | `neon` | Neon-tube face in **SF Alien Encounters**: a glowing readout over one of three selectable layouts, in one of four colour presets. |

## Shared rules across styles

- **Any filled arc references the configured zero.** Vacuum and boost scale independently and the fill grows from the zero notch, honoring the settings-page `zeroAngle` (Dyno Cell, Vault-Tec, and Night City all obey it).
- **Readouts use fixed decimal positions.** The `big-digit` value is fully tabular (constant-width digit cells, decimal pinned to face center); the decimal, ones, and tenths never move, and higher digits/sign grow leftward.
- **The web dashboard chrome does not re-skin with the gauge.** `setTheme()` drives only the gauge palette (`state.palette`); the console keeps one fixed identity.

## Theme colours and the Big Digit ground

`PUT /api/v1/themes/config` edits the three zone colours (`vacuum`, `boost`, `overboost`) of any theme and persists them in NVS, matched by theme id rather than table index, so a firmware update that adds or removes a theme cannot paint one theme with another's colours. Face, track, text, muted, and zero stay fixed: a theme whose face and text are both user-settable is a theme that can be made unreadable.

For `neon`, its cached background is painted from `track`, which is *not* user-editable but *is* changed by `neonPreset` — so a preset change repaints it. The marquee border's accent bulbs are LIVE (drawn each update in the ring's zone colour once that stage is reached), so a zone-colour edit reaches them via the scene rebuild `apply_theme()` performs; the background cache itself only bakes the neutral `track` bulbs, and `neon_bg_key_t` carries no zone fields. If `track` ever becomes editable, `neon_bg_key_t` already covers it, but that pairing is worth re-checking rather than assuming. `{"id":"...","reset":true}` restores the built-in palette, and each theme reports `customized` in `GET /themes`.

For `neon`, both of those follow the **selected preset**, not the compiled-in entry. The table has to hold one palette as the initial one (Violet), and comparing/restoring against it meant selecting any other preset immediately reported the theme as customized, while reset painted Violet over the selected preset and left the selector pointing elsewhere. The preset is the baseline: selecting one is not a customization, and reset returns to that preset's colours.

The same endpoint carries `bigDigitStaticBg`. Big Digit normally sweeps its whole ground through the zone colours, which is the only full-screen repaint in any face. Turning it off gives white numerals on the theme's face colour and removes that repaint entirely: **worst render cycle 39 ms → 7 ms**, taking Big Digit from the worst-stalling face to the best.

## Neon: layouts and presets

Unlike the other themes, `neon` is one theme with two extra settings rather than several theme entries. Both persist in NVS and both are exposed on `PUT /api/v1/themes/config`.

### `neonLayout` (0–2)

- **`0` tube** — one continuous arc.
- **`1` segments** — 45 discrete segments (6° slots with 4° lit wedges and 2° gaps), the default, smallest dirty region.
- **`2` marquee** — linear bar with a three-ring bulb border (innermost vacuum, middle boost, outermost overboost) and no value ring.

The three rings sit at 176/200/224 (`NEON_BULB_RING_STEP` 24, 1.5× the first spread) so the shared 118 px readout draws scaled to 0.87 (one sprite set, `neon_mq()` scaling). The marquee bakes that 0.87-size A8 sprite set ONCE at scene build (`neon_bake_scaled_sprites`, through the same LVGL transform the per-frame draw used, so the pixels are identical), and the live blits are plain — host A/B measured the per-frame transform at ~35–40% of every readout repaint.

Each ring's bulb count is chosen for UNIFORM chord spacing — inner 54, middle 66, outer 72 (`NEON_BULB_N_INNER/MID/OUTER`, all divisible by 6 so the 2-lit/4-dark accent pattern wraps seamlessly). The border is a **cumulative stage ladder**: ring z's accent bulbs light once the reading has REACHED that zone (vacuum → inner only, boost → inner+middle, overboost → all three); dead bulbs stay dim `track`, so the two-tone look survives even fully lit. The accent anchors are `NEON_BULB_ACCENT_OFFSET(z)` = 0/3/2.

`neonMarqueeSpin` (persisted) makes the accent bulbs CHASE around the rings — one ring advances every 90 ms, round-robin, inner/outer clockwise and middle counterclockwise, a full 6-phase rotation per ring in 1.62 s. The chase repaints only one ring per step (12 small boxes) and defers to zone flips, so it stays inside LVGL's 32-slot invalidation buffer. The pre-scaled readout cut framesOverBudget/s by ~35% and lifted the fast-motion floor. First marquee scene build is ~513 ms; cached returns ~172 ms.

Invalidation is per-glyph, not per-slot: `neon_cell_x_span` / `neon_sign_x_span` ask the baked sprite for its own footprint (marquee: the pre-scaled tile's `bbox_s` at the scaled anchor; tube/segments: the full-size bbox at `spr_dx`) and REPLACE the uniform label box with the tile's exact extent (+1 px AA margin), unioning the old and new glyph footprints when a cell's occupant changes. The marquee bar invalidates only when its DRAWN pixel extent or the zone colour changed, so a static reading goes idle; the live accent scan is skipped when the dirty region cannot reach the rings. Host audit, 25 s, all four variants: **0 severe / 0 stale px**.

### Segments repaint optimization (2026-08-10)

Same-side/same-colour movement invalidates the symmetric difference of the old and new painted segment sets instead of the angular endpoint delta. Each segment's three lit bands are baked once as 162 colour-independent A8 coverage tiles (149,840 B PSRAM) and recoloured at blit time; allocation failure keeps the original live-arc renderer. This materially improved the fast-motion floor and tail (two fresh-boot constant-slew runs 55/59 and 53/59 min/median FPS vs a 28/44 pre-change build with a 174 ms worst maximum), but the combined renderer + `teScanline` candidate still needs a physical-glass tear check before the scanline toggle is accepted.

### `neonPreset` (0–3)

Presets set `track`/`muted` as well as the three zone colours, so changing one repaints the cached background.

| Preset | vacuum | boost | overboost |
|---|---|---|---|
| Violet | `#7B00FF` | `#FF2BD6` | `#FF1500` |
| Miami | `#00E5FF` | `#FF2BD6` | `#FF2A00` |
| Toxic | `#39FF14` | `#FFF000` | `#FF00A0` |
| Blood Moon | `#0064FF` | `#C4172E` | `#FF6A00` |

### Bloom derivation

**Palette entries are the base the bloom is derived from, not what you see.** Everything lit goes through `neon_lit()`: saturate ×1.30 about luma, gain ×1.92, then overflow past full scale desaturates toward white (`NEON_WHITE_LIFT`). Two earlier overflow rules each failed in a way worth not repeating — clamping per channel pinned every saturated entry to the same corner of the colour cube and made the three zones converge; scaling the whole vector back to peak 255 preserved hue but handed back all the gain, so the bloomed body came out *darker* than the raw palette and the ring's band structure inverted in 11 of 12 zones. The ring's inner band is a *dimmed* zone colour (`NEON_HALO_DIM`), which is what guarantees the bands read dark → bright → white outward regardless of palette.

Readout glyphs, the minus mark, and the zone word are baked once at scene build as **A8 coverage tiles** with a box-blurred glow, then blitted with a recolor. Because coverage carries no colour, one set of tiles serves every zone and every preset, and both tiles and the painted background survive theme switches (keyed on layout, and on `neon_bg_key_t` respectively) — without that memoisation, entering `neon` cost ~350 ms against 45–100 ms for other themes.

## UI design tokens

The dashboard chrome keeps one fixed identity (it does not re-skin with the gauge — `setTheme()` drives only the gauge palette):

| Token | Hex | Role |
|------|-----|------|
| `--face` | `#000000` | page background |
| `--rail` / `--line` / `--field` | `#11141a` / `#2b313b` / `#171b22` | console rails, borders, wells |
| `--text` | `#e8ecf2` | primary text |
| `--muted` | `#6b7280` | secondary text / units |
| `--vacuum` / `--boost` / `--overboost` | `#2ee6c5` / `#ffb020` / `#ff3b30` | chrome accent states |
| `--zero` | `#e8ecf2` | zero marker |

Signature move: one arc that **changes climate** at zero and flares past the overboost tick, instead of a generic multi-color rainbow gauge.
