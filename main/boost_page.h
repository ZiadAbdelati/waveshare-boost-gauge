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
/** Step FORWARD one page (0 -> 1 -> 2 -> 0). */
void boost_page_qr_swipe_left(void);
/** Step BACKWARD one page (0 -> 2 -> 1 -> 0). */
void boost_page_qr_swipe_right(void);
/** Simulate a tap on a toggle SWITCH itself: the row is PAGE-LOCAL, so page 1
 *  row 0/1 = OBD BLE / APP BLE and page 2 row 0/1 = UNITS / REL/ABS. Raises
 *  CLICKED on that square exactly like an on-glass tap on the control. */
void boost_page_qr_tap_switch(int row);
/** Pending deferred-toggle request, or -1. Lets the sim assert the async
 *  request was queued (and later applied by the LVGL timer). */
int boost_page_qr_pending_toggle(void);
/** Simulate a fresh tap: hides the overlay entirely. */
void boost_page_qr_dismiss(void);

#ifdef __cplusplus
}
#endif
