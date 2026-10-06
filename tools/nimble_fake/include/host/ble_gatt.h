#pragma once
/* GATT client types/APIs touched by the driver. */
#include <stdint.h>
#include "host/ble_uuid.h"
#include "host/ble_errors.h"

struct os_mbuf;

#define BLE_GATT_CHR_PROP_BROADCAST    0x01
#define BLE_GATT_CHR_PROP_READ         0x02
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 0x04
#define BLE_GATT_CHR_PROP_WRITE        0x08
#define BLE_GATT_CHR_PROP_NOTIFY       0x10
#define BLE_GATT_CHR_PROP_INDICATE     0x20

struct ble_gatt_error {
    uint16_t status;
    uint16_t att_handle;
};

struct ble_gatt_svc {
    uint16_t      start_handle;
    uint16_t      end_handle;
    ble_uuid_any_t uuid;
};

struct ble_gatt_chr {
    uint16_t      def_handle;
    uint16_t      val_handle;
    uint8_t       properties;
    ble_uuid_any_t uuid;
};

struct ble_gatt_dsc {
    uint16_t      handle;
    ble_uuid_any_t uuid;
};

struct ble_gatt_attr {
    uint16_t        handle;
    uint16_t        offset;
    struct os_mbuf *om;
};

typedef int ble_gatt_disc_svc_fn(uint16_t conn_handle,
                                 const struct ble_gatt_error *error,
                                 const struct ble_gatt_svc *service,
                                 void *arg);

typedef int ble_gatt_chr_fn(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr,
                            void *arg);

typedef int ble_gatt_dsc_fn(uint16_t conn_handle,
                            const struct ble_gatt_error *error,
                            uint16_t chr_val_handle,
                            const struct ble_gatt_dsc *dsc,
                            void *arg);

typedef int ble_gatt_attr_fn(uint16_t conn_handle,
                             const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr,
                             void *arg);

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_disc_svc_fn *cb, void *cb_arg);
int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, ble_gatt_chr_fn *cb, void *cb_arg);
int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, ble_gatt_dsc_fn *cb, void *cb_arg);
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle,
                         const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg);
