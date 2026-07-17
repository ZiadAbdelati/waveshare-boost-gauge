#include "boost_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "boost_config.h"

static const char *TAG = "boost_wifi";
static char s_ssid[32];

void boost_wifi_get_ssid(char *out, size_t out_len)
{
    if (!out || out_len == 0) {
        return;
    }
    snprintf(out, out_len, "%s", s_ssid[0] ? s_ssid : "BoostGauge");
}

esp_err_t boost_wifi_start_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "BoostGauge-%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, s_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(s_ssid);
    wifi_config.ap.channel = 1;
    wifi_config.ap.max_connection = 4;
    wifi_config.ap.beacon_interval = 100;

    const boost_config_t *bc = boost_config_get();
    if (bc->ap_pass[0] != '\0' && strlen(bc->ap_pass) >= 8) {
        strncpy((char *)wifi_config.ap.password, bc->ap_pass, sizeof(wifi_config.ap.password) - 1);
        wifi_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
        wifi_config.ap.password[0] = '\0';
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP SSID=%s IP=192.168.4.1 auth=%s",
             s_ssid,
             wifi_config.ap.authmode == WIFI_AUTH_OPEN ? "open" : "wpa2");
    return ESP_OK;
}
