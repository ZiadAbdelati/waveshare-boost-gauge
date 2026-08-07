#include "boost_neon_geom.h"
#include "boost_theme.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

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
