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
 * the end of a dispatch table. */
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
 * `sign_gap` is measured from the first GLYPH's edge, not from the cell's,
 * which is why `font_px` is needed. The digits are not tabular and the spread
 * is extreme - '1' advances 409/1000 against '8' at 770 - so a gap measured
 * from the fixed cell edge reads as ~18 px beside a '1' and 0 px beside an
 * '8'. Measuring from the ink keeps it constant.
 *
 * `half_w` is what callers size the face against: the sign sits outside the
 * cell block, so the negative case is always the widest the readout gets.
 */
void boost_neon_layout_readout(float psi, int slot_w, int dot_w,
                               int sign_w, int sign_gap, int font_px,
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
 * Returns 0 and leaves the first and last outputs untouched when nothing is
 * lit. (Written out rather than as a pointer pair: the obvious spelling puts
 * a `/` immediately before a `*`, which opens a nested comment and the device
 * build compiles with -Werror=comment.)
 * Works in both directions, so vacuum lights anticlockwise of zero.
 */
int boost_neon_lit_span(float a_zero, float a_value, float a_start,
                        float span, int nseg, int *first, int *last);

#ifdef __cplusplus
}
#endif
