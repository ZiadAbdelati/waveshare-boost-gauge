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

/**
 * Tube dirty-region decision at the half-segment lit threshold.
 *
 * The tube layout is all-or-nothing: draw_neon_live() paints the whole run
 * from a_zero whenever boost_neon_lit_span() finds at least half a segment
 * lit, and nothing at all below that. When the value crosses that threshold
 * the painted area changes by the whole run, so a delta-only invalidation
 * between the two endpoints strands the zero-to-near-endpoint band (the
 * ring-stale audit caught this at r~228.8). This helper owns exactly that
 * decision so the renderer and the regression test share it.
 *
 * Returns true when old and new are on opposite sides of the lit threshold
 * (a lit<->unlit transition) and writes the complete dirty angular span,
 * zero through the farther endpoint, as [*lo, *hi]. Returns false when both
 * endpoints are lit or both unlit and leaves the outputs untouched, matching
 * the boost_neon_lit_span() convention; the caller then keeps its precise
 * delta-only span (a_old..a_new).
 *
 * The lit decision is taken with boost_neon_lit_span() itself, with the same
 * sweep arguments, so the helper cannot disagree with the draw. The span is
 * [min(a_zero, a_old, a_new), max(a_zero, a_old, a_new)], the same fmin/fmax
 * union used by the tube branch of update_neon(). The caller must keep its own side-flip and colour-
 * flip branches BEFORE this test: a run crossing zero while lit (or a zone
 * recolour) is not a threshold transition and is not this helper's decision.
 */
bool boost_neon_tube_dirty_span(float a_zero, float a_old, float a_new,
                                float a_start, float span, int nseg,
                                float *lo, float *hi);

/* Inclusive index ranges of segments whose painted state differs between the
 * old and new readings, on the Segments layout. */
typedef struct {
    int first[2];
    int last[2];
    int count;   /* 0, 1 or 2 ranges; only the first `count` entries are valid */
} boost_neon_seg_diff_t;

/**
 * Symmetric difference of the old and new PAINTED segment sets.
 *
 * draw_neon_live() paints every segment in
 * boost_neon_lit_span(a_zero, a_value, ...) except the baked zero marker (the
 * segment containing a_zero, which is always an endpoint of the lit run). On
 * a steady same-side frame the ring's pixels change on exactly the segments
 * whose painted state flipped - not on the whole angular delta, which also
 * reflushes the segment that merely happened to contain the old endpoint.
 * This helper is that minimal set, so the caller can repaint precisely what
 * the draw changes.
 *
 * The lit sets are taken with boost_neon_lit_span() itself (same arguments the
 * draw passes), so threshold and boundary behaviour - the half-segment lit
 * gate and floor() segment indexing - cannot disagree with the draw. The baked
 * zero marker is excluded from both painted sets before the difference, so it
 * can never appear in the output even when one side is unlit. Each painted set
 * is one contiguous range (the zero marker is always the lit run's first or
 * last segment), so their symmetric difference is at most two disjoint ranges.
 *
 * Returns out->count (0 when the painted sets are identical) and fills
 * out->first[]/out->last[] with inclusive segment indices in ascending order.
 * When nothing changes, count is zero and the range entries are untouched.
 *
 * The caller decides the branch, not this helper: side flips (runs on
 * opposite sides of the notch) and colour flips (a zone recolour) repaint
 * both runs in full regardless of which segments changed, so this is only the
 * same-side, same-colour steady case - exactly the branch that used to call
 * neon_inv_span(a_old, a_new) directly.
 */
int boost_neon_seg_diff(float a_zero, float a_old, float a_new,
                        float a_start, float span, int nseg,
                        boost_neon_seg_diff_t *out);

#ifdef __cplusplus
}
#endif
