#pragma once
/* Umbrella host header, as included by the driver. */
#include "host/ble_errors.h"
#include "host/ble_uuid.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs_adv.h"

struct ble_hs_conn;

/* Normally private to the host (ble_hs_priv.h); modelled here because GAP
 * procedure admission (fact 1) keys on it. */
struct ble_hs_conn *ble_hs_conn_find_by_addr(const ble_addr_t *addr);
