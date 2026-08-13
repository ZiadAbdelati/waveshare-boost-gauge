#include "boost_obd.h"

#include <string.h>

/* Compiled in place of the real OBD driver when BOOST_TPMS_BLE_ENABLED=n, so
 * callers (main.c, boost_web.c) need no #ifdef around the API. */

void boost_obd_init(void) {}
void boost_obd_set_enabled(bool enabled) { (void)enabled; }
bool boost_obd_enabled(void) { return false; }

void boost_obd_get_state(boost_obd_state_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}
