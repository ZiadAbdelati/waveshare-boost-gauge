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
 * exercise the QR page, the swipe-to-toggles flip and dismissal. */
bool boost_page_qr_active(void);
/** True while the overlay shows the connections-toggles page. */
bool boost_page_qr_toggles(void);
void boost_page_qr_show(void);
/** Simulate a left-swipe drag across the overlay (start x to x-SWIPE_MIN_PX). */
void boost_page_qr_swipe_left(void);
/** Simulate a right-swipe drag on the toggles page (back to the QR page). */
void boost_page_qr_swipe_right(void);
/** Simulate a tap on a toggle SWITCH itself (0=OBD2, 1=App): raises
 *  VALUE_CHANGED on that switch exactly like an on-glass tap on the control. */
void boost_page_qr_tap_switch(int row);
/** Pending deferred-toggle request, or -1. Lets the sim assert the async
 *  request was queued (and later applied by the LVGL timer). */
int boost_page_qr_pending_toggle(void);
/** Simulate a fresh tap: hides the overlay entirely. */
void boost_page_qr_dismiss(void);

#ifdef __cplusplus
}
#endif
