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

/* Low-pressure alert threshold default (~32 psi). The ND placard calls for
 * roughly 32-34 psi; the display flags anything clearly below that band.
 * Runtime-configurable through boost_tpms_config_t (Settings > OBD2 BLE). */
#define BOOST_TPMS_LOW_PRESSURE_KPA 220.0f

/* Default staleness window. The BLE OBD poll rotation refreshes each wheel's
 * DID roughly every 4-5 s, so the window must sit comfortably above one
 * rotation or a single missed DID round flips the page amber. */
#define BOOST_TPMS_STALE_AFTER_MS 15000u

typedef struct {
    float kpa;
    float psi;
    uint16_t raw;
    uint16_t did;
    uint32_t age_ms;
    bool valid;
} boost_tpms_wheel_snapshot_t;

/* Canonical snapshot shared by the TPMS service, mock provider and UI. The
 * guard lets display headers include this type without redefining it. */
#ifndef BOOST_TPMS_SNAPSHOT_DEFINED
#define BOOST_TPMS_SNAPSHOT_DEFINED 1
typedef struct {
    uint64_t sequence;
    uint32_t updated_at_ms;
    boost_tpms_status_t status;
    boost_tpms_wheel_snapshot_t wheel[4];
} boost_tpms_snapshot_t;
#endif

typedef struct {
    uint32_t stale_after_ms;
    float replacement_offset_kpa;
    float low_kpa;                 /* red below, green at/above (UI + mirror) */
    bool enabled;
} boost_tpms_config_t;

void boost_tpms_init(void);
void boost_tpms_start(void);
void boost_tpms_get_snapshot(boost_tpms_snapshot_t *out);
void boost_tpms_get_config(boost_tpms_config_t *out);
bool boost_tpms_set_config(const boost_tpms_config_t *config);
/* Transport/provider hooks. publish_raw records a fresh four-wheel sample
 * (FL, FR, RL, RR order) through the conversion path; age recomputes per-wheel
 * validity against the staleness window without touching the raw values. */
void boost_tpms_publish_raw(uint32_t now_ms, const uint16_t raw[4]);
void boost_tpms_age(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
