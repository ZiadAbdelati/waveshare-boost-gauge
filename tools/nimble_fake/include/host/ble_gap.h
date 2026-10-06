#pragma once
/* GAP types/APIs. Struct layouts follow the primary source
 * host/include/host/ble_gap.h (only the members the driver touches). */
#include <stdint.h>
#include "host/ble_uuid.h"
#include "host/ble_errors.h"

struct os_mbuf;

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

#define BLE_ADDR_PUBLIC     0x00
#define BLE_ADDR_RANDOM     0x01
#define BLE_ADDR_PUBLIC_ID  0x02
#define BLE_ADDR_RANDOM_ID  0x03

#define BLE_OWN_ADDR_PUBLIC 0x00
#define BLE_OWN_ADDR_RANDOM 0x01

#define BLE_HS_CONN_HANDLE_NONE 0xffff

#define BLE_ERR_SUCCESS            0x00
#define BLE_ERR_REM_USER_CONN_TERM 0x13

/* GAP event types (host/ble_gap.h). */
#define BLE_GAP_EVENT_CONNECT        0
#define BLE_GAP_EVENT_DISCONNECT     1
#define BLE_GAP_EVENT_DISC           7
#define BLE_GAP_EVENT_DISC_COMPLETE  8
#define BLE_GAP_EVENT_NOTIFY_RX      12

struct ble_gap_disc_desc {
    uint8_t       event_type;
    uint8_t       length_data;
    ble_addr_t    addr;
    int8_t        rssi;
    const uint8_t *data;
    ble_addr_t    direct_addr;
};

struct ble_gap_sec_state {
    unsigned encrypted:1;
    unsigned authenticated:1;
    unsigned bonded:1;
    unsigned key_size:5;
};

struct ble_gap_conn_desc {
    struct ble_gap_sec_state sec_state;
    ble_addr_t our_id_addr;
    ble_addr_t peer_id_addr;
    ble_addr_t our_ota_addr;
    ble_addr_t peer_ota_addr;
    uint16_t   conn_handle;
    uint16_t   conn_itvl;
    uint16_t   conn_latency;
    uint16_t   supervision_timeout;
    uint8_t    role;
    uint8_t    master_clock_accuracy;
};

struct ble_gap_conn_params {
    uint16_t scan_itvl;
    uint16_t scan_window;
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
    uint16_t min_ce_len;
    uint16_t max_ce_len;
};

struct ble_gap_disc_params {
    uint16_t itvl;
    uint16_t window;
    uint8_t  filter_policy;
    uint8_t  limited:1;
    uint8_t  passive:1;
    uint8_t  filter_duplicates:1;
    uint8_t  disable_observer_mode:1;
};

struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int      status;
            uint16_t conn_handle;
        } connect;
        struct {
            uint16_t conn_handle;
            uint8_t  reason;
        } disconnect;
        struct ble_gap_disc_desc disc;
        struct {
            int reason;
        } disc_complete;
        struct {
            struct os_mbuf *om;
            uint16_t        attr_handle;
            uint16_t        conn_handle;
            uint8_t         indication:1;
        } notify_rx;
    };
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);

int  ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out_desc);
int  ble_gap_conn_find_by_addr(const ble_addr_t *addr, struct ble_gap_conn_desc *out_desc);
int  ble_gap_conn_active(void);

int  ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                  const struct ble_gap_disc_params *disc_params,
                  ble_gap_event_fn *cb, void *cb_arg);
int  ble_gap_disc_cancel(void);

int  ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                     int32_t duration_ms,
                     const struct ble_gap_conn_params *conn_params,
                     ble_gap_event_fn *cb, void *cb_arg);
int  ble_gap_conn_cancel(void);
int  ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason);
