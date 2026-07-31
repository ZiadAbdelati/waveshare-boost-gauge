#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOST_TPMS_STATUS_NORMAL = 0,
    BOOST_TPMS_STATUS_STALE,
    BOOST_TPMS_STATUS_DISCONNECTED,
} boost_tpms_status_t;

typedef struct {
    float kpa;
    float psi;
    uint16_t raw;
    uint16_t did;
    uint32_t age_ms;
    bool valid;
} boost_tpms_wheel_snapshot_t;

typedef struct {
    uint64_t sequence;
    uint32_t updated_at_ms;
    boost_tpms_status_t status;
    boost_tpms_wheel_snapshot_t wheel[4];
} boost_tpms_snapshot_t;

typedef struct {
    uint32_t stale_after_ms;
    float replacement_offset_kpa;
    bool enabled;
} boost_tpms_config_t;

void boost_tpms_init(void);
void boost_tpms_start(void);
void boost_tpms_get_snapshot(boost_tpms_snapshot_t *out);
void boost_tpms_get_config(boost_tpms_config_t *out);
bool boost_tpms_set_config(const boost_tpms_config_t *config);
void boost_tpms_poll(void);

#ifdef __cplusplus
}
#endif
