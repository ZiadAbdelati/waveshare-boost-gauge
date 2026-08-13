#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * BLE central transport for a serial-UART OBD-II adapter (ELM327-over-GATT).
 * Adapter-agnostic: services/characteristics are discovered at runtime against
 * the known ELM BLE profile families, with a generic first-writable / first-
 * notifiable fallback for anything else. The link is an unauthenticated byte
 * stream; the caller (boost_obd_elm) owns command framing and pacing.
 */

typedef enum {
    BOOST_OBD_BLE_DOWN = 0,      /* not started, or disabled */
    BOOST_OBD_BLE_SCANNING,      /* looking for an adapter */
    BOOST_OBD_BLE_CONNECTING,    /* connect in progress */
    BOOST_OBD_BLE_DISCOVERING,   /* services/characteristics */
    BOOST_OBD_BLE_READY,         /* byte stream usable */
    BOOST_OBD_BLE_DISCONNECTED,  /* link dropped; reconnect loop active */
} boost_obd_ble_state_t;

typedef void (*boost_obd_ble_rx_cb_t)(const uint8_t *data, size_t len, void *ctx);

/* Mount the NimBLE host once. Idempotent; safe to call even when the radio is
 * later disabled. */
void boost_obd_ble_init(void);

/* Begin the scan/connect/reconnect loop. No-op if already running. */
void boost_obd_ble_start(void);

/* Disconnect, cancel any scan and go idle (state DOWN). */
void boost_obd_ble_stop(void);

/* Queue a write to the adapter's TX characteristic. Returns true when the
 * write was queued (delivery is asynchronous); false when the link is not
 * READY. */
bool boost_obd_ble_send(const uint8_t *data, size_t len);

/* Receive callback for adapter notifications. Invoked on the NimBLE host task;
 * must be fast and non-blocking. */
void boost_obd_ble_set_rx_cb(boost_obd_ble_rx_cb_t cb, void *ctx);

boost_obd_ble_state_t boost_obd_ble_state(void);

/* Last link-failure status (NimBLE error code); 0 while healthy. */
uint16_t boost_obd_ble_last_error(void);

/* Name/address of the last connected peer (advertised name if seen, else ""). */
const char *boost_obd_ble_peer_name(void);
const char *boost_obd_ble_peer_addr(void);

/* Milliseconds the current link has been READY (0 while not ready). */
uint32_t boost_obd_ble_uptime_ms(void);

#ifdef __cplusplus
}
#endif
