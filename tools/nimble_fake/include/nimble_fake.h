#pragma once
/*
 * Test-facing control surface of the fake NimBLE host/controller.
 *
 * The fake owns a pool of connection records (CONFIG_BT_NIMBLE_MAX_CONNECTIONS
 * == 3), a single connect procedure and a single scan procedure, plus an
 * asynchronous "controller" thread that delivers GAP events to the callback the
 * driver registered. Everything here exists so a test can drive the ordering
 * the real controller would produce nondeterministically.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "host/ble_gap.h"

/* Wipe all controller/host state (connections, procedures, deliveries, NVS).
 * Call before nimble_port_init(). */
void fake_reset(void);

/* Seed the fake NVS so the driver's load_peer() finds a stored peer. */
void fake_nvs_seed_peer(const ble_addr_t *addr);

/* Connection-completion control.
 * auto_connect ON (default): a pending connect procedure completes by itself
 *   after fake_set_connect_delay_ms().
 * OFF: only fake_conn_complete_now() completes it. */
void fake_set_auto_connect(bool on);
void fake_set_connect_delay_ms(uint32_t ms);

/* True while a ble_gap_connect() procedure is in flight. */
bool fake_conn_pending(void);
/* Complete the pending connect procedure now (the controller wins a racing
 * cancel, exactly as NimBLE's ble_gap_rx_conn_complete does for status 0).
 * Returns true if a procedure was in flight. */
bool fake_conn_complete_now(void);
/* Controller acknowledges a cancel: procedure cleared and CONNECT(EAPP)
 * delivered. Returns true if a procedure was in flight. */
bool fake_conn_cancel_complete(void);

/* Force a link loss on a live connection: frees the record and delivers
 * DISCONNECT asynchronously. Returns true if the handle existed. */
bool fake_drop_link(uint16_t conn_handle);

/* Plant a live connection record with NO CONNECT event delivered -- the state
 * an ignored CONNECT leaves behind (NimBLE does no cleanup for it). The next
 * ble_gap_connect() to that peer then returns BLE_HS_EDONE, as on the glass.
 * Returns true if a connection slot was free. */
bool fake_phantom_conn(const ble_addr_t *addr);

/* Live connection table. */
int  fake_conn_count(void);
bool fake_conn_addr(int index, ble_addr_t *out_addr, uint16_t *out_handle);
bool fake_conn_handle_of(const ble_addr_t *addr, uint16_t *out_handle);
int  fake_last_connect_rc(void);
int  fake_scan_active(void);
uint32_t fake_last_disconnect_reason(void);

/* Bounded polling helpers (ms). */
bool fake_wait_conn_count(int want, uint32_t timeout_ms);
bool fake_wait_conn_pending(uint32_t timeout_ms);

/* Render the connection table, e.g. "[(h=1 66:55:44:33:22:11)]" or "[]". */
void fake_conn_table_str(char *buf, size_t len);
/* Render the last rc ble_gap_connect() returned. */
const char *fake_rc_str(int rc);
