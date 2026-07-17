#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP BoostGauge-XXXX at 192.168.4.1. */
esp_err_t boost_wifi_start_ap(void);

/** SSID buffer must be >= 32 bytes. */
void boost_wifi_get_ssid(char *out, size_t out_len);

#ifdef __cplusplus
}
#endif
