#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "boost_tpms.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The display consumes the canonical TPMS snapshot owned by boost_tpms.h: the
 * four fixed wheel slots (FL, FR, RL, RR), their converted pressures and the
 * service-level status. No radio/protocol state belongs in this module.
 */

/** Create the 466x466 TPMS scene below parent. */
void boost_tpms_ui_create(lv_obj_t *parent);

/** Replace the fixed-slot values; NULL renders all four positions unavailable. */
void boost_tpms_ui_update(const boost_tpms_snapshot_t *snapshot);

/** Delete the scene's objects below its parent. */
void boost_tpms_ui_delete(void);

#ifdef __cplusplus
}
#endif
