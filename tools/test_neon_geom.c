#include "boost_neon_geom.h"
#include "boost_theme.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Segments symmetric-difference helper, against the production default sweep
 * (zeroAngle 236.25, ARC_START 135, ARC_RANGE 270, 54 segments, step 5). */
static void seg_diff_expect(float a_old, float a_new, int n,
                            int f0, int l0, int f1, int l1)
{
    boost_neon_seg_diff_t d;
    const int got = boost_neon_seg_diff(236.25f, a_old, a_new,
                                        135.0f, 270.0f, 54, &d);
    assert(got == n);
    assert(d.count == n);
    if (n >= 1) { assert(d.first[0] == f0); assert(d.last[0] == l0); }
    if (n >= 2) { assert(d.first[1] == f1); assert(d.last[1] == l1); }
}

int main(void)
{
    assert(boost_neon_layout_clamp(0) == BOOST_NEON_TUBE);
    assert(boost_neon_layout_clamp(1) == BOOST_NEON_SEGMENTS);
    assert(boost_neon_layout_clamp(2) == BOOST_NEON_MARQUEE);
    assert(boost_neon_layout_clamp(3) == BOOST_NEON_LAYOUT_DEFAULT);
    assert(boost_neon_layout_clamp(255) == BOOST_NEON_LAYOUT_DEFAULT);

    boost_neon_readout_t r;
    boost_neon_layout_readout(8.5f, 64, 33, 31, 8, 108, &r);
    assert(r.count == 3);
    assert(r.sign == false);
    assert(r.cells[0].ch == '8');
    assert(r.cells[1].ch == '.');
    assert(r.cells[2].ch == '5');
    assert(r.half_w == 80);
    assert(r.cells[1].x == 0);
    assert(r.cells[0].x == -48);
    assert(r.cells[2].x == 49);

    boost_neon_layout_readout(19.5f, 64, 33, 31, 8, 108, &r);
    assert(r.count == 4);
    assert(r.cells[0].ch == '1');
    assert(r.cells[1].ch == '9');
    assert(r.cells[3].ch == '5');
    assert(r.cells[2].x == 32);
    assert(r.half_w == 112);

    boost_neon_layout_readout(-12.0f, 64, 33, 31, 8, 108, &r);
    assert(r.count == 4);
    assert(r.sign == true);
    assert(r.cells[0].ch == '1');
    assert(r.cells[1].ch == '2');
    assert(r.cells[2].ch == '.');
    assert(r.cells[3].ch == '0');
    assert(r.half_w == 126);
    assert(r.half_w < 140);   /* clears the ring's inner edge */
    /* Sign hangs off the '1' glyph's ink edge: cell centre -78, glyph 42
     * wide, so ink starts at -99; gap 8 then half the 30 px mark. */
    assert(r.sign_x == -88 - 8 - 31 / 2);
    assert(r.cells[2].x == 32);

    boost_neon_layout_readout(8.95f, 64, 33, 31, 8, 108, &r);
    assert(r.cells[0].ch == '9');
    assert(r.cells[2].ch == '0');
    boost_neon_layout_readout(-0.02f, 64, 33, 31, 8, 108, &r);
    assert(r.sign == false);
    assert(r.cells[0].ch == '0');

    boost_neon_bar_t bars[BOOST_NEON_SIGN_BARS];
    boost_neon_sign_bars(0, 0, 108, 31, bars);
    assert(bars[0].y1 - bars[0].y0 == 6);
    assert(bars[1].y0 - bars[0].y0 == 8);
    assert(bars[0].y0 < 0 && bars[1].y1 > 0);
    assert(bars[0].xt0 > bars[0].xb0);
    assert(bars[0].xt1 - bars[0].xt0 == bars[0].xb1 - bars[0].xb0);
    assert(bars[0].xt0 - bars[0].xb0 == bars[1].xt0 - bars[1].xb0);

    int first = -1, last = -1;
    int n = boost_neon_lit_span(236.25f, 320.0f, 135.0f, 270.0f, 56,
                                &first, &last);
    assert(n > 0);
    assert(first == 21);
    assert(last == 38);
    n = boost_neon_lit_span(236.25f, 168.75f, 135.0f, 270.0f, 56,
                            &first, &last);
    assert(n > 0);
    assert(first == 6);
    assert(last == 21);
    assert(first <= last);
    first = last = -99;
    n = boost_neon_lit_span(236.25f, 236.25f, 135.0f, 270.0f, 56,
                            &first, &last);
    assert(n == 0);
    assert(first == -99 && last == -99);

    /* --- tube dirty span at the half-segment lit threshold ---------------- */
    /* The tube is all-or-nothing: within half a segment of the notch nothing
     * is lit, at half a segment or beyond the whole run lights. Crossing that
     * threshold flips the painted area by the whole run, so the dirty span
     * must run from the notch through the FARTHER endpoint, not the (tiny)
     * delta between the endpoints. Production uses 54 segments: step 5 deg,
     * so 2.49 deg from the notch is still unlit while 2.51 deg is lit - a 0.02 deg movement across the
     * threshold, well inside the 0.1 deg movement bound. */
    const float t_zero = 236.25f;
    static const struct {
        float a_old, a_new;
    } t_transitions[4] = {
        { t_zero + 2.49f, t_zero + 2.51f },  /* boost 0 -> 1 */
        { t_zero + 2.51f, t_zero + 2.49f },  /* boost 1 -> 0 */
        { t_zero - 2.49f, t_zero - 2.51f },  /* vacuum 0 -> 1 */
        { t_zero - 2.51f, t_zero - 2.49f },  /* vacuum 1 -> 0 */
    };
    for (int i = 0; i < 4; ++i) {
        const float a_old = t_transitions[i].a_old;
        const float a_new = t_transitions[i].a_new;
        assert(fabsf(a_new - a_old) <= 0.1f);
        float lo = -1.0f, hi = -1.0f;
        assert(boost_neon_tube_dirty_span(t_zero, a_old, a_new,
                                          135.0f, 270.0f, 54, &lo, &hi));
        /* The full dirty span includes the notch... */
        assert(lo <= t_zero + 1e-3f && t_zero - 1e-3f <= hi);
        /* ...and is much wider than the endpoint-only delta. */
        assert((hi - lo) > 5.0f * fabsf(a_new - a_old));
        /* The span is exactly zero through the farther endpoint. */
        assert(fabsf(lo - fminf(t_zero, fminf(a_old, a_new))) < 1e-3f);
        assert(fabsf(hi - fmaxf(t_zero, fmaxf(a_old, a_new))) < 1e-3f);
    }
    /* No lit-state change: both endpoints lit or both unlit keep the precise
     * delta-only span, so the helper must report no transition and leave the
     * outputs untouched. */
    {
        float lo = -1.0f, hi = -1.0f;
        assert(!boost_neon_tube_dirty_span(t_zero, t_zero + 3.0f, t_zero + 4.0f,
                                           135.0f, 270.0f, 54, &lo, &hi));
        assert(lo == -1.0f && hi == -1.0f);
        assert(!boost_neon_tube_dirty_span(t_zero, t_zero + 1.0f, t_zero + 2.0f,
                                           135.0f, 270.0f, 54, &lo, &hi));
        assert(lo == -1.0f && hi == -1.0f);
    }

    /* --- segments symmetric-difference invalidation ---------------------- */
    /* a_zero = 236.25 sits in segment 20 (floor(101.25 / 5)); the baked zero
     * marker is segment 20, so a boost run of length k paints [21..20+k] and
     * a vacuum run paints down from 19. Only the segments whose painted state
     * changed may be invalidated - the angular delta would also reflush the
     * segment that merely contained the old endpoint. */
    /* Boost expansion: 251.25 lights [21..23], 261.25 lights [21..25]; only
     * the two newly-lit segments differ. */
    seg_diff_expect(251.25f, 261.25f, 1, 24, 25, -1, -1);
    /* Retraction is the same symmetric difference. */
    seg_diff_expect(261.25f, 251.25f, 1, 24, 25, -1, -1);
    /* A one-segment step keeps just that segment. */
    seg_diff_expect(251.25f, 256.25f, 1, 24, 24, -1, -1);
    /* Vacuum expansion: 221.25 paints [17..19], 211.25 paints [15..19]. */
    seg_diff_expect(221.25f, 211.25f, 1, 15, 16, -1, -1);
    seg_diff_expect(211.25f, 221.25f, 1, 15, 16, -1, -1);
    seg_diff_expect(221.25f, 216.25f, 1, 16, 16, -1, -1);

    /* Lit/unlit threshold: within half a segment of the notch nothing is lit.
     * Crossing the threshold while the run still stops inside the zero segment
     * paints nothing either side (only the baked marker would light), so the
     * painted sets are identical and the diff is empty - including at the
     * exact 2.5 deg half-segment boundary. */
    seg_diff_expect(238.74f, 238.76f, 0, -1, -1, -1, -1);
    seg_diff_expect(238.74f, 238.75f, 0, -1, -1, -1, -1);
    seg_diff_expect(238.76f, 238.74f, 0, -1, -1, -1, -1);
    /* Unlit -> lit repaints the WHOLE newly-painted set, not the tiny angular
     * delta between the endpoints. */
    seg_diff_expect(238.74f, 256.25f, 1, 21, 24, -1, -1);
    seg_diff_expect(256.25f, 238.74f, 1, 21, 24, -1, -1);
    seg_diff_expect(233.76f, 216.25f, 1, 16, 19, -1, -1);
    seg_diff_expect(216.25f, 233.76f, 1, 16, 19, -1, -1);

    /* Exact segment boundaries: a value landing exactly on a boundary belongs
     * to the floor() segment and paints it fully, in both directions. */
    seg_diff_expect(250.0f, 255.0f, 1, 24, 24, -1, -1);
    seg_diff_expect(255.0f, 260.0f, 1, 25, 25, -1, -1);
    seg_diff_expect(260.0f, 255.0f, 1, 25, 25, -1, -1);
    /* Boundary between segments 0 and 1: floor() yields 1, so a vacuum run
     * stepping off it lights segment 1, never the out-of-range segment 0. */
    seg_diff_expect(140.0f, 145.0f, 1, 1, 1, -1, -1);

    /* Identical painted sets: same far segment (value moved within it) or the
     * exact same value produce no difference. */
    seg_diff_expect(251.25f, 253.0f, 0, -1, -1, -1, -1);
    seg_diff_expect(251.25f, 251.25f, 0, -1, -1, -1, -1);

    /* Zero segment exclusion: the baked marker (segment 20) is never in the
     * diff, even when one side is unlit and the other's whole run is new. */
    seg_diff_expect(246.25f, 238.74f, 1, 21, 22, -1, -1);
    seg_diff_expect(238.74f, 256.25f, 1, 21, 24, -1, -1);

    /* Two disjoint ranges: the helper returns the full symmetric difference
     * even across the notch (update_neon never asks for this - side flips are
     * repainted in full - but the helper stays a faithful set operation). */
    seg_diff_expect(261.25f, 211.25f, 2, 15, 19, 21, 25);

    /* --- theme table -------------------------------------------------- */
    /* The three neon palettes are ONE theme plus a preset, not three themes:
     * they were always one face with three colourways. */
    boost_theme_init();
    assert(boost_theme_count() == 5);

    const boost_theme_t *neon = boost_theme_find("neon");
    assert(neon != NULL);
    assert(neon->style == BOOST_STYLE_NEON);
    assert(neon->face == 0x000000u);

    /* The old ids are gone, so a stale one must not resolve - that is what the
     * config migration in boost_model.c exists to catch. */
    assert(boost_theme_find("neon-violet") == NULL);
    assert(boost_theme_find("neon-miami") == NULL);
    assert(boost_theme_find("neon-toxic") == NULL);

    /* Each preset repaints the live theme. */
    boost_theme_set_neon_preset(BOOST_NEON_PRESET_VIOLET);
    assert(boost_theme_neon_preset() == BOOST_NEON_PRESET_VIOLET);
    neon = boost_theme_find("neon");
    /* Vacuum was 0x8B3DFF, which through neon_lit()'s original PER-CHANNEL
     * clamp pinned both R and B to 255 - the same corner boost's 0xFF2BD6
     * pins to - so the two zones converged on one post-bloom magenta. The
     * workaround was to darken it to 0x4A1FFF, which cost the palette its
     * name: at 74 red against 255 blue it blooms to (101,64,255), an indigo.
     * neon_lit() no longer clamps per channel, so the bright base is safe
     * again; 0x7B00FF blooms to (143,51,255), the violet this palette is
     * supposed to be. */
    assert(neon->vacuum == 0x7B00FFu);
    assert(neon->boost == 0xFF2BD6u);
    assert(neon->overboost == 0xFF1500u);

    boost_theme_set_neon_preset(BOOST_NEON_PRESET_MIAMI);
    neon = boost_theme_find("neon");
    assert(neon->vacuum == 0x00E5FFu);
    /* Overboost is orange-red, not pink: against boost magenta the pink
     * could not be told apart at a glance. */
    assert(neon->overboost == 0xFF2A00u);

    boost_theme_set_neon_preset(BOOST_NEON_PRESET_TOXIC);
    neon = boost_theme_find("neon");
    assert(neon->vacuum == 0x39FF14u);
    assert(neon->boost == 0xFFF000u);

    boost_theme_set_neon_preset(BOOST_NEON_PRESET_BLOODMOON);
    assert(boost_theme_neon_preset() == BOOST_NEON_PRESET_BLOODMOON);
    neon = boost_theme_find("neon");
    assert(neon->vacuum == 0x0064FFu);
    assert(neon->boost == 0xC4172Eu);
    /* Overboost is orange against a crimson boost - adjacent warm hues, and
     * the narrowest post-bloom separation of any palette here (77 units).
     * Specified deliberately; see the note in boost_theme.c. */
    assert(neon->overboost == 0xFF6A00u);

    /* --- reset and "customized" follow the PRESET, not s_defaults[] ----- */
    /* s_defaults[] holds Violet, because some palette had to be the compiled-in
     * one. Both of these used to compare/restore against it, so selecting any
     * other preset immediately reported the theme as customized, and resetting
     * painted Violet's colours over the selected preset while leaving the
     * selector where it was - the two disagreed until the selector was cycled
     * away and back. */
    boost_theme_set_neon_preset(BOOST_NEON_PRESET_TOXIC);
    assert(!boost_theme_is_customized("neon"));   /* selecting != customizing */

    boost_theme_colors_t edited = { .vacuum = 0x112233u, .boost = 0x445566u,
                                    .overboost = 0x778899u };
    boost_theme_set_colors("neon", &edited);
    assert(boost_theme_is_customized("neon"));

    assert(boost_theme_reset_colors("neon"));
    /* Back to TOXIC, not to Violet, and the preset has not moved. */
    assert(boost_theme_neon_preset() == BOOST_NEON_PRESET_TOXIC);
    neon = boost_theme_find("neon");
    assert(neon->vacuum == 0x39FF14u);
    assert(neon->boost == 0xFFF000u);
    assert(neon->overboost == 0xFF00A0u);
    assert(!boost_theme_is_customized("neon"));

    /* Out of range clamps to the default rather than indexing off the end. */
    boost_theme_set_neon_preset((boost_neon_preset_t)99);
    assert(boost_theme_neon_preset() == BOOST_NEON_PRESET_VIOLET);

    assert(strcmp(boost_style_name(BOOST_STYLE_NEON), "neon") == 0);

    /* The four originals keep their identity and their order. */
    assert(strcmp(boost_theme_at(0)->id, "dyno-cell") == 0);
    assert(strcmp(boost_theme_at(3)->id, "big-digit") == 0);
    assert(strcmp(boost_theme_at(4)->id, "neon") == 0);

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

    printf("neon geom: all assertions passed\n");
    return 0;
}
