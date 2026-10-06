#pragma once
/* UUID types, mirroring host/ble_uuid.h layouts exactly (offsets matter:
 * the driver reads .u.type and ble_uuid_u16() casts the same struct). */
#include <stdint.h>

struct os_mbuf;

enum {
    BLE_UUID_TYPE_16  = 16,
    BLE_UUID_TYPE_32  = 32,
    BLE_UUID_TYPE_128 = 128,
};

typedef struct {
    uint8_t type;
} ble_uuid_t;

typedef struct {
    ble_uuid_t u;
    uint16_t   value;
} ble_uuid16_t;

typedef struct {
    ble_uuid_t u;
    uint32_t   value;
} ble_uuid32_t;

typedef struct {
    ble_uuid_t u;
    uint8_t    value[16];
} ble_uuid128_t;

typedef union {
    ble_uuid_t   u;
    ble_uuid16_t u16;
    ble_uuid32_t u32;
    ble_uuid128_t u128;
} ble_uuid_any_t;

#define BLE_UUID16_INIT(uuid16) { .u = { .type = BLE_UUID_TYPE_16 }, .value = (uuid16) }
#define BLE_UUID16(u) ((ble_uuid16_t *)(u))

uint16_t ble_uuid_u16(const ble_uuid_t *uuid);
