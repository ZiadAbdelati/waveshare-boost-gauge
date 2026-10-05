/*
 * Host test: the persisted quarter turn must actually land on the visible glass,
 * with the touch frame rotated to match it.
 *
 * The LVGL adapter refuses to rotate a PANEL_IF_OTHER display, so the turn is
 * applied to the CO5300's scan order at panel init from
 * boost_theme_panel_orientation() (main/boost_theme.c). That table is easy to
 * get subtly wrong - a transposed mirror axis or a gap left on the wrong side
 * still produces a *rotated* picture, just shifted, clipped, or with the touch
 * landing on mirrored coordinates - and the only other way to catch it is a
 * physical panel run.
 *
 * So this test does not restate the table: it models the panel independently
 * and checks the observable behaviour.
 *   1. The panel flags must be the adapter's own rotation matrix (its
 *      lcd_orientation_helper table), not a neighbour's.
 *   2. A full-screen logical window plus the gap must cover exactly the visible
 *      466x466 area the vendor init insets at (6, 0) inside the 480x480 GRAM.
 *   3. TAP SIMULATION: for every rotation and a set of sample points, take the
 *      logical position of a drawn element, project it to the glass with the
 *      rotation, invert the rotation-0 touch calibration to get what the
 *      CST9217 would report, and require the touch flags to map that report back
 *      to the element's logical position. This is the property a user actually
 *      feels; the composed flags are the inverse of the panel turn composed with
 *      the rotation-0 baseline, NOT the forward turn.
 *   4. Rotation 0 is pinned to the vendor geometry, and a non-quarter value must
 *      fall back to it rather than to a guess.
 *
 * Wired in sim/CMakeLists.txt as `test_panel_orientation`, like test_neon_geom.
 */

#include "boost_theme.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* CO5300 GRAM and the panel's visible area, as the vendor init sets them. */
#define GRAM        480
#define VISIBLE     466
#define VISIBLE_X0    6
#define VISIBLE_Y0    0
#define LAST        (VISIBLE - 1)

typedef struct { int m00, m01, m10, m11; } mat_t;

/* Same algebra as the adapter's lcd_orientation_helper: swap exchanges the two
 * axes, mirror_x negates the x axis, mirror_y negates the y axis. */
static mat_t mat_from_flags(bool swap_xy, bool mirror_x, bool mirror_y)
{
    int xx = 1, xy = 0, yx = 0, yy = 1;
    if (swap_xy) {
        const int tx = xx, ty = xy;
        xx = yx; xy = yy;
        yx = tx; yy = ty;
    }
    if (mirror_x) { xx = -xx; xy = -xy; }
    if (mirror_y) { yx = -yx; yy = -yy; }
    mat_t m = { xx, xy, yx, yy };
    return m;
}

/* The adapter's rotation -> panel flag table (its matrix_from_rotation). */
static mat_t mat_for_rotation(unsigned deg)
{
    switch (deg) {
    case 90:  return mat_from_flags(true, true, false);
    case 180: return mat_from_flags(false, true, true);
    case 270: return mat_from_flags(true, false, true);
    default:  return mat_from_flags(false, false, false);
    }
}

/* Where the panel puts a logical point: the inverse of the adapter's own flush
 * offset arithmetic (rotate_copy_region), on the square 466x466 frame. */
static void panel_map(unsigned deg, int lx, int ly, int *px, int *py)
{
    switch (deg) {
    case 90:  *px = LAST - ly; *py = lx;              break;
    case 180: *px = LAST - lx; *py = LAST - ly;       break;
    case 270: *px = ly;        *py = LAST - lx;       break;
    default:  *px = lx;        *py = ly;              break;
    }
}

/* What esp_lcd_touch does with a raw point, in its real order
 * (managed_components/espressif__esp_lcd_touch/esp_lcd_touch.c:91-104):
 * mirror_x, then mirror_y, then swap_xy. Mirror and swap do NOT commute, so
 * modelling the order wrongly inverts the composition and silently validates
 * the wrong flags - which is exactly what happened once (see the ledger row). */
static void flags_apply(bool swap_xy, bool mirror_x, bool mirror_y,
                        int x, int y, int *ox, int *oy)
{
    if (mirror_x) x = LAST - x;
    if (mirror_y) y = LAST - y;
    if (swap_xy) { const int t = x; x = y; y = t; }
    *ox = x; *oy = y;
}

/* The rotation-0 calibration this repo ships: raw -> logical. */
#define BASE_SWAP false
#define BASE_MX   true
#define BASE_MY   true

/* Physical coverage of a full-screen logical window. */
static void coverage(const boost_panel_orientation_t *o,
                     int *col_lo, int *col_hi, int *row_lo, int *row_hi)
{
    /* The driver adds the gap to the logical window and sends the result to
     * CASET (x counter) and RASET (y counter) without swapping axes; swap_xy
     * exchanges which physical axis each counter addresses. */
    const int a_lo = o->gap_x, a_hi = LAST + o->gap_x;
    const int b_lo = o->gap_y, b_hi = LAST + o->gap_y;
    const bool x_drives_cols = !o->swap_xy;
    const int hi = GRAM - 1;

    *col_lo = x_drives_cols ? (o->mirror_x ? hi - a_hi : a_lo) : (o->mirror_x ? hi - b_hi : b_lo);
    *col_hi = x_drives_cols ? (o->mirror_x ? hi - a_lo : a_hi) : (o->mirror_x ? hi - b_lo : b_hi);
    *row_lo = x_drives_cols ? (o->mirror_y ? hi - b_hi : b_lo) : (o->mirror_y ? hi - a_hi : a_lo);
    *row_hi = x_drives_cols ? (o->mirror_y ? hi - b_lo : b_hi) : (o->mirror_y ? hi - a_lo : a_hi);
}

int main(void)
{
    const unsigned rotations[] = { 0, 90, 180, 270 };
    const int samples[][2] = { {0, 0}, {100, 200}, {200, 100}, {LAST, LAST}, {63, LAST}, {LAST, 0} };
    const size_t n_samples = sizeof(samples) / sizeof(samples[0]);
    int checks = 0;

    for (size_t i = 0; i < sizeof(rotations) / sizeof(rotations[0]); ++i) {
        const unsigned deg = rotations[i];
        boost_panel_orientation_t o;
        boost_theme_panel_orientation((uint16_t)deg, &o);
        checks++;

        /* 1. The panel flags are this rotation's, not a neighbour's. */
        const mat_t want = mat_for_rotation(deg);
        const mat_t got = mat_from_flags(o.swap_xy, o.mirror_x, o.mirror_y);
        if (want.m00 != got.m00 || want.m01 != got.m01 ||
            want.m10 != got.m10 || want.m11 != got.m11) {
            fprintf(stderr,
                    "FAIL %u deg: panel flags (swap=%d mx=%d my=%d) are not the "
                    "adapter's rotation matrix\n", deg, o.swap_xy, o.mirror_x, o.mirror_y);
            return 1;
        }

        /* 2. The gap puts a full-screen window exactly on the visible area. */
        int clo, chi, rlo, rhi;
        coverage(&o, &clo, &chi, &rlo, &rhi);
        checks++;
        if (clo != VISIBLE_X0 || chi != VISIBLE_X0 + LAST ||
            rlo != VISIBLE_Y0 || rhi != VISIBLE_Y0 + LAST) {
            fprintf(stderr,
                    "FAIL %u deg: gap %d,%d covers cols %d..%d rows %d..%d, expected "
                    "cols %d..%d rows %d..%d\n",
                    deg, o.gap_x, o.gap_y, clo, chi, rlo, rhi,
                    VISIBLE_X0, VISIBLE_X0 + LAST, VISIBLE_Y0, VISIBLE_Y0 + LAST);
            return 1;
        }

        /* 3. Tap simulation: the flags must return the logical point that the
         *    panel drew at the physical point the user touched. */
        for (size_t s = 0; s < n_samples; ++s) {
            const int lx = samples[s][0], ly = samples[s][1];
            int px, py, rx, ry, bx, by;
            panel_map(deg, lx, ly, &px, &py);
            /* The controller reports in its own frame, so what it sends is the
             * inverse of the rotation-0 calibration applied to the glass point.
             * That calibration is its own inverse (swap=0, both mirrors), so it
             * is applied once - and if the baseline ever stops being an
             * involution, this is the line that must invert it properly. */
            flags_apply(BASE_SWAP, BASE_MX, BASE_MY, px, py, &rx, &ry);
            flags_apply(o.touch_swap_xy, o.touch_mirror_x, o.touch_mirror_y,
                        rx, ry, &bx, &by);
            checks++;
            if (bx != lx || by != ly) {
                fprintf(stderr,
                        "FAIL %u deg: tap at logical (%d,%d) -> glass (%d,%d) -> raw "
                        "(%d,%d) -> flags give (%d,%d); touch does not follow the "
                        "rotated content\n", deg, lx, ly, px, py, rx, ry, bx, by);
                return 1;
            }
        }
    }

    /* 4a. The unrotated frame is what every existing build ships: pin it. */
    boost_panel_orientation_t zero;
    boost_theme_panel_orientation(0, &zero);
    checks++;
    if (zero.swap_xy || zero.mirror_x || zero.mirror_y ||
        zero.gap_x != VISIBLE_X0 || zero.gap_y != VISIBLE_Y0 ||
        zero.touch_swap_xy != BASE_SWAP || zero.touch_mirror_x != BASE_MX ||
        zero.touch_mirror_y != BASE_MY) {
        fprintf(stderr, "FAIL 0 deg: the unrotated frame changed from the vendor geometry\n");
        return 1;
    }

    /* 4b. An unknown value must fall back to rotation 0 rather than a guess. */
    boost_panel_orientation_t odd;
    boost_theme_panel_orientation(45, &odd);
    checks++;
    if (memcmp(&odd, &zero, sizeof(zero)) != 0) {
        fprintf(stderr, "FAIL 45 deg: a non-quarter turn must fall back to rotation 0\n");
        return 1;
    }

    printf("test_panel_orientation: PASS (%d checks: 4 rotations x {panel matrix,\n"
           "visible-window coverage, %zu tap simulations} + rotation-0 pin +\n"
           "non-quarter fallback)\n", checks, n_samples);
    return 0;
}
