#include "boost_neon_geom.h"

#include <math.h>
#include <stddef.h>

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

/* Advance width and left side bearing per mille of em for '0'..'9', sampled
 * from the typeface. Both are needed: the ink is not centred in the advance
 * box. '1' advances only 409 but still carries a 133 bearing, so its ink sits
 * far right of the advance centre - centring on the advance alone left a 25 px
 * hole between the sign and the digit. */
const boost_neon_digit_metrics_t boost_neon_sf_metrics = {
    { 780, 409, 767, 753, 673, 767, 762, 677, 770, 763 },
    { 133, 133, 113,  92,  99, 107, 133, 126, 128, 147 },
    0,
};
/* Doto ROND=100 / wght=700 is tabular: every digit shares one advance and one
 * bearing (600 and 13 per mille). The post-processed period's ink centre sits
 * 100 units left of the digit ink centre, so shift it right by that amount to
 * balance the visible gaps on both sides. */
const boost_neon_digit_metrics_t boost_neon_doto_metrics = {
    { 600, 600, 600, 600, 600, 600, 600, 600, 600, 600 },
    { 13, 13, 13, 13, 13, 13, 13, 13, 13, 13 },
    100,
};

void boost_neon_layout_readout(float psi, int slot_w, int dot_w,
                               int sign_w, int sign_gap, int negative_shift,
                               int font_px,
                               const boost_neon_digit_metrics_t *metrics,
                               boost_neon_readout_t *out)
{
    if (out == NULL) return;
    if (metrics == NULL) metrics = &boost_neon_sf_metrics;
    const uint16_t *k_adv_per_mille = metrics->adv_per_mille;
    const uint16_t *k_lsb_per_mille = metrics->lsb_per_mille;

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
    out->cells[out->count].ch = '.';
    out->count++;
    out->cells[out->count].ch = (char)('0' + tenths);
    out->count++;

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
        if (out->cells[i].ch == '.') {
            out->cells[i].x += (int16_t)(((int32_t)metrics->dot_center_per_mille *
                                          (int32_t)font_px) / 1000);
        }
        x += width[i];
    }

    if (out->sign) {
        for (uint8_t i = 0; i < out->count; ++i) {
            out->cells[i].x += (int16_t)negative_shift;
        }
    }

    /* Hang the sign off the first glyph's ink edge rather than the cell edge,
     * so the visual gap does not swing with the digit that happens to be
     * there. */
    const int d0 = out->cells[0].ch - '0';
    const int gw = (d0 >= 0 && d0 <= 9)
                 ? (int)(((uint32_t)k_adv_per_mille[d0] * (uint32_t)font_px) / 1000u)
                 : slot_w;
    const int lsb = (d0 >= 0 && d0 <= 9)
                  ? (int)(((uint32_t)k_lsb_per_mille[d0] * (uint32_t)font_px) / 1000u)
                  : 0;
    /* Left edge of the INK, not of the advance box. */
    const int glyph_left = out->cells[0].x - gw / 2 + lsb;
    out->sign_x = (int16_t)(glyph_left - sign_gap - sign_w / 2);
    const int left_edge = out->sign ? -(out->sign_x - sign_w / 2)
                                    : total / 2;
    const int right_edge = total / 2 + (out->sign ? negative_shift : 0);
    out->half_w = (int16_t)(left_edge > right_edge ? left_edge : right_edge);
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
        /* Round, do not truncate. The mark is only ~14 px tall, so each bar's
         * share of the shear is well under a pixel and truncation collapsed it
         * to zero - leaving the lower bar sitting a pixel RIGHT of the upper
         * one, which reads as a left lean against right-leaning digits. */
        const int dx = (int)lroundf((float)(cy - (y0 + y1) / 2) * skew);
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

bool boost_neon_tube_dirty_span(float a_zero, float a_old, float a_new,
                                float a_start, float span, int nseg,
                                float *lo, float *hi)
{
    int of = 0, ol = 0, nf = 0, nl = 0;
    const bool old_lit = boost_neon_lit_span(a_zero, a_old, a_start, span,
                                             nseg, &of, &ol) > 0;
    const bool new_lit = boost_neon_lit_span(a_zero, a_new, a_start, span,
                                             nseg, &nf, &nl) > 0;
    if (old_lit == new_lit) return false;

    *lo = fminf(a_zero, fminf(a_old, a_new));
    *hi = fmaxf(a_zero, fmaxf(a_old, a_new));
    return true;
}

/* Painted range of a lit span once the baked zero marker is removed. The lit
 * span is zero-rooted, so the marker is always its first or last segment and
 * removal leaves one contiguous range - empty when the marker is all that is
 * lit. The two split-at-interior branches cannot occur from a zero-rooted
 * span (a_zero is always one endpoint of [lo,hi] in boost_neon_lit_span());
 * they are kept as bounded fallbacks rather than written as unreachable. */
static int neon_painted_seg(int first, int last, int zero_seg, int *a, int *b)
{
    if (first > last) return 0;
    if (zero_seg == first) {
        if (first == last) return 0;
        *a = first + 1;
        *b = last;
        return 1;
    }
    if (zero_seg == last) {
        if (first == last) return 0;
        *a = first;
        *b = last - 1;
        return 1;
    }
    *a = first;
    *b = last;
    return 1;
}

int boost_neon_seg_diff(float a_zero, float a_old, float a_new,
                        float a_start, float span, int nseg,
                        boost_neon_seg_diff_t *out)
{
    out->count = 0;
    if (nseg <= 0 || span <= 0.0f) return 0;

    int of = 0, ol = 0, nf = 0, nl = 0;
    const int old_n = boost_neon_lit_span(a_zero, a_old, a_start, span,
                                          nseg, &of, &ol);
    const int new_n = boost_neon_lit_span(a_zero, a_new, a_start, span,
                                          nseg, &nf, &nl);

    /* Baked zero marker, by the same floor() the draw uses via
     * neon_seg_index(). Clamped like boost_neon_lit_span() clamps its span
     * so a_zero outside the sweep cannot index past the ends. */
    const float step = span / (float)nseg;
    int z = (int)floorf((a_zero - a_start) / step);
    if (z < 0) z = 0;
    if (z > nseg - 1) z = nseg - 1;

    int oa, ob, na, nb;
    const bool old_paint = old_n > 0 &&
        neon_painted_seg(of, ol, z, &oa, &ob);
    const bool new_paint = new_n > 0 &&
        neon_painted_seg(nf, nl, z, &na, &nb);

    /* Interval symmetric difference of the two painted ranges. Each is one
     * range, so the XOR is at most two disjoint ranges (one range minus the
     * other can only ever produce a left and a right edge). */
    int cand[4][2];
    int ncand = 0;
    if (old_paint) {
        if (!new_paint || ob < na || oa > nb) {
            cand[ncand][0] = oa; cand[ncand][1] = ob; ncand++;
        } else {
            if (oa <= na - 1) { cand[ncand][0] = oa; cand[ncand][1] = na - 1; ncand++; }
            if (nb + 1 <= ob) { cand[ncand][0] = nb + 1; cand[ncand][1] = ob; ncand++; }
        }
    }
    if (new_paint) {
        if (!old_paint || nb < oa || na > ob) {
            cand[ncand][0] = na; cand[ncand][1] = nb; ncand++;
        } else {
            if (na <= oa - 1) { cand[ncand][0] = na; cand[ncand][1] = oa - 1; ncand++; }
            if (ob + 1 <= nb) { cand[ncand][0] = ob + 1; cand[ncand][1] = nb; ncand++; }
        }
    }

    /* Sort ascending and merge overlapping/adjacent ranges into minimal form.
     * ncand never exceeds 2 for two intervals, so this is cheap either way. */
    for (int i = 1; i < ncand; ++i) {
        const int f = cand[i][0], l = cand[i][1];
        int j = i;
        while (j > 0 && cand[j - 1][0] > f) {
            cand[j][0] = cand[j - 1][0];
            cand[j][1] = cand[j - 1][1];
            j--;
        }
        cand[j][0] = f; cand[j][1] = l;
    }
    out->count = 0;
    for (int i = 0; i < ncand; ++i) {
        if (out->count == 0) {
            out->first[0] = cand[i][0];
            out->last[0] = cand[i][1];
            out->count = 1;
        } else if (cand[i][0] <= out->last[out->count - 1] + 1) {
            if (cand[i][1] > out->last[out->count - 1])
                out->last[out->count - 1] = cand[i][1];
        } else if (out->count < 2) {
            out->first[1] = cand[i][0];
            out->last[1] = cand[i][1];
            out->count = 2;
        } else {
            /* Unreachable for two intervals; fold defensively to stay in
             * bounds rather than drop the segment. */
            if (cand[i][0] < out->first[0]) out->first[0] = cand[i][0];
            if (cand[i][1] > out->last[1]) out->last[1] = cand[i][1];
        }
    }
    return out->count;
}
