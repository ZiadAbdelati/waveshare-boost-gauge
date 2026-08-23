#include "boost_obd.h"

#include <string.h>

/* Compiled in place of the real OBD driver when BOOST_TPMS_BLE_ENABLED=n, so
 * callers (main.c, boost_web.c) need no #ifdef around the API. */

void boost_obd_init(void) {}
void boost_obd_set_enabled(bool enabled) { (void)enabled; }
bool boost_obd_enabled(void) { return false; }

/* BLE-less image: the shared NimBLE host never exists. Keep the host API the
 * companion peripheral and main.c call linkable and inert. */
bool boost_obd_ble_host_up(void) { return false; }
void boost_obd_ble_host_start(void) {}

void boost_obd_get_state(boost_obd_state_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
}
