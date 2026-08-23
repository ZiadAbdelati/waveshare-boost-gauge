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

/** Display hook for the six-digit pairing passkey; rendered by
 *  boost_app_ble_ui.c as a panel overlay. The callback runs on the NimBLE host
 *  task and must not touch LVGL directly. */
typedef void (*boost_app_ble_passkey_cb_t)(uint32_t passkey, void *ctx);

/** Pair-result hook: pairing finished (BLE_GAP_EVENT_ENC_CHANGE; ok=true on
 *  success) or a repeat-pairing attempt arrived. The passkey overlay dismisses
 *  on any call. Runs on the NimBLE host task. */
typedef void (*boost_app_ble_pair_result_cb_t)(bool ok, void *ctx);

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

/** Install the passkey display hook; pass NULL to clear. */
void boost_app_ble_set_passkey_display_cb(boost_app_ble_passkey_cb_t cb, void *ctx);

/** Install the pair-result hook; pass NULL to clear. */
void boost_app_ble_set_pair_result_cb(boost_app_ble_pair_result_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif
