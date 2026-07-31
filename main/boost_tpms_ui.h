#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * This is the narrow hand-off between the page and the TPMS framework.  The
 * provider may fill these four fixed wheel positions; no radio/protocol state
 * belongs in the display module.  A framework that already declares this type
 * can define BOOST_TPMS_SNAPSHOT_DEFINED before including this header.
 */
#ifndef BOOST_TPMS_SNAPSHOT_DEFINED
#define BOOST_TPMS_SNAPSHOT_DEFINED 1
typedef struct boost_tpms_snapshot {
    float psi[4];
    bool valid[4];
    bool low_pressure[4];
    uint32_t age_ms;
} boost_tpms_snapshot;
typedef boost_tpms_snapshot boost_tpms_snapshot_t;
#endif

/** Create the 466x466 TPMS scene below parent. */
void boost_tpms_ui_create(lv_obj_t *parent);

/** Replace the fixed-slot values; NULL renders all four positions unavailable. */
void boost_tpms_ui_update(const boost_tpms_snapshot_t *snapshot);

/** Delete the scene's objects below its parent. */
void boost_tpms_ui_delete(void);

#ifdef __cplusplus
}
#endif
