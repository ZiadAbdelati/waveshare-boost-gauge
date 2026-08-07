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

/* Advance width and left side bearing per mille of em for '0'..'9', sampled
 * from the typeface. Both are needed: the ink is not centred in the advance
 * box. '1' advances only 409 but still carries a 133 bearing, so its ink sits
 * far right of the advance centre - centring on the advance alone left a 25 px
 * hole between the sign and the digit. */
static const uint16_t k_adv_per_mille[10] = {
    780, 409, 767, 753, 673, 767, 762, 677, 770, 763
};
static const uint16_t k_lsb_per_mille[10] = {
    133, 133, 113,  92,  99, 107, 133, 126, 128, 147
};

void boost_neon_layout_readout(float psi, int slot_w, int dot_w,
                               int sign_w, int sign_gap, int font_px,
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
    const int sign_edge = -(out->sign_x - sign_w / 2);
    out->half_w = (int16_t)(out->sign && sign_edge > total / 2 ? sign_edge
                                                               : total / 2);
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
