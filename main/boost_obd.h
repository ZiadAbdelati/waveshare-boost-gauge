#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * OBD-II application driver: glues the BLE central transport + ELM327 framing
 * to the gauge. Owns the poll task, the mode-01 PID table and the Mazda MX-5 ND
 * TPMS DID reads that feed boost_tpms. Runtime state is published to the web
 * model for the dashboard. Entire module is no-op when BLE is not compiled in.
 */

#define BOOST_OBD_PEER_MAX 32

typedef struct {
    /* 0 disabled, 1 scanning, 2 connecting, 3 ready, 4 error */
    int state;
    uint16_t last_error;           /* NimBLE status of the last failed link step */
    char peer[24];                /* advertised name (lowercased), "" if none */
    char peer_addr[BOOST_OBD_PEER_MAX];
    uint32_t uptime_ms;           /* time the current link has been usable */
    /* Latest successful readings; valid=false (and zeros) until first read. */
    bool valid;
    uint32_t age_ms;              /* age of the newest successful reading */
    float rpm;
    float speed_kph;
    float coolant_c;
    float map_kpa;
    float iat_c;
    float throttle_pct;
    float maf_gps;
    float fuel_pct;
    float battery_v;
} boost_obd_state_t;

/* Create the poll task (idle until enabled). Call once from app_main after the
 * web control plane is up so a BLE init failure cannot precede OTA recovery. */
void boost_obd_init(void);

/* Runtime enable/disable. Enable starts the BLE central; disable tears the link
 * down and returns the module to idle. */
void boost_obd_set_enabled(bool enabled);

bool boost_obd_enabled(void);

/* Thread-safe copy of the current state. */
void boost_obd_get_state(boost_obd_state_t *out);

#ifdef __cplusplus
}
#endif
