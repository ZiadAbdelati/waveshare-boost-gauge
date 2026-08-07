# Neon Theme Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship an original neon gauge face on SF Alien Encounters Italic as one `BOOST_STYLE_NEON` with three selectable layouts and three colour palettes.

**Architecture:** Pure geometry (digit-cell layout, sign-mark bars, segment index maths) lives in a new dependency-free `main/boost_neon_geom.c`, unit-tested on the host. Rendering follows the existing house pattern in `main/boost_gauge.c`: a PSRAM `lv_canvas` bake for static art plus `LV_EVENT_DRAW_MAIN` callbacks for live parts, with a single `build_neon()` / `update_neon()` pair branching on the persisted layout.

**Tech Stack:** C99, ESP-IDF, LVGL 9, `lv_font_conv` (npx), CMake + Ninja + mingw for the host sim, vanilla JS canvas-2D for the web mirror, Python for the mock server.

**Design spec:** `docs/superpowers/specs/2026-08-04-neon-theme-design.md`. Read it before starting.

## Global Constraints

- Panel is 466x466 RGB565 round AMOLED. `DISP_SIZE` is 466. Frame budget is 16 ms.
- **Never** use per-frame alpha for glow. Layer opaque strokes instead. Translucent full-circle overlays are banned.
- **Never** draw directly to RGB565 from `LV_EVENT_DRAW_MAIN`.
- All static art is baked into a PSRAM canvas once at scene build and freed in `destroy_scene()`.
- Centre coordinates come from `px_cx()`/`px_cy()`/`px_icx()`/`px_icy()`, never `DISP_SIZE/2`. Inside a baked canvas, subtract `s_px_dx`/`s_px_dy` (see `paint_sport_background()` line 3296).
- Moving elements draw from a committed value advanced only when invalidated, and are seeded from `s_display_psi` at build.
- Scale mapping uses `psi_to_sweep(psi, a0, a1)`. Never write a fresh linear map.
- Dyno Cell (`arc`) geometry and wedge invalidation must stay byte-for-byte unchanged — it is the reference path for the 60 FPS cadence guard.
- NVS keys are 15 characters maximum.
- Generated files (`main/generated_web_assets.c/.h`, `main/fonts/*.c`) are never hand-edited.
- Work on branch `feature/neon-theme` in the worktree at `.claude/worktrees/new-theme-vac-boost-over-98343f`.

### Worktree build prerequisites

**A fresh git worktree cannot build the simulator until this is done.** Both
`managed_components/` (238 MB of LVGL, created by the first ESP-IDF build) and
`sim/build/` are gitignored, so they exist only in the main checkout. A worker
that skips this hits "sim/build is not a directory" and cannot reach a
compile at all.

The junction avoids copying 238 MB and is safe — `managed_components` is
read-only build input:

```bash
cmd //c mklink //J "C:\Users\aliab\boost-gauge\.claude\worktrees\new-theme-vac-boost-over-98343f\managed_components" "C:\Users\aliab\boost-gauge\managed_components"
```

mingw must be on PATH for the CMake configure to find a compiler:

```bash
export PATH="$PATH:/c/Users/aliab/AppData/Local/Microsoft/WinGet/Packages/BrechtSanders.WinLibs.POSIX.UCRT.LLVM_Microsoft.Winget.Source_8wekyb3d8bbwe/mingw64/bin"
```

Then configure once:

```bash
cmake -S sim -B sim/build -G Ninja
```

This is already done in the current worktree. Re-run only if `sim/build`
disappears.

### Palette values (exact)

| id | name | face | track | text | muted | vacuum | boost | overboost | zero |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `neon-violet` | Neon Violet | `0x000000` | `0x241038` | `0xFFFFFF` | `0x5A3A7A` | `0x8B3DFF` | `0xFF2BD6` | `0xFF6A00` | `0xFFFFFF` |
| `neon-miami` | Neon Miami | `0x000000` | `0x10222E` | `0xFFFFFF` | `0x3F6E80` | `0x00E5FF` | `0xFF2BD6` | `0xFF2A00` | `0xFFFFFF` |
| `neon-toxic` | Neon Toxic | `0x000000` | `0x12300A` | `0xFFFFFF` | `0x4C7A2E` | `0x39FF14` | `0xFFF000` | `0xFF00A0` | `0xFFFFFF` |

### Geometry constants (exact, from the approved mockups)

**LVGL's `lv_draw_arc_dsc_t.radius` is the OUTER radius of the stroke, and that
is exactly what this design wants.** All three strokes are drawn at the same
`radius` with different widths, so they share an outer edge and stack inward:

| stroke | width | occupies | reads as |
| --- | --- | --- | --- |
| white core | 6 | 176-182 | bright top surface |
| body | `W` | 152-182 | the lit colour |
| halo | `W + 14` | 138-182 | shadow falling inward |

That stacking is what gives each lit segment its three-dimensional look. PIL's
`ImageDraw.arc` extends its width inward from the radius in precisely the same
way, which is why the mockups and the firmware agree without any adjustment.

**Do not "centre" these strokes on R.** An earlier revision passed
`radius = R + width/2` on the theory that `radius` meant the centreline. It does
not. That change put the white band in the middle of the segment with colour on
both sides, flattened the shadow, and moved the whole ring outward from 152-182
to 167-197, which also threw off the readout spacing. The band structure below
is the correct target; verify against it after any change to the ring.

```
Ring outer radius        R = 182   (outer edge of every stroke; see above)
Tube stroke width        26
Segment stroke width     30, NSEG = 54, inter-segment gap 2 degrees (all integer)
Scale label radius       220, font F_COND18, values -15 -10 -5 0 5 10
Tube tick radii          major R+16..R+26, minor R+18..R+24
Tube tick set            -15 -12.5 -10 -7.5 -5 -2.5 0 2 4 5 6 8 10 (labelled: -15 -10 -5 0 5 10)
Readout tube/segments    neon_big 104 px, slot 62, dot 32, sign gap 10, sign width 30
Readout marquee          neon_huge 156 px, slot 94, dot 48, sign gap 11, sign width 53
Sign mark                2 bars, bar height size*8/128, period size*10.4/128, skew tan(12 deg) = 0.2126
Marquee bar              300 x 16, centred, 76 px below face centre
```

---

### Task 1: Neon geometry module

Pure, dependency-free maths for the readout composition and the sign mark. No LVGL, no globals, so it is host-testable — and the `-12.0` overrun that this design already hit once is exactly what the test pins down.

**Files:**
- Create: `main/boost_neon_geom.h`
- Create: `main/boost_neon_geom.c`
- Create: `tools/test_neon_geom.c`
- Modify: `sim/CMakeLists.txt` (add test target after the `test_tpms_protocol` block, line 88)
- Modify: `main/CMakeLists.txt` (add `boost_neon_geom.c` to SRCS)

**Interfaces:**
- Consumes: nothing.
- Produces: `boost_neon_layout_t`, `boost_neon_layout_clamp()`, `boost_neon_readout_t`, `boost_neon_layout_readout()`, `boost_neon_bar_t`, `boost_neon_sign_bars()`, `boost_neon_lit_span()`.

- [ ] **Step 1: Write the header**

```c
/* main/boost_neon_geom.h */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Layout selector for the neon face. Values are persisted in NVS as a u8, so
 * the numbering is part of the on-flash format and must not be reordered. */
typedef enum {
    BOOST_NEON_TUBE = 0,
    BOOST_NEON_SEGMENTS = 1,
    BOOST_NEON_MARQUEE = 2,
} boost_neon_layout_t;

#define BOOST_NEON_LAYOUT_DEFAULT BOOST_NEON_SEGMENTS
#define BOOST_NEON_MAX_CELLS 4
#define BOOST_NEON_SIGN_BARS 2

/** Any out-of-range stored byte becomes the default rather than indexing off
 *  the end of a dispatch table. */
boost_neon_layout_t boost_neon_layout_clamp(uint8_t stored);

typedef struct {
    int16_t x;   /* cell centre, relative to face centre, positive right */
    char ch;     /* '0'..'9' or '.' */
} boost_neon_cell_t;

typedef struct {
    boost_neon_cell_t cells[BOOST_NEON_MAX_CELLS];
    uint8_t count;
    bool sign;        /* true when psi is negative */
    int16_t sign_x;   /* sign mark centre, relative to face centre */
    int16_t half_w;   /* half-width of the whole composition, sign included */
} boost_neon_readout_t;

/**
 * Lay the value out in constant-width cells, the block centred on the face.
 * The typeface is not tabular ('1' advances 409/1000 against '0' at 780), so
 * cells are fixed and the glyph is centred inside its cell.
 *
 * The block is centred rather than having the decimal pinned to the face
 * centre. Pinning would stop the cells moving entirely, which is what Big
 * Digit does, but it pushes the negative case out to 191 px half-width
 * against the segment ring's 167 px inner edge — it collides. Centring the
 * block gives 160 px and clears. The cost is that the block re-centres when
 * a tens digit appears or disappears; within a digit count nothing moves,
 * and the sign is drawn outside the block so it does not shift the cells
 * when it appears at the zero crossing.
 *
 * `half_w` is what callers size the face against: the sign sits outside the
 * cell block, so the negative case is always the widest the readout gets.
 */
void boost_neon_layout_readout(float psi, int slot_w, int dot_w,
                               int sign_w, int sign_gap,
                               boost_neon_readout_t *out);

/*
 * One slanted bar of the sign mark. Six values, not four: the bar is a
 * parallelogram, so the top edge is offset from the bottom by the italic
 * lean and an x0/y0/x1/y1 rectangle cannot express it. The first version of
 * this struct had four fields and rendered the sign as thin slivers.
 */
typedef struct {
    int16_t xt0, xt1;  /* top edge, left and right */
    int16_t xb0, xb1;  /* bottom edge, left and right */
    int16_t y0, y1;    /* top and bottom */
} boost_neon_bar_t;

/**
 * The typeface ships no hyphen and no plus — both are zero-contour blanks —
 * so the sign is drawn as geometry in the font's own slice rhythm.
 * Always writes BOOST_NEON_SIGN_BARS bars. `size` is the font size in px.
 */
void boost_neon_sign_bars(int cx, int cy, int size, int width,
                          boost_neon_bar_t out[BOOST_NEON_SIGN_BARS]);

/**
 * Inclusive index span of segments lit between the zero notch and the value.
 * Returns 0 and leaves *first/*last untouched when nothing is lit.
 * Works in both directions, so vacuum lights anticlockwise of zero.
 */
int boost_neon_lit_span(float a_zero, float a_value, float a_start,
                        float span, int nseg, int *first, int *last);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 2: Write the failing test**

```c
/* tools/test_neon_geom.c */
#include "boost_neon_geom.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* --- layout clamp ------------------------------------------------- */
    assert(boost_neon_layout_clamp(0) == BOOST_NEON_TUBE);
    assert(boost_neon_layout_clamp(1) == BOOST_NEON_SEGMENTS);
    assert(boost_neon_layout_clamp(2) == BOOST_NEON_MARQUEE);
    assert(boost_neon_layout_clamp(3) == BOOST_NEON_LAYOUT_DEFAULT);
    assert(boost_neon_layout_clamp(255) == BOOST_NEON_LAYOUT_DEFAULT);

    boost_neon_readout_t r;

    /* --- positive, one whole digit: cells are ones, dot, tenths -------- */
    boost_neon_layout_readout(8.5f, 62, 32, 35, 16, &r);
    assert(r.count == 3);
    assert(r.sign == false);
    assert(r.cells[0].ch == '8');
    assert(r.cells[1].ch == '.');
    assert(r.cells[2].ch == '5');
    /* total = 62 + 32 + 62 = 156, so half_w is 78 */
    assert(r.half_w == 78);

    /* Three cells are symmetric about the centre, so the dot lands on it. */
    assert(r.cells[1].x == 0);
    assert(r.cells[0].x == -47);
    assert(r.cells[2].x == 47);

    /* --- two whole digits: the block re-centres, so the dot moves right.
     *     Pinning the dot instead would overrun the ring — see the header. */
    boost_neon_layout_readout(19.5f, 62, 32, 35, 16, &r);
    assert(r.count == 4);
    assert(r.cells[0].ch == '1');
    assert(r.cells[1].ch == '9');
    assert(r.cells[3].ch == '5');
    assert(r.cells[2].x == 31);
    assert(r.half_w == 109);

    /* --- the negative case is the widest the readout ever gets, and is
     *     what the face is sized against. At 104 px on the segments ring
     *     (inner edge 167) this must stay clear. --------------------- */
    boost_neon_layout_readout(-12.0f, 62, 32, 30, 10, &r);
    assert(r.count == 4);
    assert(r.sign == true);
    assert(r.cells[0].ch == '1');
    assert(r.cells[1].ch == '2');
    assert(r.cells[2].ch == '.');
    assert(r.cells[3].ch == '0');
    /* cells span 62+62+32+62 = 218 -> 109; sign adds gap 16 + width 35 */
    assert(r.half_w == 149);
    /* Clears the lit body, whose inner edge is at 152. The dim halo reaches
     * further in and the sign is allowed to overlap it, as the mockup does. */
    assert(r.half_w < 152);
    /* The sign sits left of the block and does not displace the cells, so
     * crossing zero never shifts the digits. */
    assert(r.sign_x == -109 - 10 - 30 / 2);
    assert(r.cells[2].x == 31);

    /* --- rounding: .95 must carry into the whole part, not print "8.10" */
    boost_neon_layout_readout(8.95f, 62, 32, 35, 16, &r);
    assert(r.cells[0].ch == '9');
    assert(r.cells[2].ch == '0');

    /* --- a value that rounds to zero is not negative ------------------- */
    boost_neon_layout_readout(-0.02f, 62, 32, 35, 16, &r);
    assert(r.sign == false);
    assert(r.cells[0].ch == '0');

    /* --- sign bars follow the typeface's slice rhythm ------------------ */
    boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS];
    boost_neon_sign_bars(0, 0, 104, 35, bars);
    /* bar height = 104*8/128 = 6.5 -> 6; period = 104*10.4/128 = 8.45 -> 8 */
    assert(bars[0].y1 - bars[0].y0 == 6);
    assert(bars[1].y0 - bars[0].y0 == 8);
    /* the pair straddles the centre line */
    assert(bars[0].y0 < 0 && bars[1].y1 > 0);
    /* italic lean: the top edge sits right of the bottom edge, and both
     * edges are the same width — it is a parallelogram, not a trapezoid */
    assert(bars[0].xt0 > bars[0].xb0);
    assert(bars[0].xt1 - bars[0].xt0 == bars[0].xb1 - bars[0].xb0);
    assert(bars[0].xt0 - bars[0].xb0 == bars[1].xt0 - bars[1].xb0);

    /* --- lit span: boost lights clockwise of the zero notch ------------ */
    int first = -1, last = -1;
    /* zero notch at 236.25, full sweep 135..405, 56 segments */
    int n = boost_neon_lit_span(236.25f, 320.0f, 135.0f, 270.0f, 56,
                                &first, &last);
    assert(n > 0);
    assert(first == 21);   /* floor((236.25-135)/4.8214) */
    assert(last == 38);    /* floor((320-135)/4.8214) */

    /* --- vacuum lights the other way, and the span stays ordered ------- */
    n = boost_neon_lit_span(236.25f, 168.75f, 135.0f, 270.0f, 56,
                            &first, &last);
    assert(n > 0);
    assert(first == 6);
    assert(last == 21);
    assert(first <= last);

    /* --- sitting exactly on the notch lights nothing ------------------- */
    first = last = -99;
    n = boost_neon_lit_span(236.25f, 236.25f, 135.0f, 270.0f, 56,
                            &first, &last);
    assert(n == 0);
    assert(first == -99 && last == -99);

    printf("neon geom: all assertions passed\n");
    return 0;
}
```

- [ ] **Step 3: Add the test target, stub the source, and watch it fail to link**

CMake needs every listed source to exist before it will configure, so a
missing `boost_neon_geom.c` is a *configure* error — which proves nothing
about the test. Create the file with only its include, so the target
configures and then fails at **link** with undefined references to exactly
the functions the test calls. That failure is the real red state: it proves
the header and the test both compile and that nothing is implemented yet.

```bash
printf '#include "boost_neon_geom.h"\n' > main/boost_neon_geom.c
```

Append to `sim/CMakeLists.txt` after the `test_tpms_protocol` block (after line 88):

```cmake
# Host-side unit test for the pure-C neon layout geometry. No LVGL or IDF
# dependency — just the geometry source and the test harness.
add_executable(test_neon_geom
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/test_neon_geom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/boost_neon_geom.c
)
target_include_directories(test_neon_geom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
target_link_libraries(test_neon_geom PRIVATE m)
```

Run:
```bash
cmake -S sim -B sim/build -G Ninja && cmake --build sim/build --target test_neon_geom
```

Expected: configure succeeds, then the build FAILS at link with
`undefined reference to` for each of `boost_neon_layout_clamp`,
`boost_neon_layout_readout`, `boost_neon_sign_bars` and `boost_neon_lit_span`,
ending in `collect2.exe: error: ld returned 1 exit status`.

If instead you get `No SOURCES given to target: test_neon_geom`, the stub file
was not created. If you get a *compile* error rather than a link error, the
header or the test is malformed — fix that before going on, because the red
state is meant to prove they are both sound.

- [ ] **Step 4: Write the implementation**

Replace the stub's contents with the following in full.

```c
/* main/boost_neon_geom.c */
#include "boost_neon_geom.h"

#include <math.h>

boost_neon_layout_t boost_neon_layout_clamp(uint8_t stored)
{
    switch (stored) {
        case BOOST_NEON_TUBE:
        case BOOST_NEON_SEGMENTS:
        case BOOST_NEON_MARQUEE:
            return (boost_neon_layout_t)stored;
        default:
            return BOOST_NEON_LAYOUT_DEFAULT;
    }
}

void boost_neon_layout_readout(float psi, int slot_w, int dot_w,
                               int sign_w, int sign_gap,
                               boost_neon_readout_t *out)
{
    if (out == NULL) return;

    /* Round once, then decompose. Rounding per-digit lets 8.95 print as
     * "8.10" when the tenths carry but the whole part is taken from the
     * unrounded value. */
    const int tenths_total = (int)lroundf(fabsf(psi) * 10.0f);
    const int whole = tenths_total / 10;
    const int tenths = tenths_total % 10;
    const int tens = (whole / 10) % 10;

    /* A value that rounds to 0.0 is not negative, however it was measured. */
    out->sign = (psi < 0.0f) && (tenths_total != 0);
    out->count = 0;

    if (whole >= 10) {
        out->cells[out->count].ch = (char)('0' + tens);
        out->count++;
    }
    out->cells[out->count].ch = (char)('0' + (whole % 10));
    out->count++;
    const uint8_t dot_index = out->count;
    out->cells[out->count].ch = '.';
    out->count++;
    out->cells[out->count].ch = (char)('0' + tenths);
    out->count++;

    (void)dot_index;

    /* Centre the cell block on the face. Fixed cell widths mean nothing
     * moves as the value changes within a digit count; the block re-centres
     * only when a tens digit appears or disappears. The sign is placed
     * outside the block, so appearing at the zero crossing does not shift
     * the digits either. */
    int total = 0;
    int width[BOOST_NEON_MAX_CELLS];
    for (uint8_t i = 0; i < out->count; ++i) {
        width[i] = (out->cells[i].ch == '.') ? dot_w : slot_w;
        total += width[i];
    }
    int x = -total / 2;
    for (uint8_t i = 0; i < out->count; ++i) {
        out->cells[i].x = (int16_t)(x + width[i] / 2);
        x += width[i];
    }

    out->sign_x = (int16_t)(-total / 2 - sign_gap - sign_w / 2);
    out->half_w = (int16_t)(total / 2 + (out->sign ? sign_gap + sign_w : 0));
}

void boost_neon_sign_bars(int cx, int cy, int size, int width,
                          boost_neon_bar_t out[BOOST_NEON_SIGN_BARS])
{
    /* Slice rhythm sampled from the typeface itself: at 128 px the bars are
     * 8 px on a 10.4 px period. Italic angle is 12 degrees. */
    const int bar_h = (int)((float)size * 8.0f / 128.0f);
    const int period = (int)((float)size * 10.4f / 128.0f);
    const float skew = 0.2126f;   /* tanf(12 deg) */

    const int block = (BOOST_NEON_SIGN_BARS - 1) * period + bar_h;
    const int top = cy - block / 2;
    const int lean = (int)((float)bar_h * skew);

    for (int i = 0; i < BOOST_NEON_SIGN_BARS; ++i) {
        const int y0 = top + i * period;
        const int y1 = y0 + bar_h;
        /* Lean the whole mark about the centre line so it matches the digits
         * beside it rather than standing upright next to italic glyphs. */
        const int dx = (int)((float)(cy - (y0 + y1) / 2) * skew);
        out[i].y0 = (int16_t)y0;
        out[i].y1 = (int16_t)y1;
        /* Top edge sits `lean` to the right of the bottom edge; both are the
         * same width, which is what makes it a parallelogram. */
        out[i].xb0 = (int16_t)(cx - width / 2 + dx);
        out[i].xb1 = (int16_t)(cx + width / 2 + dx);
        out[i].xt0 = (int16_t)(out[i].xb0 + lean);
        out[i].xt1 = (int16_t)(out[i].xb1 + lean);
    }
}

int boost_neon_lit_span(float a_zero, float a_value, float a_start,
                        float span, int nseg, int *first, int *last)
{
    if (nseg <= 0 || span <= 0.0f) return 0;
    const float step = span / (float)nseg;

    float lo = a_zero, hi = a_value;
    if (lo > hi) { const float t = lo; lo = hi; hi = t; }
    if ((hi - lo) < (step * 0.5f)) return 0;

    int i0 = (int)floorf((lo - a_start) / step);
    int i1 = (int)floorf((hi - a_start) / step);
    if (i0 < 0) i0 = 0;
    if (i1 > nseg - 1) i1 = nseg - 1;
    if (i1 < i0) return 0;

    *first = i0;
    *last = i1;
    return (i1 - i0) + 1;
}
```

Add `boost_neon_geom.c` to the `SRCS` list in `main/CMakeLists.txt`, alongside the other `boost_*.c` entries.

It must **also** be added to the `boost_gauge_sim` target sources in
`sim/CMakeLists.txt`, not only to the `test_neon_geom` target. The simulator
compiles `main/` sources directly rather than consuming the IDF component, so
once `boost_theme.c` references the geometry (Task 3) the sim fails to link
without it. Same for `boost_theme.c` on the `test_neon_geom` target once Task 2
lands. Both were missed on the first pass and found at link time.

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build sim/build --target test_neon_geom && ./sim/build/test_neon_geom.exe
```
Expected: `neon geom: all assertions passed`, exit 0.

If the `lit_span` index assertions fail, print the computed values before adjusting the expectations — the segment index is `floor((angle - 135) / (270/56))` and the test values were derived from that formula, not measured.

- [ ] **Step 6: Commit**

```bash
git add main/boost_neon_geom.c main/boost_neon_geom.h main/CMakeLists.txt tools/test_neon_geom.c sim/CMakeLists.txt
git commit -m "feat: neon face geometry with host tests"
```

---

### Task 2: Theme table entries and the style enum

**Files:**
- Modify: `main/boost_theme.h:16-22` (the `boost_gauge_style_t` enum)
- Modify: `main/boost_theme.c:37-93` (`s_defaults[]`)
- Modify: `main/boost_theme.c:603-618` (`boost_style_name()`)
- Modify: `tools/test_neon_geom.c` (extend)
- Modify: `sim/CMakeLists.txt` (link `boost_theme.c` into the test)

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: `BOOST_STYLE_NEON`, theme ids `neon-violet` / `neon-miami` / `neon-toxic`, style token `"neon"`.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tools/test_neon_geom.c`, before the final `printf`:

```c
    /* --- theme table -------------------------------------------------- */
    assert(boost_theme_count() == 7);

    const boost_theme_t *violet = boost_theme_find("neon-violet");
    assert(violet != NULL);
    assert(violet->style == BOOST_STYLE_NEON);
    assert(violet->vacuum == 0x8B3DFFu);
    assert(violet->boost == 0xFF2BD6u);
    assert(violet->overboost == 0xFF6A00u);
    assert(violet->face == 0x000000u);

    const boost_theme_t *miami = boost_theme_find("neon-miami");
    assert(miami != NULL && miami->style == BOOST_STYLE_NEON);
    /* Overboost is orange-red, not pink: against boost magenta the pink
     * could not be told apart at a glance. */
    assert(miami->overboost == 0xFF2A00u);

    const boost_theme_t *toxic = boost_theme_find("neon-toxic");
    assert(toxic != NULL && toxic->style == BOOST_STYLE_NEON);
    assert(toxic->boost == 0xFFF000u);

    assert(strcmp(boost_style_name(BOOST_STYLE_NEON), "neon") == 0);

    /* The four originals keep their identity and their order. */
    assert(strcmp(boost_theme_at(0)->id, "dyno-cell") == 0);
    assert(strcmp(boost_theme_at(3)->id, "big-digit") == 0);
    assert(strcmp(boost_theme_at(4)->id, "neon-violet") == 0);
```

Add `#include "boost_theme.h"` at the top of the test file, and add `boost_theme.c` to the `test_neon_geom` target sources in `sim/CMakeLists.txt`.

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build sim/build --target test_neon_geom
```
Expected: FAIL — `BOOST_STYLE_NEON` undeclared.

- [ ] **Step 3: Add the enum member**

In `main/boost_theme.h`, extend the enum. Append only — the values are not persisted, but `web/app.js` and the `/themes` API dispatch on the token, and reordering would silently repoint existing faces:

```c
typedef enum {
    BOOST_STYLE_ARC = 0,   /* dual-climate arc face (Dyno Cell) */
    BOOST_STYLE_VAULT,     /* phosphor needle dial (Vault-Tec) */
    BOOST_STYLE_HUD,       /* cyberpunk targeting HUD (Night City) */
    BOOST_STYLE_BIGDIGIT,  /* huge Alvida numeral on a color-sweep ground */
    BOOST_STYLE_SPORT,     /* magenta segmented circular sport cluster */
    BOOST_STYLE_NEON,      /* neon sign face, three layouts (Neon *) */
} boost_gauge_style_t;
```

- [ ] **Step 4: Add the three theme rows**

In `main/boost_theme.c`, insert into `s_defaults[]` after the `big-digit` entry and before the Sport comment:

```c
    {
        .id = "neon-violet",
        .name = "Neon Violet",
        .style = BOOST_STYLE_NEON,
        .face = 0x000000,
        .track = 0x241038,
        .text = 0xFFFFFF,
        .muted = 0x5A3A7A,
        .vacuum = 0x8B3DFF,
        .boost = 0xFF2BD6,
        .overboost = 0xFF6A00,
        .zero = 0xFFFFFF,
    },
    {
        .id = "neon-miami",
        .name = "Neon Miami",
        .style = BOOST_STYLE_NEON,
        .face = 0x000000,
        .track = 0x10222E,
        .text = 0xFFFFFF,
        .muted = 0x3F6E80,
        .vacuum = 0x00E5FF,
        .boost = 0xFF2BD6,
        /* Orange-red, not pink: pink against the boost magenta could not be
         * told apart at a glance, and overboost is the one cue that has to
         * survive peripheral vision. */
        .overboost = 0xFF2A00,
        .zero = 0xFFFFFF,
    },
    {
        .id = "neon-toxic",
        .name = "Neon Toxic",
        .style = BOOST_STYLE_NEON,
        .face = 0x000000,
        .track = 0x12300A,
        .text = 0xFFFFFF,
        .muted = 0x4C7A2E,
        .vacuum = 0x39FF14,
        .boost = 0xFFF000,
        .overboost = 0xFF00A0,
        .zero = 0xFFFFFF,
    },
```

Then add the token to `boost_style_name()`:

```c
        case BOOST_STYLE_NEON:     return "neon";
```

- [ ] **Step 5: Run the test to verify it passes**

```bash
cmake --build sim/build --target test_neon_geom && ./sim/build/test_neon_geom.exe
```
Expected: PASS.

`THEME_COUNT` is derived with `sizeof(s_defaults)/sizeof(s_defaults[0])`, and `s_themes[]` and the override array follow it, so no size constant needs bumping. The NVS colour-override loader accepts a blob shorter than the current table and matches by id (`boost_theme.c:305`), so existing panels keep their customised colours.

- [ ] **Step 6: Commit**

```bash
git add main/boost_theme.c main/boost_theme.h tools/test_neon_geom.c sim/CMakeLists.txt
git commit -m "feat: add three neon themes and the neon style token"
```

---

### Task 3: Persisted layout setting

Follows the `vaultVignette` pattern, not `vaultNeedleRed` — this is a range-validated number, not a bool, so the JSON side needs `cJSON_IsNumber` rather than `cJSON_IsBool`.

**Files:**
- Modify: `main/boost_theme.h` (declarations, near the vault block at line 167)
- Modify: `main/boost_theme.c:14-33` (NVS key), `~114` (static), `persist()` line 187 area, `boost_theme_init()` line 285 area, and the getter/setter near line 550
- Modify: `tools/test_neon_geom.c`

**Interfaces:**
- Consumes: `boost_neon_layout_t`, `boost_neon_layout_clamp()` from Task 1.
- Produces: `boost_theme_neon_layout()` returning `boost_neon_layout_t`, `boost_theme_set_neon_layout(boost_neon_layout_t)`.

- [ ] **Step 1: Write the failing test**

Append inside `main()` in `tools/test_neon_geom.c`:

```c
    /* --- neon layout setting ------------------------------------------ */
    /* Default is segments: it has the smallest dirty region of the three. */
    assert(boost_theme_neon_layout() == BOOST_NEON_SEGMENTS);
    boost_theme_set_neon_layout(BOOST_NEON_MARQUEE);
    assert(boost_theme_neon_layout() == BOOST_NEON_MARQUEE);
    boost_theme_set_neon_layout(BOOST_NEON_TUBE);
    assert(boost_theme_neon_layout() == BOOST_NEON_TUBE);
    /* An out-of-range value must not be stored verbatim. */
    boost_theme_set_neon_layout((boost_neon_layout_t)9);
    assert(boost_theme_neon_layout() == BOOST_NEON_LAYOUT_DEFAULT);
```

- [ ] **Step 2: Run to verify it fails**

```bash
cmake --build sim/build --target test_neon_geom
```
Expected: FAIL — `boost_theme_neon_layout` undeclared.

- [ ] **Step 3: Implement**

`main/boost_theme.h`, after the vault needle declarations, plus `#include "boost_neon_geom.h"` at the top:

```c
/*
 * Which neon layout the three Neon themes render. Applies to all of them at
 * once: the palette is the theme, the layout is a display setting, and
 * keeping them independent is what lets one dropdown restyle every palette.
 */
boost_neon_layout_t boost_theme_neon_layout(void);
void boost_theme_set_neon_layout(boost_neon_layout_t layout);
```

`main/boost_theme.c` — NVS key (10 chars, inside the 15 cap):

```c
#define NVS_KEY_NEONLAY "neon_layout"
```

Static, near `s_vault_needle_tail`:

```c
static uint8_t s_neon_layout = (uint8_t)BOOST_NEON_LAYOUT_DEFAULT;
```

In `persist()`, beside the other `nvs_set_u8` calls:

```c
    nvs_set_u8(h, NVS_KEY_NEONLAY, s_neon_layout);
```

In `boost_theme_init()`, beside the vault reads. Clamp on the way in as well as the way out, so a byte written by a future firmware with more layouts cannot select a layout this build has no branch for:

```c
    uint8_t nl = 0;
    if (nvs_get_u8(h, NVS_KEY_NEONLAY, &nl) == ESP_OK) {
        s_neon_layout = (uint8_t)boost_neon_layout_clamp(nl);
    }
```

Getter and setter, beside the vault ones:

```c
boost_neon_layout_t boost_theme_neon_layout(void)
{
    return boost_neon_layout_clamp(s_neon_layout);
}

void boost_theme_set_neon_layout(boost_neon_layout_t layout)
{
    ensure_loaded();
    s_neon_layout = (uint8_t)boost_neon_layout_clamp((uint8_t)layout);
    persist();
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
cmake --build sim/build --target test_neon_geom && ./sim/build/test_neon_geom.exe
```
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add main/boost_theme.c main/boost_theme.h tools/test_neon_geom.c
git commit -m "feat: persist the neon layout selection"
```

---

### Task 4: Generate and wire the three fonts

**Files:**
- Create: `main/fonts/neon_big.c`, `main/fonts/neon_huge.c`, `main/fonts/neon_label.c`
- Modify: `main/CMakeLists.txt:27-36` (font source list)
- Modify: `main/boost_gauge.c:249-269` (declarations and aliases)

**Interfaces:**
- Consumes: nothing.
- Produces: `NEON_BIG` (104 px), `NEON_HUGE` (156 px), `NEON_LABEL` (26 px) font pointer macros.

- [ ] **Step 1: Generate the fonts**

Digit-only subsetting on the two large sizes is what keeps this affordable — the full face at 156 px would be many times the size. `--lv-include lvgl.h` is required; the default `lvgl/lvgl.h` does not resolve against the managed component.

```bash
npx --yes lv_font_conv --font main/fonts/SFAlienEncounters-Italic.ttf --size 104 --bpp 4 --format lvgl --lv-include lvgl.h --no-compress -r 0x30-0x39,0x2E -o main/fonts/neon_big.c
```

```bash
npx --yes lv_font_conv --font main/fonts/SFAlienEncounters-Italic.ttf --size 156 --bpp 4 --format lvgl --lv-include lvgl.h --no-compress -r 0x30-0x39,0x2E -o main/fonts/neon_huge.c
```

```bash
npx --yes lv_font_conv --font main/fonts/SFAlienEncounters-Italic.ttf --size 26 --bpp 4 --format lvgl --lv-include lvgl.h --no-compress -r 0x20,0x30-0x39,0x41-0x5A -o main/fonts/neon_label.c
```

- [ ] **Step 2: Verify the flash cost matches the spec**

```bash
python -c "import re,glob; [print(f, sum(1 for _ in re.finditer(r'0x[0-9a-fA-F]{2}', re.search(r'glyph_bitmap\[\]\s*=\s*\{(.*?)\n\};', open(f,encoding='utf-8',errors='replace').read(), re.S).group(1)))) for f in sorted(glob.glob('main/fonts/neon_*.c'))]"
```
Expected, within a few percent: `neon_big` ~11,784 B, `neon_huge` ~25,220 B, `neon_label` ~3,496 B. Total ~40 KB. A materially larger number means the range subset did not apply — check the `-r` argument reached the tool.

- [ ] **Step 3: Wire them in**

Add the three files to the font list in `main/CMakeLists.txt` alongside `fonts/font_cond_96.c`.

In `main/boost_gauge.c`, extend the declaration block:

```c
LV_FONT_DECLARE(neon_big);
LV_FONT_DECLARE(neon_huge);
LV_FONT_DECLARE(neon_label);
```

and the alias block:

```c
#define NEON_BIG   (&neon_big)
#define NEON_HUGE  (&neon_huge)
#define NEON_LABEL (&neon_label)
```

- [ ] **Step 4: Verify it compiles**

```bash
cmake --build sim/build --target boost_gauge_sim
```
Expected: builds clean. The fonts are not referenced yet, so this only proves they compile and link.

- [ ] **Step 5: Commit**

```bash
git add main/fonts/neon_big.c main/fonts/neon_huge.c main/fonts/neon_label.c main/CMakeLists.txt main/boost_gauge.c
git commit -m "feat: embed SF Alien Encounters at three sizes for the neon face"
```

---

### Task 5: Segments layout — the default

The whole neon face lands in this task: state, bake, draw callbacks, dispatch and teardown. Only the segments geometry is implemented; the other two branches fall through to it until Tasks 6 and 7.

**Files:**
- Modify: `main/boost_gauge.c` — new static block after the sport block (~line 458), forward declarations, `build_scene()` switch (line 3544), `boost_gauge_update()` switch (line 3742), `destroy_scene()` reset (line 3506 area)

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: `build_neon()`, `update_neon()`, `draw_neon_live()`, `paint_neon_background()`.

- [ ] **Step 1: Add state and forward declarations**

After the sport block in `main/boost_gauge.c`:

```c
/* ---- neon style ---------------------------------------------------------- */
#define NEON_R          182     /* ring centre radius */
#define NEON_SEG_W      30      /* segment stroke width */
#define NEON_TUBE_W     26      /* tube stroke width */
#define NEON_NSEG       54
/* 54 segments, not 56: LV_USE_FLOAT is 0 so lv_value_precise_t is int32_t and
 * arc angles quantise to WHOLE DEGREES. 270/56 = 4.8214 deg puts every boundary
 * off-grid, so segments rounded to 3 or 4 degrees with gaps varying to match and
 * the ring looked visibly uneven. 270/54 = 5 deg exactly, so gap 2 and lit 3 are
 * both integers and every segment is identical. */
#define NEON_SEG_GAP    2.0f    /* degrees of dark between segments */
#define NEON_LABEL_R    220     /* scale numeral radius */
#define NEON_SLOT_W     62
#define NEON_DOT_W      32
/* The sign sets the widest the readout ever gets. Clearance is measured to the
 * BODY's inner edge at 152; the halo below it is a dim inward shadow that the
 * sign may sit against, exactly as in the approved mockup. half_w 149 clears
 * the body by 3 px. */
#define NEON_SIGN_W     30
#define NEON_SIGN_GAP   10

static void *s_neon_bg_buf;
static lv_obj_t *s_neon_bg;
static lv_obj_t *s_neon_face;
static lv_obj_t *s_neon_zone;
static lv_obj_t *s_neon_unit;
static lv_obj_t *s_neon_peak;
static float s_neon_psi;          /* committed value the art is drawn from */
static float s_neon_peak_value;
static boost_neon_layout_t s_neon_layout;

static void paint_neon_background(lv_obj_t *canvas, const boost_theme_t *theme);
static void draw_neon_live(lv_event_t *e);
static void build_neon(lv_obj_t *scr);
static void update_neon(const boost_sample_t *sample, const boost_theme_t *theme);
```

Add `#include "boost_neon_geom.h"` to the include block at the top of the file.

- [ ] **Step 2: Write the bake**

Static art only. Everything expensive — the unlit segment bodies, the scale, the halo behind the ring — is paid once here. Nothing that moves may be drawn into this canvas.

```c
static void paint_neon_background(lv_obj_t *canvas, const boost_theme_t *theme)
{
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_draw_rect_dsc_t bg;
    lv_draw_rect_dsc_init(&bg);
    bg.bg_color = lv_color_black();
    bg.bg_opa = LV_OPA_COVER;
    lv_area_t full = { 0, 0, DISP_SIZE - 1, DISP_SIZE - 1 };
    lv_draw_rect(&layer, &bg, &full);

    /* Canvas coordinates, so the burn-in offset is subtracted back out — the
     * canvas itself is moved by s_root, not redrawn. */
    const int cx = px_icx() - s_px_dx;
    const int cy = px_icy() - s_px_dy;

    lv_draw_arc_dsc_t arc;
    lv_draw_arc_dsc_init(&arc);
    arc.center.x = cx;
    arc.center.y = cy;
    arc.radius = NEON_R;
    arc.rounded = false;
    arc.opa = LV_OPA_COVER;

    const float step = (float)ARC_RANGE / (float)NEON_NSEG;
    for (int i = 0; i < NEON_NSEG; ++i) {
        arc.start_angle = (float)ARC_START + (float)i * step + NEON_SEG_GAP * 0.5f;
        arc.end_angle = arc.start_angle + step - NEON_SEG_GAP;
        arc.color = c(theme->track);
        arc.width = NEON_SEG_W;
        lv_draw_arc(&layer, &arc);
    }

    /* Scale numerals sit outside the ring. Inside does not fit: the negative
     * readout reaches 144 px and the ring's inner edge is 167, and a "-10"
     * overruns that 23 px annulus into the lit arc. */
    lv_draw_label_dsc_t lbl;
    lv_draw_label_dsc_init(&lbl);
    lbl.font = F_COND18;
    lbl.align = LV_TEXT_ALIGN_CENTER;
    static const float ticks[] = { -15.0f, -10.0f, -5.0f, 0.0f, 5.0f, 10.0f };
    for (size_t i = 0; i < sizeof(ticks) / sizeof(ticks[0]); ++i) {
        const float a = psi_to_sweep(ticks[i], (float)ARC_START,
                                     (float)(ARC_START + ARC_RANGE));
        const float rad = a * (float)M_PI / 180.0f;
        const int lx = cx + (int)lroundf(cosf(rad) * (float)NEON_LABEL_R);
        const int ly = cy + (int)lroundf(sinf(rad) * (float)NEON_LABEL_R);
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", (int)ticks[i]);
        lbl.color = (ticks[i] == 0.0f) ? c(theme->text) : c(theme->muted);
        lbl.text = buf;
        /* lv_draw_label only rasterises at finish_layer, so a loop-local
         * buffer must be copied rather than referenced. */
        lbl.text_local = 1;
        lv_area_t la = { lx - 26, ly - 12, lx + 26, ly + 12 };
        lv_draw_label(&layer, &lbl, &la);
    }

    lv_canvas_finish_layer(canvas, &layer);
}
```

- [ ] **Step 3: Write the live draw callback**

Glow is faked with three opaque strokes of decreasing brightness plus a white core. Never use alpha here — per-frame alpha is the largest discrete raster cost in this codebase.

```c
static void draw_neon_live(lv_event_t *e)
{
    const boost_theme_t *theme = active_theme();
    lv_layer_t *layer = lv_event_get_layer(e);
    const int cx = px_icx();
    const int cy = px_icy();
    const float psi = s_neon_psi;
    const lv_color_t accent = color_for_psi(theme, psi);
    const uint32_t accent_rgb = (psi >= s_psi_overboost) ? theme->overboost
                              : (psi > 0.05f) ? theme->boost : theme->vacuum;

    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START,
                                      (float)(ARC_START + ARC_RANGE));
    const float a_val = psi_to_sweep(psi, (float)ARC_START,
                                     (float)(ARC_START + ARC_RANGE));

    if (clip_reaches_radius(layer, (float)cx, (float)cy,
                            (float)(NEON_R - NEON_SEG_W))) {
        lv_draw_arc_dsc_t arc;
        lv_draw_arc_dsc_init(&arc);
        arc.center.x = cx;
        arc.center.y = cy;
        arc.radius = NEON_R;
        arc.opa = LV_OPA_COVER;

        const float step = (float)ARC_RANGE / (float)NEON_NSEG;
        int first = 0, last = 0;
        if (boost_neon_lit_span(a_zero, a_val, (float)ARC_START,
                                (float)ARC_RANGE, NEON_NSEG,
                                &first, &last) > 0) {
            for (int i = first; i <= last; ++i) {
                const float s = (float)ARC_START + (float)i * step + NEON_SEG_GAP * 0.5f;
                arc.start_angle = s;
                arc.end_angle = s + step - NEON_SEG_GAP;
                /* Pre-blended halo, body, then a white core: three opaque
                 * passes instead of one translucent one. */
                arc.color = c(lerp_rgb(0x000000u, accent_rgb, 0.30f));
                arc.width = NEON_SEG_W + 14;
                lv_draw_arc(layer, &arc);
                arc.color = accent;
                arc.width = NEON_SEG_W;
                lv_draw_arc(layer, &arc);
                arc.color = lv_color_white();
                arc.width = 6;
                lv_draw_arc(layer, &arc);
            }
        }

        /* Peak tell-tale: one held segment, coloured by the zone the peak
         * itself falls in rather than the current one. */
        if (s_neon_peak_value > 0.2f) {
            const float ap = psi_to_sweep(s_neon_peak_value, (float)ARC_START,
                                          (float)(ARC_START + ARC_RANGE));
            const int pi = (int)floorf((ap - (float)ARC_START) / step);
            if (pi >= 0 && pi < NEON_NSEG) {
                const float s = (float)ARC_START + (float)pi * step + NEON_SEG_GAP * 0.5f;
                arc.start_angle = s;
                arc.end_angle = s + step - NEON_SEG_GAP;
                arc.color = color_for_psi(theme, s_neon_peak_value);
                arc.width = NEON_SEG_W;
                lv_draw_arc(layer, &arc);
            }
        }
    }

    /* Readout: fixed cells, decimal pinned to centre, sign drawn as bars. */
    boost_neon_readout_t r;
    boost_neon_layout_readout(psi, NEON_SLOT_W, NEON_DOT_W,
                              NEON_SIGN_W, NEON_SIGN_GAP, &r);

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = NEON_BIG;
    dsc.color = accent;
    dsc.align = LV_TEXT_ALIGN_CENTER;
    char ch[2] = { 0, 0 };
    for (uint8_t i = 0; i < r.count; ++i) {
        ch[0] = r.cells[i].ch;
        dsc.text = ch;
        dsc.text_local = 1;
        /* lv_draw_label draws from the TOP of its area, not centred in it.
         * With the box starting at cy-78 the ~73 px of digit ink centred
         * ~40 px above the face centre, which left the sign (centred on cy)
         * visibly low and pushed the digits into the zone word. Start the
         * box half an ink-height above centre instead. */
        lv_area_t a = { cx + r.cells[i].x - NEON_SLOT_W / 2, cy - 37,
                        cx + r.cells[i].x + NEON_SLOT_W / 2, cy + 120 };
        lv_draw_label(layer, &dsc, &a);
    }
    if (r.sign) {
        boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS];
        boost_neon_sign_bars(cx + r.sign_x, cy, 104, NEON_SIGN_W, bars);
        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = accent;
        tri.opa = LV_OPA_COVER;
        for (int i = 0; i < BOOST_NEON_SIGN_BARS; ++i) {
            /* Each slanted bar is two triangles: the renderer has no
             * parallelogram primitive and a rotated rect would need a
             * transform pass this pipeline does not have. */
            tri.p[0].x = bars[i].x0; tri.p[0].y = bars[i].y0;
            tri.p[1].x = bars[i].x1; tri.p[1].y = bars[i].y0;
            tri.p[2].x = bars[i].x1 - (bars[i].x0 - bars[i].x1 > 0 ? 0 : 0);
            tri.p[2].y = bars[i].y1;
            lv_draw_triangle(layer, &tri);
            tri.p[0].x = bars[i].x0; tri.p[0].y = bars[i].y0;
            tri.p[1].x = bars[i].x1; tri.p[1].y = bars[i].y1;
            tri.p[2].x = bars[i].x0 - (bars[i].x1 - bars[i].x0);
            tri.p[2].y = bars[i].y1;
            lv_draw_triangle(layer, &tri);
        }
    }
}
```

Note on the sign: verify the two-triangle decomposition renders a clean parallelogram in the sim before moving on. If it seams, replace it with a single `lv_draw_rect` per bar and accept an upright sign — record the decision in a comment rather than leaving the seam.

- [ ] **Step 4: Write build and update**

```c
static void build_neon(lv_obj_t *scr)
{
    const boost_theme_t *theme = active_theme();
    s_neon_layout = boost_theme_neon_layout();

    const uint32_t bg_bytes = LV_CANVAS_BUF_SIZE(DISP_SIZE, DISP_SIZE, 16,
                                                 LV_DRAW_BUF_STRIDE_ALIGN);
    s_neon_bg_buf = BG_ALLOC(bg_bytes);
    if (s_neon_bg_buf != NULL) {
        s_neon_bg = lv_canvas_create(scr);
        lv_canvas_set_buffer(s_neon_bg, s_neon_bg_buf, DISP_SIZE, DISP_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_center(s_neon_bg);
        lv_obj_clear_flag(s_neon_bg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        paint_neon_background(s_neon_bg, theme);
    } else {
        ESP_LOGW(TAG, "neon background cache alloc failed (%u B)", (unsigned)bg_bytes);
    }

    s_neon_face = lv_obj_create(scr);
    lv_obj_remove_style_all(s_neon_face);
    lv_obj_set_size(s_neon_face, DISP_SIZE, DISP_SIZE);
    lv_obj_center(s_neon_face);
    lv_obj_clear_flag(s_neon_face, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_neon_face, draw_neon_live, LV_EVENT_DRAW_MAIN, NULL);

    s_neon_zone = lv_label_create(scr);
    lv_label_set_text(s_neon_zone, "VACUUM");
    lv_obj_set_style_text_font(s_neon_zone, NEON_LABEL, 0);
    lv_obj_set_style_text_letter_space(s_neon_zone, 2, 0);
    lv_obj_set_style_text_color(s_neon_zone, c(theme->vacuum), 0);
    lv_obj_set_style_text_align(s_neon_zone, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_neon_zone, 300, 34);
    lv_obj_align(s_neon_zone, LV_ALIGN_CENTER, 0, -82);

    /* Unit mark under the readout. Static text, so an ordinary label. */
    s_neon_unit = lv_label_create(scr);
    lv_label_set_text(s_neon_unit, "P S I");
    lv_obj_set_style_text_font(s_neon_unit, NEON_LABEL, 0);
    lv_obj_set_style_text_letter_space(s_neon_unit, 3, 0);
    lv_obj_set_style_text_color(s_neon_unit, c(theme->muted), 0);
    lv_obj_set_style_text_align(s_neon_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_neon_unit, 200, 30);
    lv_obj_align(s_neon_unit, LV_ALIGN_CENTER, 0, 58);

    s_neon_peak = lv_label_create(scr);
    lv_label_set_text(s_neon_peak, "PEAK 0.0");
    lv_obj_set_style_text_font(s_neon_peak, F_MONO16, 0);
    lv_obj_set_style_text_color(s_neon_peak, c(theme->muted), 0);
    lv_obj_set_style_text_align(s_neon_peak, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(s_neon_peak, 260, 24);
    lv_obj_align(s_neon_peak, LV_ALIGN_CENTER, 0, 104);

    /* Seed from the live value: without this the first frame draws zero and
     * then jumps, which reads as a glitch on every theme switch. */
    s_neon_psi = isfinite(s_display_psi) ? s_display_psi : 0.0f;
    s_neon_peak_value = fmaxf(s_peak_psi, 0.0f);
}

static void update_neon(const boost_sample_t *sample, const boost_theme_t *theme)
{
    const float psi = isfinite(sample->psi) ? sample->psi : 0.0f;
    const float old_psi = s_neon_psi;
    s_neon_psi = psi;
    s_neon_peak_value = fmaxf(s_peak_psi, 0.0f);

    const char *zone = (psi >= s_psi_overboost) ? "OVERBOOST"
                     : (psi > 0.05f) ? "BOOST" : "VACUUM";
    const lv_color_t zc = color_for_psi(theme, psi);
    if (strcmp(lv_label_get_text(s_neon_zone), zone) != 0)
        lv_label_set_text(s_neon_zone, zone);
    if (!lv_color_eq(lv_obj_get_style_text_color(s_neon_zone, 0), zc))
        lv_obj_set_style_text_color(s_neon_zone, zc, 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "PEAK %.1f", (double)s_neon_peak_value);
    if (strcmp(lv_label_get_text(s_neon_peak), buf) != 0)
        lv_label_set_text(s_neon_peak, buf);

    /* Invalidate the segments that actually changed plus the readout box,
     * never the whole widget. This bounded update is the reason segments is
     * the default layout. */
    const float a_zero = psi_to_sweep(0.0f, (float)ARC_START,
                                      (float)(ARC_START + ARC_RANGE));
    const float a_old = psi_to_sweep(old_psi, (float)ARC_START,
                                     (float)(ARC_START + ARC_RANGE));
    const float a_new = psi_to_sweep(psi, (float)ARC_START,
                                     (float)(ARC_START + ARC_RANGE));
    float lo = fminf(fminf(a_old, a_new), a_zero);
    float hi = fmaxf(fmaxf(a_old, a_new), a_zero);
    lv_area_t ring;
    const int rr = NEON_R + NEON_SEG_W / 2 + 8;
    (void)lo; (void)hi;
    /* A bounding box over the changed arc: cheaper to compute than an exact
     * wedge and still far smaller than the face. */
    ring.x1 = px_icx() - rr; ring.y1 = px_icy() - rr;
    ring.x2 = px_icx() + rr; ring.y2 = px_icy() + rr;
    lv_obj_invalidate_area(s_neon_face, &ring);

    lv_area_t readout = {
        px_icx() - 175, px_icy() - 80,
        px_icx() + 175, px_icy() + 80,
    };
    lv_obj_invalidate_area(s_neon_face, &readout);
}
```

The ring invalidation above is deliberately a whole-ring box for the first working version. Task 8 tightens it to the changed wedge once the audit is passing — correctness first, then bounds.

- [ ] **Step 5: Wire dispatch and teardown**

In `build_scene()`, add before the `BOOST_STYLE_ARC` case:

```c
        case BOOST_STYLE_NEON:     build_neon(s_root); break;
```

In `boost_gauge_update()`:

```c
        case BOOST_STYLE_NEON:     update_neon(sample, theme); break;
```

In `destroy_scene()`, beside the sport reset. **This is the step nothing enforces** — `destroy_scene()` is style-agnostic and clears every face's statics unconditionally, so a face that omits its block leaks its canvas on every theme switch:

```c
    if (s_neon_bg_buf != NULL) {
        BG_FREE(s_neon_bg_buf);
        s_neon_bg_buf = NULL;
    }
    s_neon_bg = NULL;
    s_neon_face = s_neon_zone = s_neon_unit = s_neon_peak = NULL;
```

- [ ] **Step 6: Render and eyeball**

```bash
cmake --build sim/build --target boost_gauge_sim
```

```bash
./sim/build/boost_gauge_sim.exe --screenshot preview/sim_neon --theme neon-violet && python sim/raw_to_png.py preview/sim_neon
```

Open `preview/sim_neon/gauge_sheet.png`. Compare against `preview/neon_mock/B_segments_sheet.png`. Check specifically: the `-12.0` sign renders as two slanted bars, the readout clears the ring, all six scale numerals are present, and the lit run starts at the zero notch rather than at the sweep start.

- [ ] **Step 7: Run the stale-pixel audit**

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-violet --seconds 25
```
Expected: zero severe mismatches. A non-zero count means the invalidation bounds do not cover what the draw callback paints — widen the invalidated area until it passes, then narrow it again in Task 8.

- [ ] **Step 8: Commit**

```bash
git add main/boost_gauge.c
git commit -m "feat: neon segment-ring face"
```

---

### Task 6: Tube layout

**Files:**
- Modify: `main/boost_gauge.c` (`paint_neon_background`, `draw_neon_live`)

**Interfaces:**
- Consumes: Task 5's `s_neon_layout` and the bake/draw structure.
- Produces: nothing new.

- [ ] **Step 1: Branch the bake**

Wrap the segment loop in `paint_neon_background()` in `if (s_neon_layout == BOOST_NEON_SEGMENTS) { ... }` and add the tube branch. The tick set is asymmetric on purpose — vacuum is linear over a wider span than boost, so evenly spaced minors both sides would misrepresent the scale:

```c
    } else if (s_neon_layout == BOOST_NEON_TUBE) {
        arc.start_angle = (float)ARC_START;
        arc.end_angle = (float)(ARC_START + ARC_RANGE);
        arc.color = c(theme->track);
        arc.width = NEON_TUBE_W;
        lv_draw_arc(&layer, &arc);

        static const float tick_psi[] = {
            -15.0f, -12.5f, -10.0f, -7.5f, -5.0f, -2.5f, 0.0f,
            2.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f
        };
        static const bool tick_major[] = {
            true, false, true, false, true, false, true,
            false, false, true, false, false, true
        };
        lv_draw_line_dsc_t ln;
        lv_draw_line_dsc_init(&ln);
        ln.opa = LV_OPA_COVER;
        for (size_t i = 0; i < sizeof(tick_psi) / sizeof(tick_psi[0]); ++i) {
            const float a = psi_to_sweep(tick_psi[i], (float)ARC_START,
                                         (float)(ARC_START + ARC_RANGE));
            const float rad = a * (float)M_PI / 180.0f;
            const int r0 = NEON_R + (tick_major[i] ? 16 : 18);
            const int r1 = NEON_R + (tick_major[i] ? 26 : 24);
            ln.width = tick_major[i] ? 4 : 2;
            ln.color = (tick_psi[i] == 0.0f) ? c(theme->text) : c(theme->muted);
            ln.p1.x = (float)cx + cosf(rad) * (float)r0;
            ln.p1.y = (float)cy + sinf(rad) * (float)r0;
            ln.p2.x = (float)cx + cosf(rad) * (float)r1;
            ln.p2.y = (float)cy + sinf(rad) * (float)r1;
            lv_draw_line(&layer, &ln);
        }
    }
```

The scale-numeral loop stays shared and runs for both ring layouts.

- [ ] **Step 2: Branch the live draw**

In `draw_neon_live()`, replace the segment loop with a layout branch. The tube draws one arc per brightness pass rather than one per segment:

```c
        if (s_neon_layout == BOOST_NEON_TUBE) {
            float lo = a_zero, hi = a_val;
            if (lo > hi) { const float t = lo; lo = hi; hi = t; }
            if ((hi - lo) > 0.4f) {
                arc.start_angle = lo;
                arc.end_angle = hi;
                arc.color = c(lerp_rgb(0x000000u, accent_rgb, 0.35f));
                arc.width = NEON_TUBE_W + 16;
                lv_draw_arc(layer, &arc);
                arc.color = c(lerp_rgb(0x000000u, accent_rgb, 0.75f));
                arc.width = NEON_TUBE_W;
                lv_draw_arc(layer, &arc);
                arc.color = accent;
                arc.width = NEON_TUBE_W - 12;
                lv_draw_arc(layer, &arc);
                arc.color = lv_color_white();
                arc.width = 3;
                lv_draw_arc(layer, &arc);
            }
        } else { /* existing segment loop */ }
```

- [ ] **Step 3: Render and compare**

```bash
cmake --build sim/build --target boost_gauge_sim
```

Set the layout to tube, then render. The layout is persisted, so the sim needs it set — add a temporary `boost_theme_set_neon_layout(BOOST_NEON_TUBE);` call in `sim/main.c` beside the theme application, or drive it through the API against the mock server. Prefer the sim edit; revert it before committing.

```bash
./sim/build/boost_gauge_sim.exe --screenshot preview/sim_neon_tube --theme neon-violet && python sim/raw_to_png.py preview/sim_neon_tube
```

Compare against `preview/neon_mock/A_tube_sheet.png`. Confirm all six labels render — the mockup had a bug where `5` was silently absent because the tick set listed 2, 4, 6, 8 while the label check looked for 5. The tick table above includes `5.0f` explicitly to prevent exactly that.

- [ ] **Step 4: Audit**

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-violet --seconds 25
```
Expected: zero severe mismatches.

- [ ] **Step 5: Commit**

```bash
git add main/boost_gauge.c
git commit -m "feat: neon tube layout"
```

---

### Task 7: Marquee layout

**Files:**
- Modify: `main/boost_gauge.c` (`paint_neon_background`, `draw_neon_live`, `build_neon`)

**Interfaces:**
- Consumes: Tasks 5-6.
- Produces: nothing new.

- [ ] **Step 1: Bake the bulb border and the bar track**

Add a third branch in `paint_neon_background()`:

```c
    } else { /* BOOST_NEON_MARQUEE */
        lv_draw_rect_dsc_t bulb;
        lv_draw_rect_dsc_init(&bulb);
        bulb.radius = LV_RADIUS_CIRCLE;
        bulb.bg_opa = LV_OPA_COVER;
        for (int i = 0; i < 72; ++i) {
            const float rad = (float)i * 5.0f * (float)M_PI / 180.0f;
            const int bx = cx + (int)lroundf(cosf(rad) * 216.0f);
            const int by = cy + (int)lroundf(sinf(rad) * 216.0f);
            bulb.bg_color = ((i % 6) < 2) ? c(theme->muted) : c(theme->track);
            lv_area_t a = { bx - 4, by - 4, bx + 4, by + 4 };
            lv_draw_rect(&layer, &bulb, &a);
        }
        lv_draw_rect_dsc_t track;
        lv_draw_rect_dsc_init(&track);
        track.radius = 8;
        track.bg_opa = LV_OPA_COVER;
        track.bg_color = c(theme->track);
        lv_area_t bar = { cx - 150, cy + 68, cx + 150, cy + 84 };
        lv_draw_rect(&layer, &track, &bar);

        lv_draw_label_dsc_t sl;
        lv_draw_label_dsc_init(&sl);
        sl.font = F_COND18;
        sl.align = LV_TEXT_ALIGN_CENTER;
        static const float mticks[] = { -15.0f, -10.0f, -5.0f, 0.0f, 5.0f, 10.0f };
        for (size_t i = 0; i < sizeof(mticks) / sizeof(mticks[0]); ++i) {
            const int tx = cx - 150 + (int)lroundf(
                300.0f * (mticks[i] - s_psi_min) / (s_psi_max - s_psi_min));
            char buf[8];
            snprintf(buf, sizeof(buf), "%d", (int)mticks[i]);
            sl.color = (mticks[i] == 0.0f) ? c(theme->text) : c(theme->muted);
            sl.text = buf;
            sl.text_local = 1;
            lv_area_t la = { tx - 22, cy + 92, tx + 22, cy + 114 };
            lv_draw_label(&layer, &sl, &la);
        }
    }
```

The shared scale-numeral loop must now be skipped for marquee — guard it with `if (s_neon_layout != BOOST_NEON_MARQUEE)`.

- [ ] **Step 2: Draw the live bar and the larger readout**

In `draw_neon_live()`, marquee replaces the ring work entirely and uses the larger font and cell metrics:

```c
    if (s_neon_layout == BOOST_NEON_MARQUEE) {
        const float lo_psi = fminf(0.0f, psi);
        const float hi_psi = fmaxf(0.0f, psi);
        const int x_lo = cx - 150 + (int)lroundf(
            300.0f * (clampf(lo_psi, s_psi_min, s_psi_max) - s_psi_min)
            / (s_psi_max - s_psi_min));
        const int x_hi = cx - 150 + (int)lroundf(
            300.0f * (clampf(hi_psi, s_psi_min, s_psi_max) - s_psi_min)
            / (s_psi_max - s_psi_min));
        if (x_hi - x_lo > 2) {
            lv_draw_rect_dsc_t fill;
            lv_draw_rect_dsc_init(&fill);
            fill.radius = 8;
            fill.bg_opa = LV_OPA_COVER;
            fill.bg_color = accent;
            lv_area_t a = { x_lo, cy + 68, x_hi, cy + 84 };
            lv_draw_rect(layer, &fill, &a);
            fill.bg_color = lv_color_white();
            fill.radius = 2;
            lv_area_t hl = { x_lo, cy + 71, x_hi, cy + 74 };
            lv_draw_rect(layer, &fill, &hl);
        }
    }
```

The readout block gains layout-dependent metrics — replace the fixed constants:

```c
    const bool marquee = (s_neon_layout == BOOST_NEON_MARQUEE);
    const int slot_w  = marquee ? 94 : NEON_SLOT_W;
    const int dot_w   = marquee ? 48 : NEON_DOT_W;
    const int sign_w  = marquee ? 53 : NEON_SIGN_W;
    const int sign_gap= marquee ? 11 : NEON_SIGN_GAP;
    const int fsize   = marquee ? 156 : 104;
    dsc.font = marquee ? NEON_HUGE : NEON_BIG;
```

and pass those into `boost_neon_layout_readout()` and `boost_neon_sign_bars()`. Marquee's sign gap is proportionally smaller because its numeral is 50% larger; held at the same fraction the sign reads as marooned.

In `build_neon()`, marquee moves its labels: zone word to `LV_ALIGN_CENTER, 0, 136` and peak to `0, 168`, and the readout centre shifts to `cy - 26`.

- [ ] **Step 3: Render, compare, audit**

```bash
cmake --build sim/build --target boost_gauge_sim
```

```bash
./sim/build/boost_gauge_sim.exe --screenshot preview/sim_neon_marquee --theme neon-violet && python sim/raw_to_png.py preview/sim_neon_marquee
```

Compare against `preview/neon_mock/C_marquee_sheet.png`. Check the `-12.0` sign does not overrun the bulb border — marquee's readout reaches 204 px half-width and the bulbs sit at 216.

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-violet --seconds 25
```
Expected: zero severe mismatches.

- [ ] **Step 4: Commit**

```bash
git add main/boost_gauge.c
git commit -m "feat: neon marquee layout"
```

---

### Task 8: Tighten the ring invalidation

Task 5 shipped a whole-ring bounding box to get correctness first. Now narrow it to the changed wedge and prove the audit still passes — this is what makes segments the cheapest face in the project rather than merely a correct one.

**Files:**
- Modify: `main/boost_gauge.c` (`update_neon`)

**Interfaces:**
- Consumes: Task 5's `update_neon`.
- Produces: nothing new.

- [ ] **Step 1: Record the baseline**

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-violet --seconds 25
```
Write down the reported mean and max flushed pixels per cycle. That is the number this task has to beat.

**The delta wedge alone is not sufficient, and this is why the first attempt
failed.** Three distinct cases must be handled, exactly as the arc face already
does in `set_value_arc()` (`main/boost_gauge.c:1436`) — read that function
before writing this one:

1. **Side flip** — the value crossed zero, so the old and new lit runs are on
   opposite sides of the notch and disjoint. Invalidate both spans separately.
2. **Colour flip** — `color_for_psi()` returned a different colour than it did
   for the committed value. Every lit segment recolours, not just the new ones,
   so invalidate the union of the two *full* spans (notch to old, notch to new).
   This is the case a delta-only wedge silently gets wrong: crossing 0.35 psi or
   the overboost threshold restyles the whole run and leaves it stale.
3. **Otherwise** — only the moving end moved. Invalidate the delta between the
   old and new endpoints.

Track the committed colour reference alongside the committed value, mirroring
`s_arc_drawn_psi` / `s_arc_color_psi`. The peak tell-tale sits outside all three
spans: keep its segment index and, when it changes, invalidate the old and the
new peak segment as well.

Size every wedge with the **halo** width (`NEON_SEG_W + 14`), not the body
width — the halo is the widest stroke and reaches furthest in.

- [ ] **Step 2: Replace the box with a wedge union**

```c
    /* Union of the old and new arc positions, split at the 90 degree
     * boundaries so a span crossing an axis does not inflate into a box
     * covering the whole ring. */
    lv_area_t inv;
    lv_draw_arc_dsc_t probe;
    lv_draw_arc_dsc_init(&probe);
    probe.center.x = px_icx();
    probe.center.y = px_icy();
    probe.radius = NEON_R;
    probe.width = NEON_SEG_W + 16;
    probe.start_angle = lo;
    probe.end_angle = hi;
    lv_draw_arc_get_area(probe.center.x, probe.center.y, probe.radius,
                         lo, hi, probe.width, false, &inv);
    lv_area_increase(&inv, 4, 4);
    lv_obj_invalidate_area(s_neon_face, &inv);
```

Remove the `(void)lo; (void)hi;` and the `rr` box from Task 5.

- [ ] **Step 3: Re-audit and compare**

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-violet --seconds 25
```
Expected: still zero severe mismatches, and mean flushed pixels per cycle materially lower than the Step 1 baseline. If mismatches appear, the wedge is smaller than what the draw callback paints — the peak tell-tale sits outside the changed span and must be unioned in, or invalidated separately when the peak index changes.

- [ ] **Step 4: Repeat for all three layouts**

```bash
./sim/build/boost_gauge_sim.exe --audit --theme neon-miami --seconds 25
```
Run once per layout by flipping the persisted setting. Record all three numbers for the README ledger.

- [ ] **Step 5: Commit**

```bash
git add main/boost_gauge.c
git commit -m "perf: bound neon ring invalidation to the changed wedge"
```

---

### Task 9: HTTP API

**Files:**
- Modify: `main/boost_web.c:711-739` (`themes_get()` serialisation)
- Modify: `main/boost_web.c:902-918` (`PUT /themes/config` parse)

**Interfaces:**
- Consumes: `boost_theme_neon_layout()` / `boost_theme_set_neon_layout()` from Task 3.
- Produces: JSON field `neonLayout`, an integer 0/1/2.

- [ ] **Step 1: Serialise it**

Add `\"neonLayout\":%u,` to the format string in `themes_get()` beside `vaultNeedleTail`, and the matching argument in position:

```c
             (unsigned)boost_theme_neon_layout(),
```

- [ ] **Step 2: Parse it**

Add after the `vaultNeedleTail` block. This follows `vaultVignette`, not `vaultNeedleRed` — it is a range-validated number, and an out-of-range value is a client error rather than something to silently clamp:

```c
    const cJSON *nlay = cJSON_GetObjectItemCaseSensitive(root, "neonLayout");
    if (cJSON_IsNumber(nlay)) {
        const double v = nlay->valuedouble;
        if (!(v >= 0.0 && v <= 2.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_neon_layout");
        }
        boost_theme_set_neon_layout((boost_neon_layout_t)(int)v);
    }
```

`PUT /themes/active` already rebuilds the scene under the display lock, so switching between neon palettes takes effect immediately. Changing `neonLayout` does **not** rebuild on its own — it takes effect on the next scene build. Add that rebuild so the dropdown behaves like the theme picker:

```c
        if (boost_display_lock(1000) == ESP_OK) {
            boost_gauge_apply_theme(boost_model_active_theme());
            boost_display_unlock();
        }
```
Place it immediately after `boost_theme_set_neon_layout()`, inside the same `if`.

- [ ] **Step 3: Verify against the mock**

Build for the device is not required to test the shape. Confirm the field round-trips once Task 10 has updated the mock server.

- [ ] **Step 4: Commit**

```bash
git add main/boost_web.c
git commit -m "feat: expose neonLayout on the themes API"
```

---

### Task 10: Web mirror, settings control and mock server

**Files:**
- Modify: `web/app.js` — `drawGauge()` switch (line 471), new `drawNeonGauge()`, state defaults (line ~113), `queueThemeConfig` merge (line ~1685), hydration (line ~2500), theme editor control (near line 1774)
- Modify: `web/styles.css` — `@font-face` for the typeface
- Modify: `tools/mock_server.py` — `THEMES` (line 24), `CONFIG` (line 92), serialisation (line ~405), PUT parse (line ~615)
- Regenerate: `main/generated_web_assets.c/.h`

**Interfaces:**
- Consumes: the `neon` style token and `neonLayout` field.
- Produces: browser parity.

- [ ] **Step 1: Add the three themes to the mock server**

Append to `THEMES` in `tools/mock_server.py`:

```python
    {
        "id": "neon-violet",
        "name": "Neon Violet",
        "style": "neon",
        "colors": {
            "face": "#000000", "track": "#241038", "text": "#FFFFFF",
            "muted": "#5A3A7A", "vacuum": "#8B3DFF", "boost": "#FF2BD6",
            "overboost": "#FF6A00", "zero": "#FFFFFF",
        },
    },
    {
        "id": "neon-miami",
        "name": "Neon Miami",
        "style": "neon",
        "colors": {
            "face": "#000000", "track": "#10222E", "text": "#FFFFFF",
            "muted": "#3F6E80", "vacuum": "#00E5FF", "boost": "#FF2BD6",
            "overboost": "#FF2A00", "zero": "#FFFFFF",
        },
    },
    {
        "id": "neon-toxic",
        "name": "Neon Toxic",
        "style": "neon",
        "colors": {
            "face": "#000000", "track": "#12300A", "text": "#FFFFFF",
            "muted": "#4C7A2E", "vacuum": "#39FF14", "boost": "#FFF000",
            "overboost": "#FF00A0", "zero": "#FFFFFF",
        },
    },
```

Add `"neonLayout": 1,` to `CONFIG`, `"neonLayout": int(CONFIG.get("neonLayout", 1)),` to the themes payload, and to the PUT parse:

```python
            if "neonLayout" in payload and isinstance(payload["neonLayout"], int):
                if payload["neonLayout"] not in (0, 1, 2):
                    self.send_json({"error": "invalid_neon_layout"}, HTTPStatus.BAD_REQUEST)
                    return
                CONFIG["neonLayout"] = int(payload["neonLayout"])
```

- [ ] **Step 2: Add the mirror renderer**

In `web/app.js`, add `case "neon": return drawNeonGauge(sample, psi, g);` to the `drawGauge()` switch, add `neonLayout: 1,` to the state defaults, `if (payload.neonLayout !== undefined) state.neonLayout = Number(payload.neonLayout);` to the `queueThemeConfig` merge, and `state.neonLayout = Number(themes.neonLayout) ?? 1;` to hydration.

Write `drawNeonGauge()` following the `drawSportGauge()` house style: `ctx.save()`, `translate(cx, cy)`, `scale(scale, scale)`, work in the 466 design space, `ctx.restore()`. It must branch on `state.neonLayout` across the same three geometries and reproduce **behaviour**, not just colour — specifically the drawn sign mark and the constant-width digit cells. Every documented parity bug in this codebase was the mirror matching colours while ignoring a setting.

Port the cell layout directly from `boost_neon_layout_readout()` rather than relying on the browser's text metrics: the browser will happily kern the digits and the panel will not.

- [ ] **Step 3: Inline the typeface**

Add an `@font-face` to `web/styles.css` with the TTF base64-inlined, following the existing `"Alvida Fatface"` precedent, and use it for the neon readout. The sign mark is drawn with `ctx.fill()` paths, not text, matching the firmware.

- [ ] **Step 4: Add the layout dropdown**

In the theme editor in `web/app.js`, beside the Vault "Needle colour" selector at line 1774, add a control shown only for neon themes:

```js
    const lrow = document.createElement("label");
    lrow.className = "theme-select-row";
    const lselect = document.createElement("select");
    lselect.innerHTML = `
      <option value="0">Neon tube</option>
      <option value="1">Segment ring</option>
      <option value="2">Marquee</option>
    `;
    lselect.value = String(state.neonLayout ?? 1);
    lselect.addEventListener("change", () => {
      state.neonLayout = Number(lselect.value);
      scheduleGaugeRender();
      queueThemeConfig({ neonLayout: state.neonLayout });
    });
    const lname = document.createElement("span");
    lname.textContent = "Layout";
    lrow.append(lname, lselect);
    wrap.append(lrow);
```

- [ ] **Step 5: Verify in the browser**

```bash
python tools/mock_server.py --host 127.0.0.1 --port 18080
```

Open `http://127.0.0.1:18080/`, select each neon theme, and step the layout dropdown through all three. Confirm the sign mark appears at negative psi, the digits do not slide as the value crosses a `1`, and the dropdown persists across a reload.

- [ ] **Step 6: Regenerate the embedded assets**

Never hand-edit the generated files.

```bash
python tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
```

- [ ] **Step 7: Commit**

```bash
git add web/app.js web/styles.css tools/mock_server.py main/generated_web_assets.c main/generated_web_assets.h
git commit -m "feat: neon face web mirror and layout dropdown"
```

---

### Task 11: Documentation and hardware verification

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: measurements from Tasks 8 and 11.
- Produces: the ledger entry.

- [ ] **Step 1: Flash the board and measure**

```bash
idf.py -p <PORT> flash monitor
```

Confirm the boot log shows `panel up at 80 MHz QSPI`.

- [ ] **Step 2: Run the cadence guard per layout**

Turn pixel shift off first — its periodic full repaint contaminates the numbers.

```bash
python tools/check_display_cadence.py --url http://<board-ip> --seconds 30
```

Run once per layout. Compare `worstRenderUs`, `pixelsPerSecond` and `framesOverBudget`, not `renderFps` — `renderFps` counts render-ready events and is not a smoothness metric. Discard the first samples after each theme change, since scene build costs roughly 50 ms.

Expectations from the spec: `segments` should beat every existing face on `worstRenderUs`; `tube` should land near the arc face; `marquee` is expected to be the weakest. If marquee cannot hold the budget, reduce its numeral size rather than accepting a full-screen repaint.

- [ ] **Step 3: Confirm Dyno Cell is unchanged**

```bash
python tools/check_display_cadence.py --url http://<board-ip> --seconds 30
```
With `dyno-cell` active and demo mode on. Expected: min ≥ 60, median ~63, matching the recorded reference. Any regression here means the shared helpers were touched.

- [ ] **Step 4: Measure the flash delta**

```bash
python -c "import os; print(os.path.getsize('build/boost_gauge.bin'))"
```
Baseline is 2,125,296 B. Expected increase is about 40 KB from the fonts plus the face code. A materially larger delta means something other than the fonts grew — explain it before merging.

- [ ] **Step 5: Update README and the AGENTS ledger**

README: add the three themes to the theme list, describe the three layouts and the `neonLayout` setting, add the fonts to the font inventory table with their sources and sizes, and record the measured cadence numbers per layout beside the existing per-face baselines. Correct the stale ~1.38 MB app image figure to the measured value.

AGENTS.md: add a ledger entry recording the two typeface findings — the absent hyphen and plus glyphs, and the non-tabular digit advances — since both are non-obvious and would otherwise be rediscovered the hard way.

- [ ] **Step 6: Commit**

```bash
git add README.md AGENTS.md
git commit -m "docs: record the neon face, its layouts and its measurements"
```

---

### Task 12 (deferred, after Task 11): pre-rendered glow sprites for the ring

Not scheduled. Parked here so the numbers are not re-derived. The face currently
approximates the mockup's bloom with three concentric opaque bands, because LVGL
cannot blur and per-frame alpha is this pipeline's most expensive operation. The
question is whether pre-rendered A8 glow sprites beat vector rasterisation.

Three approaches were checked against this LVGL build, not from memory:

- **Box shadow** — `lv_draw_box_shadow` exists and works, but only on rounded
  rects. `lv_draw_arc_dsc_t` has no shadow field, and label shadows come from the
  object's background box rather than the glyphs. Usable for marquee's bar and
  bulbs, useless for the ring, the tube and every readout. This build does carry
  per-glyph `text_outline_stroke_width`, but it is implemented only in the
  FreeType vector-outline path, so it needs FreeType + ThorVG and does not apply
  to compiled bitmap fonts.
- **Canvas blur** — `lv_canvas_blur_hor`/`_ver` were removed in LVGL 9. Writing
  one is trivial; the problem is where it can run. The baked background is the
  *unlit* art, so a free blur there buys nothing, and a per-frame blur needs an
  offscreen render plus a composite, which breaks partial refresh.
- **Pre-rendered sprites** — the viable one. Numbers below.

Measured from the shipped artefacts (glyph boxes parsed from the generated font
files, ring boxes computed as exact annular sectors from the constants in
`boost_gauge.c`; only the glow margin is swept):

| glow margin | readout 108 | readout 154 | ring x54 | zone words x3 | total A8 |
| --- | --- | --- | --- | --- | --- |
| 8 px | 75.5 KB | 135.6 KB | 119.4 KB | 20.9 KB | 351 KB |
| 12 px | 90.4 KB | 155.2 KB | 163.8 KB | 26.9 KB | 436 KB |
| 16 px | 106.6 KB | 176.2 KB | 214.9 KB | 33.3 KB | 531 KB |
| 24 px | 143.3 KB | 222.4 KB | 337.3 KB | 46.9 KB | 750 KB |
| 32 px | 185.4 KB | 274.1 KB | 486.7 KB | 58.9 KB | 1.00 MB |

The app partition is 4 MB with 2.03 MB used, so every row fits in flash,
memory-mapped, costing no RAM. The format is forced, though: pre-tinted RGB565
across three zones is 2.56 MB and RGB565A8 is 3.83 MB, so it must be A8 plus
recolor — which means per-pixel alpha blending is unavoidable. The "opaque sprite
over black" shortcut does not survive the memory budget.

Dirty area per changed element, against today's 15,808 px per 108 px cell and
24,480 px per 154 px cell:

| glow margin | digit @108 | digit @154 | one ring segment |
| --- | --- | --- | --- |
| 8 px | 8,556 | 15,500 | 2,703 |
| 12 px | 10,100 | 17,556 | 3,599 |
| 16 px | 11,772 | 19,740 | 4,623 |
| 24 px | 15,500 | 24,492 | 7,055 |
| 32 px | 19,740 | 29,756 | 9,999 |

Flushed pixels go DOWN, not up. What changes is the cost per pixel.

**The overlap cliff.** Segment pitch at mid radius is 14.0 px, so a glow margin
under 14 has 3 sprites overlapping any point, 16-24 has 5, and 32 has 7. That
multiplier applies to every ring redraw and it is a step function at 14 px.

Two ledger rows pull opposite ways and both are load-bearing. AGENTS row 149:
the bottleneck is CPU rasterisation, not the link - 6% bus utilisation, and GIF
blits sustain 3.16 Mpx/s against the gauge's 0.9 on the same silicon. AGENTS row
206: this was already tried for the readout. `BOOST_HUD_READOUT_CACHE` expanded
12 glyphs to A8 in PSRAM and removed lookup work but not the dominant three-pass
coverage raster, and a 276x79 precomposed RGB565 tile prototype RAISED host flush
work to 21,175 px/cycle and was rejected before flashing.

The readout precedent plausibly does not transfer to the ring: that cache stored
glyph alphas, same shape and same three-pass raster with faster lookup, whereas a
glow sprite removes the mask generation entirely. The tile precedent does not
transfer either, since one tile for the whole readout dirties on any change while
per-glyph sprites keep per-cell invalidation. Both of those are reasoning, not
measurement.

**Scope if it goes ahead: ring only, 12 px margin, 164 KB.** Skip the readout -
largest memory line and the one with a prior negative result. Note that
`LV_BLEND_MODE_ADDITIVE` into RGB565 exists but lives in the generic per-pixel
branch rather than the NORMAL path that carries the optimised blenders, so using
it for overlap correctness could hand back the whole blit advantage; under one
segment pitch of margin the overlap is a single neighbour and plain NORMAL
blending may be close enough.

**Blocked on a measurement the simulator cannot produce.** The audit counts
pixels and treats them all alike; the deciding number is cost per blended pixel
versus per vector-rasterised pixel on this silicon. Per AGENTS row 192, commit
the harness before quoting any figure from it.

---

## Self-Review Notes

Spec coverage checked section by section. Every requirement maps to a task: themes and palettes to Task 2, the layout selector to Tasks 3/5/6/7, the readout and sign to Task 1, rendering rules to Tasks 5-8, fonts to Task 4, persistence to Task 3, the web mirror to Task 10, verification to Tasks 8 and 11.

Two things are deliberately staged rather than done once. The ring invalidation starts as a bounding box in Task 5 and tightens in Task 8, because a correct-but-wide dirty region is a working face and a narrow-but-wrong one is stale pixels. The layouts land one per task so each gets its own audit rather than three geometries being debugged at once.

One risk carried forward: the sign mark's two-triangle decomposition in Task 5 Step 3 is the least certain code in this plan. LVGL has no parallelogram primitive, and the fallback (upright rectangles) is specified inline so the task can complete either way.
