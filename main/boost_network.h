#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOST_NET_MODE_AP = 0,
    BOOST_NET_MODE_APSTA = 1,
} boost_net_mode_t;

#define BOOST_NET_MAX_SAVED 5

typedef struct {
    char ssid[33];
    char pass[65];
    bool has_pass;
} boost_saved_entry_t;

typedef struct {
    boost_net_mode_t mode;
    char sta_ssid[33];
    char sta_pass[65];
    bool has_sta_pass;
    uint8_t saved_count;
    boost_saved_entry_t saved[BOOST_NET_MAX_SAVED];
} boost_net_config_t;

typedef struct {
    char ssid[33];
} boost_saved_network_t;

typedef struct {
    boost_net_mode_t mode;
    bool sta_enabled;
    bool sta_connected;
    char sta_ssid[33];
    char sta_ip[16];
    char ap_ssid[33];
    char ap_ip[16];
    int rssi;
    bool has_sta_pass;
    uint8_t saved_count;
    boost_saved_network_t saved[BOOST_NET_MAX_SAVED];
} boost_net_status_t;

#define BOOST_WIFI_SCAN_MAX_RECORDS 16

/* SoftAP password. Single source of truth - the QR overlay encodes this same
 * literal so the on-screen code always matches the real AP credential. */
#define BOOST_AP_PASSWORD "boost1234"

typedef struct {
    char ssid[33];
    int rssi;
    int authmode;
} boost_wifi_scan_record_t;

/** Init NVS-backed config. Secrets header seeds STA only when NVS empty. */
esp_err_t boost_network_init(void);

/** Bring up SoftAP and optional STA. Blocks up to timeout_ms for first DHCP. */
esp_err_t boost_network_start(uint32_t timeout_ms);

void boost_network_get_config(boost_net_config_t *out);
void boost_network_get_status(boost_net_status_t *out);

/**
 * Update Wi-Fi settings. Empty password with keep_password=true retains stored PSK.
 * Applies mode/SSID immediately (reconnect).
 */
esp_err_t boost_network_update(const char *ssid, const char *password, bool keep_password,
                               boost_net_mode_t mode, bool have_mode);

esp_err_t boost_network_delete_saved(const char *ssid);

esp_err_t boost_network_reconnect(void);

esp_err_t boost_network_scan(boost_wifi_scan_record_t *records, uint16_t max_records,
                             uint16_t *out_count);

#ifdef __cplusplus
}
#endif
