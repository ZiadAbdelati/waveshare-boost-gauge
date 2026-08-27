#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Companion-app BLE peripheral: a dual-role GATT server that shares the one
 * NimBLE host/controller with the OBD2 BLE central (boost_obd_ble.c). This
 * module never calls nimble_port_init()/nimble_port_freertos_init(); the host
 * is mounted by boost_obd_ble_init() (the RAM-guarded shared path) and the
 * GATT service definitions are registered from boost_app_ble_init() BEFORE
 * the host task starts so ble_hs_start() registers them authoritatively.
 *
 * Radio activity is opt-in and persisted: appBle defaults off (NVS "boost" /
 * "app_ble", the same namespace/format as tpmsBle), and a fresh boot never
 * touches the radio unless either persisted toggle is on.
 */

/**
 * Register the companion GATT service definitions, set the shared host's
 * security config, and create this module's serializing driver task.
 * Idempotent. MUST be called before the first boost_obd_ble_init()/host task
 * start (main.c guarantees this ordering). Does not mount the host itself.
 */
void boost_app_ble_init(void);

/** Persisted toggle, default OFF. Mirrors tpmsBle's storage semantics. */
bool boost_app_ble_enabled(void);

/** Persist the toggle, then start/stop the peripheral live. */
void boost_app_ble_set_enabled(bool enabled);

/** Begin/end connectable advertising. Idempotent; safe before host up (the
 *  driver retries until the shared host is mounted). */
void boost_app_ble_start(void);
void boost_app_ble_stop(void);

/** True while exactly one phone is connected to the companion peripheral. */
bool boost_app_ble_connected(void);

/* Rebuild the BLE device-info JSON with the current STA IP so BLE-only
 * clients can learn the board's HTTP host and fetch the full log ring. */
void boost_app_ble_set_sta_ip(const char *ip);

#ifdef __cplusplus
}
#endif
