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

/** Process page gestures; call from LVGL input event context. */
void boost_page_handle_event(lv_event_t *event);

#ifdef __cplusplus
}
#endif
