#pragma once
/* Advertising-data field view used by the driver's adv_matches(). */
#include <stdint.h>
#include "host/ble_uuid.h"

struct ble_hs_adv_fields {
    uint8_t             flags;
    const ble_uuid16_t *uuids16;
    uint8_t             num_uuids16;
    const uint8_t      *name;
    uint8_t             name_len;
    const uint8_t      *svc_data_uuid16;
    uint8_t             svc_data_uuid16_len;
    int8_t              tx_pwr_lvl;
};

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields,
                            const uint8_t *data, uint8_t length);
