#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "boost_sim.h"
#include "boost_tpms_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOST_PAGE_BOOST = 0,
    BOOST_PAGE_TPMS = 1,
} boost_page_id_t;

/** Create the persistent page roots and the initial boost scene. */
void boost_page_create(void);

/** Feed the current MAP sample to the active boost scene. */
void boost_page_update(const boost_sample_t *sample);

/** Feed a framework-owned TPMS snapshot to the TPMS page. */
void boost_page_update_tpms(const boost_tpms_snapshot_t *snapshot);

/** Return the active page. */
boost_page_id_t boost_page_active(void);

/** Force a page without a gesture (test/remote integration hook). */
void boost_page_show(boost_page_id_t page);

/* --- Host-sim / test hooks (compiled everywhere, tiny, side-effect free) ---
 * The headless sim has no multi-touch input device, so the two-finger hold
 * cannot be synthesised there. These hooks drive the exact same overlay code
 * the gesture path uses (show_qr/hide_qr/qr_pressing_cb), letting the sim
 * exercise all three pages, the swipe paging and dismissal. */
bool boost_page_qr_active(void);
/** Overlay page, or -1 while the overlay is closed.
 *  0 = QR, 1 = Connections (OBD BLE, APP BLE), 2 = Units (UNITS, REL/ABS). */
int boost_page_qr_page(void);
/** Open the overlay on page 0 (the QR page). No-op while it is already open. */
void boost_page_qr_show(void);
/** Open (or, while already open, jump to) a page: 0..2, out-of-range ignored. */
void boost_page_qr_show_page(int page);
/** Step FORWARD one page (0 -> 1 -> 2 -> 0): a leftward flick driven through
 *  the production drag classifier, not qr_step() directly. */
void boost_page_qr_swipe_left(void);
/** Step BACKWARD one page (0 -> 2 -> 1 -> 0); a rightward flick. */
void boost_page_qr_swipe_right(void);
/* --- Raw gesture injection (same functions the on-glass callbacks use) ------
 * The sim has no pointer device, so a press/drag/release is injected here and
 * runs the production gesture state machine and classifier. */
/** Touch-down at (x, y): seeds the gesture origin. */
void boost_page_qr_press(int x, int y);
/** One PRESSING sample at (x, y). */
void boost_page_qr_move(int x, int y);
/** Release: runs the RELEASED cleanup and the CLICKED that follows it, which
 *  dismisses the overlay only if the gesture never moved past the tap slop. */
void boost_page_qr_release(void);
/** press -> move -> release, one move sample: a complete flick. */
void boost_page_qr_drag(int x0, int y0, int x1, int y1);
/** move -> move -> release with NO touch-down: the first sample must establish
 *  its own origin (the path a background press takes when the tracker still
 *  holds the previous gesture's point - the stale-origin regression). */
void boost_page_qr_drag_unseeded(int x0, int y0, int x1, int y1);
/** A drag that STARTS on a toggle switch: models the square owning the whole
 *  PRESSED/PRESSING/RELEASED stream (plus the CLICKED it raises while it is
 *  still alive), so the switch's own drag rules are exercised - a drag is not a
 *  tap there either. Row is PAGE-LOCAL: page 1 = 0 OBD BLE / 1 APP BLE,
 *  page 2 = 0 UNITS / 1 REL/ABS. */
void boost_page_qr_switch_gesture(int row, int dx, int dy);
/** Simulate a tap on a toggle SWITCH itself: the row is PAGE-LOCAL, so page 1
 *  row 0/1 = OBD BLE / APP BLE and page 2 row 0/1 = UNITS / REL/ABS. Raises
 *  CLICKED on that square exactly like an on-glass tap on the control. */
void boost_page_qr_tap_switch(int row);
/** Pending deferred-toggle request, or -1. Lets the sim assert the async
 *  request was queued (and later applied by the LVGL timer). */
int boost_page_qr_pending_toggle(void);
/** The secondary line currently rendered inside a toggle square ("" if the
 *  square is absent). This is the RENDERED state, not the link state: it is
 *  read back from the built widget, so it only reports a toggle once something
 *  has repainted the overlay - which is the property the sim asserts. */
const char *boost_page_qr_switch_text(int row);
/** Simulate a fresh tap: hides the overlay entirely. */
void boost_page_qr_dismiss(void);

#ifdef __cplusplus
}
#endif
