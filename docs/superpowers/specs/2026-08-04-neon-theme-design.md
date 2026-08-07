# Neon Theme Design

## Goal

Add an original neon-inspired gauge face built on the SF Alien Encounters Italic
typeface, shipped as three selectable layouts behind one dropdown and three
colour palettes selectable as ordinary themes.

## User-visible behavior

### Themes

Three new entries join `s_defaults[]`, after Big Digit, all sharing one style:

| id | name | vacuum | boost | overboost |
| --- | --- | --- | --- | --- |
| `neon-violet` | Neon Violet | `#8B3DFF` | `#FF2BD6` | `#FF6A00` |
| `neon-miami` | Neon Miami | `#00E5FF` | `#FF2BD6` | `#FF2A00` |
| `neon-toxic` | Neon Toxic | `#39FF14` | `#FFF000` | `#FF00A0` |

Structural colours per theme: `text` `#FFFFFF`, `zero` `#FFFFFF`, `face`
`#000000`, and `track`/`muted` tuned per palette (violet `#241038`/`#5A3A7A`,
miami `#10222E`/`#3F6E80`, toxic `#12300A`/`#4C7A2E`).

`face` is true black on all three so unused AMOLED emitters are physically off,
the same reasoning as `hudTrueBlack`.

Miami's overboost is deliberately orange-red rather than the hot pink first
mocked. Pink `#FF1466` against boost magenta `#FF2BD6` could not be told apart
at a glance, and the overboost transition is the one cue that must survive
peripheral vision.

Each theme's three zone colours stay user-editable through the existing colour
editor, keyed by theme id. Swiping on the panel cycles through them in table
order along with the existing four.

### Layout selector

One persisted setting, `neonLayout`, chooses the geometry. It applies to all
three neon themes at once, so palette and layout are independent choices.

- `tube` — one continuous glass tube at radius 182, width 26, running the full
  270 degree sweep, lit from the zero notch to the current value.
- `segments` (default) — 56 discrete tube segments at radius 182, width 30; the
  run between the zero notch and the value is lit, the rest sit dark.
- `marquee` — no ring. An oversized numeral, a linear bidirectional bar, and a
  baked border of theatre-marquee bulbs.

**All three carry a labelled scale** at `-15, -10, -5, 0, 5, 10`, with the zero
label in `text` white and the rest in `muted`. Placement differs because the
geometry does:

- tube and segments put the labels *outside* the ring, at radius 220, with the
  tick marks between ring and label. Inside was tried first and does not fit:
  the negative readout reaches 144 px and the ring's inner edge is at 167,
  leaving 23 px of annulus, which a `-10` label overruns into the lit arc.
- marquee puts them under the linear bar with short tick marks, since it has no
  ring to hang them on.

Tube's tick set is `-15, -12.5, -10, -7.5, -5, -2.5, 0, 2, 4, 5, 6, 8, 10`,
with minor ticks drawn shorter and only the six listed values labelled. The
minor ticks are asymmetric by design — vacuum is linear over a wider span than
boost, so evenly spaced minors on both sides would misrepresent the scale.

All three carry: the sliced numeral readout, a `PSI` mark, the zone word
(`VACUUM`/`BOOST`/`OVERBOOST`) in the zone colour, a peak tell-tale, and the
existing `DEMO` indicator when demo mode is on.

The peak tell-tale takes the natural form of each layout, and is coloured by
the zone the peak itself falls in, not the current zone:

- `tube` — a short radial tick outside the tube at the peak angle.
- `segments` — the single segment at the peak angle stays lit while the run
  below it goes dark. One segment, so it costs nothing.
- `marquee` — a thin vertical marker on the linear bar.

Tap-to-reset-peak keeps working unchanged on all three.

The setting appears as a dropdown in the neon theme editor, is served by
`GET /api/v1/themes`, and is accepted by `PUT /api/v1/themes/config`. It
persists in NVS under its own short key, independent of the zone colours;
resetting a theme's colours does not change the layout.

### Readout

The value is rendered in SF Alien Encounters Italic at every layout. Two
properties of that typeface drive the design.

**No sign glyphs.** `hyphen` and `plus` are present in `cmap` but are
zero-contour blanks — the font ships no minus and no plus. Vacuum is 60% of the
scale, so the minus is not optional. It is drawn as a shape, in the typeface's
own slice rhythm: two bars, each `8/128` of the font size tall, on a `10.4/128`
period, skewed to the font's 12 degree italic angle, width `0.34` of the size.
This follows the existing rule that marks a font lacks are drawn as geometry
rather than smuggled in as escaped codepoints. It also costs nothing in glyph
data and scales to any layout.

The gap between the sign and the first digit cell is **per layout**, not a
single shared constant:

| layout | gap | resulting ink gap |
| --- | --- | --- |
| tube, segments | `0.26` of a digit cell | 19.7 px, `0.189` of the numeral size |
| marquee | `0.12` of a digit cell | 17.3 px, `0.111` of the numeral size |

A single proportional constant was tried first and looked wrong. Held at the
same fraction of the numeral size, the sign reads as too tight on the small
readouts and marooned on the large one, because the eye judges the gap against
the absolute size of the face, not against the glyph. Marquee's numeral is
50% larger than the other two, so it needs a proportionally smaller gap to
land at a similar absolute one. Tube and segments share a numeral size and
therefore share a gap.

The sign sits outside the fixed-cell block, so it sets the widest case the
readout ever occupies. The negative case is what the readout is sized against
on every layout, not the positive one.

**Not tabular.** `one` advances 409/1000 against `zero` at 780. Live digits
therefore sit in constant-width cells, each glyph centred inside its cell,
using the same fixed-slot approach Big Digit already uses. Without this the
readout jitters horizontally and the dirty rectangle grows every time the value
crosses a `1`.

The cell block is **centred on the face**; the decimal is not pinned to the
centre. Pinning it is what Big Digit does and it is strictly better in
isolation — nothing on screen ever moves. It does not fit here. Pinning
displaces the block left by half a cell whenever a tens digit is present,
which takes the negative case to 191 px half-width against the segment ring's
167 px inner edge. Centring the block gives 160 px and clears.

What this costs is bounded and small: the block re-centres only when a tens
digit appears or disappears, which on this scale means crossing ±10.0. Within
a digit count nothing moves at all, and because the sign is drawn outside the
block, crossing zero — much the more frequent transition — does not shift the
digits either.

## Architecture

### Style and dispatch

One new enum member `BOOST_STYLE_NEON` in `boost_gauge_style_t`, token `"neon"`
from `boost_style_name()`. Three code sites in `main/boost_gauge.c`:

- `build_scene()` case calling `build_neon(s_root)`;
- `boost_gauge_update()` case calling `update_neon(sample, theme)`;
- a reset/free block in `destroy_scene()`.

The third is the one nothing enforces — `destroy_scene()` is style-agnostic and
unconditionally clears every face's statics, so a new face that omits its block
leaks its canvas across theme switches.

`build_neon()` and `update_neon()` branch internally on `neonLayout`. They are
one face with three geometries, not three faces: the digit-slot code, the
minus, the peak logic, the zone word and the label placement are shared, and
only the ring/bar art differs.

### Rendering

Static art is baked once into a PSRAM `lv_canvas` at scene build, following the
established pattern (`BG_ALLOC`, `lv_canvas_set_buffer` RGB565,
`lv_canvas_init_layer` / `lv_draw_*` / `lv_canvas_finish_layer`). Baked per
layout: the unlit track or segment bodies, tick marks, scale numerals, the
marquee bulb border, and every static glow halo.

Glow is the defining feature of this face and also the single most dangerous
thing to implement, because per-frame alpha is the largest discrete raster cost
in this codebase and translucent full-circle overlays are banned outright. Two
rules follow:

1. Every static glow is baked into the canvas, where cost is paid once.
2. The live lit run is drawn as concentric **opaque** strokes of decreasing
   brightness — halo, mid, core, white centre line — never as alpha over the
   background. This is the same pre-blend technique that took Night City's
   ghost labels from 39 to 45 FPS.

The bake is rebuilt on scene build and freed in `destroy_scene()`, matching
arc, hud and sport. It is deliberately **not** memoised across theme switches
the way Vault's is: Vault earns that complexity because its per-pixel dithered
vignette costs ~1077 ms on first build, whereas this face bakes vector art —
arcs, segments, ticks, text — which is the same class of work arc and hud
rebuild every time without trouble. If the measured build cost exceeds roughly
200 ms, add memoisation then, keyed on range, zero angle, palette, layout and
pixel-shift offset. Not before.

Moving elements stay inside the clip guard radius and are invalidated by
computed bounds, never by invalidating the whole widget:

- `segments` invalidates only the segments between the committed and new value.
  This is the cheapest live update of any face in the project and is the reason
  it is the default.
- `tube` invalidates the arc delta between committed and new angle, reusing the
  existing 90-degree-boundary splitting helper.
- `marquee` invalidates the bar delta plus the readout box.

Every moving element draws from a committed value advanced only when
invalidated, matching the `s_vault_needle_deg` / `s_hud_fill_deg` pattern, and
is seeded from `s_display_psi` at build so the first frame does not jump.

The sweep uses `psi_to_sweep()` so the configured `zeroAngle` is honoured and
vacuum and boost scale independently. Geometry changes go through the shared
40 ms interpolator; the numeric value and the overboost colour transition are
never delayed.

### Fonts

Three generated LVGL fonts via `lv_font_conv`, always with `--lv-include lvgl.h`:

| symbol | size | glyph subset | used by |
| --- | --- | --- | --- |
| `neon_big` | 104 px | `0-9` `.` | tube, segments readout |
| `neon_huge` | 156 px | `0-9` `.` | marquee readout |
| `neon_label` | ~26 px | `A-Z` `0-9` space | zone word, `PSI`, wordmark |

Tube and segments share one generated size at 104 px. Measured on the mockup,
`-12.0` at 104 px is 144 px half-width against the ring's 167 px inner edge, so
it clears by 23 px. Marquee's 156 px readout reaches 204 px half-width, which
is why that layout has no ring: there is no room for one.

Digit-only subsetting on the two large sizes is what keeps this affordable.
Small text below roughly 24 px keeps using the existing `font_cond_*` and
`font_mono_*` faces: the typeface's horizontal slices collapse into mush at
small sizes, and its scale numerals would be less legible than Saira Condensed.

**Flash cost, measured.** Generated with `lv_font_conv` at 4 bpp,
`--no-compress`, and sized by counting the emitted `glyph_bitmap` bytes plus
the 8-byte-per-glyph descriptor tables:

| font | 4 bpp | 2 bpp |
| --- | --- | --- |
| `neon_big` 104 px, 11 glyphs | 11.9 KB | 6.0 KB |
| `neon_huge` 156 px, 11 glyphs | 25.4 KB | 12.6 KB |
| `neon_label` 26 px, 37 glyphs | 3.9 KB | 2.2 KB |
| **total** | **40.2 KB** | **20.7 KB** |

For scale, the same method puts all ten currently shipped fonts at 65.7 KB
combined, of which `alvida_big` alone is 17.2 KB. The current app image is
2,125,296 B against a 4 MB OTA slot, so 40.2 KB is **+1.9% of the image and
2.0% of the remaining headroom**.

Ship 4 bpp at both sizes. The contingency plans — one shared numeral size, or
dropping to 2 bpp — are not needed and are not taken: 2 bpp would coarsen the
antialiasing exactly along the horizontal slice edges that give this typeface
its character, and it would buy 20 KB of a budget with 1.9 MB free. Compression
is likewise declined; it trades flash this design is not short of for CPU at
draw time, which is the resource it *is* short of.

(The ~1.38 MB app image quoted in README is stale — the built binary is
2.03 MB. Worth correcting when this lands.)

### Persistence

`neonLayout` is stored as a `uint8` under its own NVS key (max 15 characters),
loaded in `boost_theme_init()`, defaulting to `segments` when the key is
absent. An out-of-range stored value falls back to the default rather than
indexing past the layout table.

Growing `s_defaults[]` from four entries to seven is safe for existing panels.
`THEME_COUNT` is derived from the array, and the colour-override loader accepts
a stored blob shorter than the current table and matches entries by id rather
than index. A panel that later downgrades to four-theme firmware would find a
seven-entry blob, fail the `n <= THEME_COUNT` check and fall back to built-in
palettes — acceptable, and the same behaviour any table change already has.

### Web mirror

`drawNeonGauge()` in `web/app.js`, dispatched from the `activeThemeStyle()`
switch, mirrors all three layouts including the drawn minus and the fixed digit
slots. The three themes and the `neonLayout` field are added to
`tools/mock_server.py`. The dashboard chrome does not re-skin.

The mirror must reproduce behaviour, not just colour: the shape-drawn sign, the
constant-width digit cells and the layout dropdown all have to behave the same,
since the documented parity bugs in this codebase were all cases of the mirror
matching colours while ignoring a setting.

The typeface is inlined as a base64 `@font-face` in `web/styles.css`, following
the Alvida Fatface precedent. Web source changes regenerate
`main/generated_web_assets.c/.h` in the same commit via `tools/embed_web.py`;
generated files are never hand-edited.

## Verification

The simulator compiles the real `main/boost_gauge.c` on the host, so all visual
verification happens there before any flash cycle:

```
cmake --build sim\build
.\sim\build\boost_gauge_sim.exe --screenshot preview\sim_neon --theme neon-violet
python sim\raw_to_png.py preview\sim_neon
```

Gates, all run per layout because they are three different geometries:

1. Host RGB565 stale-pixel audit reports zero severe mismatches. This is the
   correctness gate for invalidation bounds and the reason the audit exists.
2. Contact sheets for all three layouts across all three palettes, checked at
   `-12.0` specifically. Two known layout risks both appear only in the
   negative case: the readout crowding the ring, and the `-10` and `-15` scale
   labels colliding with the lit arc. Both were caught this way during design.
   Check also that every one of the six labels actually renders — a mismatch
   between the tick set and the label set silently dropped `5` from the tube
   layout, and a missing label is invisible unless looked for.
3. Cadence guard on hardware, comparing `worstRenderUs`, `pixelsPerSecond` and
   `framesOverBudget`, not `renderFps`, which is not a smoothness metric.
   Discard the first samples after a theme change, since scene build is ~50 ms.
   Disable pixel shift while measuring or its periodic full repaint contaminates
   the numbers.
4. Dyno Cell's geometry and wedge invalidation stay byte-for-byte unchanged; it
   is the reference path for the 60 FPS guard.
5. Flash delta reported against the pre-change app image of 2,125,296 B.
   Expected +40.2 KB from the three fonts; a materially larger delta means
   something other than the fonts grew and should be explained before merge.
6. README and the AGENTS ledger updated in the same change, per the project's
   standing rule that a face lands with its documentation.

Targets: `segments` should beat every existing face on `worstRenderUs` given
its bounded invalidation. `tube` should land near the arc face. `marquee`
carries a large readout repaint and is expected to be the weakest; if it cannot
hold the frame budget it ships with a smaller numeral rather than a full-screen
repaint.

## Out of scope

- Animated glow, flicker or pulse effects. A neon flicker would repaint the
  full ring every frame, which is exactly the cost this design is built to
  avoid.
- Theme transition animations. Already ruled out project-wide.
- Applying the typeface to the four existing faces.
- A per-layout colour editor. Colours belong to the theme, layout is a display
  setting, and keeping them independent is the point.
