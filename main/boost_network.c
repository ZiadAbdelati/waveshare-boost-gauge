#include "boost_network.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("boost_wifi_secrets.h")
#include "boost_wifi_secrets.h"
#endif
#ifndef BOOST_WIFI_STA_SSID
#define BOOST_WIFI_STA_SSID ""
#endif
#ifndef BOOST_WIFI_STA_PASS
#define BOOST_WIFI_STA_PASS ""
#endif

#define NVS_NS "boost_wifi"
#define NVS_KEY_MODE "mode"
#define NVS_KEY_SSID "ssid"
#define NVS_KEY_PASS "pass"

#define WIFI_BIT_GOT_IP BIT0
#define WIFI_BIT_FAIL   BIT1
#define WIFI_SCAN_ACTIVE_MIN_MS 40
#define WIFI_SCAN_ACTIVE_MAX_MS 80

static const char *TAG = "boost_net";

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;
static boost_net_config_t s_cfg;
static char s_sta_ip[16];
static char s_ap_ssid[33];
static bool s_sta_got_ip;
static bool s_started;
static int s_rssi = 0;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;

static void defaults_from_secrets(boost_net_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = BOOST_NET_MODE_AP;
    if (BOOST_WIFI_STA_SSID[0] != '\0') {
        strlcpy(cfg->sta_ssid, BOOST_WIFI_STA_SSID, sizeof(cfg->sta_ssid));
        strlcpy(cfg->sta_pass, BOOST_WIFI_STA_PASS, sizeof(cfg->sta_pass));
        cfg->has_sta_pass = cfg->sta_pass[0] != '\0';
        cfg->mode = BOOST_NET_MODE_APSTA;
    }
}

static esp_err_t save_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)s_cfg.mode);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_SSID, s_cfg.sta_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY_PASS, s_cfg.sta_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_config(void)
{
    defaults_from_secrets(&s_cfg);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t mode = (uint8_t)s_cfg.mode;
    if (nvs_get_u8(h, NVS_KEY_MODE, &mode) == ESP_OK) {
        s_cfg.mode = (mode == BOOST_NET_MODE_APSTA) ? BOOST_NET_MODE_APSTA : BOOST_NET_MODE_AP;
    }
    size_t len = sizeof(s_cfg.sta_ssid);
    char ssid[sizeof(s_cfg.sta_ssid)] = {0};
    if (nvs_get_str(h, NVS_KEY_SSID, ssid, &len) == ESP_OK) {
        strlcpy(s_cfg.sta_ssid, ssid, sizeof(s_cfg.sta_ssid));
    }
    len = sizeof(s_cfg.sta_pass);
    char pass[sizeof(s_cfg.sta_pass)] = {0};
    if (nvs_get_str(h, NVS_KEY_PASS, pass, &len) == ESP_OK) {
        strlcpy(s_cfg.sta_pass, pass, sizeof(s_cfg.sta_pass));
    }
    s_cfg.has_sta_pass = s_cfg.sta_pass[0] != '\0';
    /* NVS present with empty SSID forces SoftAP-only even if secrets exist. */
    if (s_cfg.sta_ssid[0] == '\0') {
        s_cfg.mode = BOOST_NET_MODE_AP;
    } else if (s_cfg.mode == BOOST_NET_MODE_APSTA && !s_cfg.has_sta_pass) {
        /* open STA allowed */
    }
    nvs_close(h);
}

static void fill_ap_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "BoostGauge-%02X%02X", mac[4], mac[5]);
}

static esp_err_t apply_ap_config(void)
{
    wifi_config_t ap = {0};
    strlcpy((char *)ap.ap.ssid, s_ap_ssid, sizeof(ap.ap.ssid));
    strlcpy((char *)ap.ap.password, "boost1234", sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_AP, &ap);
}

static esp_err_t apply_sta_config(void)
{
    wifi_config_t sta = {0};
    const size_t ssid_len = strlen(s_cfg.sta_ssid);
    if (ssid_len == 0 || ssid_len >= sizeof(sta.sta.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(sta.sta.ssid, s_cfg.sta_ssid, ssid_len);
    strlcpy((char *)sta.sta.password, s_cfg.sta_pass, sizeof(sta.sta.password));
    sta.sta.threshold.authmode = s_cfg.sta_pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta.sta.pmf_cfg.capable = true;
    sta.sta.pmf_cfg.required = false;
    return esp_wifi_set_config(WIFI_IF_STA, &sta);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.sta_ssid[0]) {
            esp_wifi_connect();
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "STA disconnected reason=%d", ev ? ev->reason : -1);
        s_sta_got_ip = false;
        s_sta_ip[0] = '\0';
        s_rssi = 0;
        if (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.sta_ssid[0]) {
            esp_wifi_connect();
        }
        if (s_events) {
            xEventGroupSetBits(s_events, WIFI_BIT_FAIL);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_sta_got_ip = true;
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }
        ESP_LOGI(TAG, "STA got IP %s", s_sta_ip);
        ESP_LOGI(TAG, "BOOST_WEB_IP=%s", s_sta_ip);
        if (s_events) {
            xEventGroupSetBits(s_events, WIFI_BIT_GOT_IP);
        }
    }
}

esp_err_t boost_network_init(void)
{
    if (s_lock) {
        return ESP_OK;
    }
    s_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "lock");
    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "events");
    load_config();
    fill_ap_ssid();
    ESP_LOGI(TAG, "net config mode=%s ssid_len=%u",
             s_cfg.mode == BOOST_NET_MODE_APSTA ? "apsta" : "ap",
             (unsigned)strlen(s_cfg.sta_ssid));
    return ESP_OK;
}

esp_err_t boost_network_start(uint32_t timeout_ms)
{
    ESP_RETURN_ON_ERROR(boost_network_init(), TAG, "init");
    if (s_started) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");

    s_ap_netif = esp_netif_create_default_wifi_ap();
    const bool want_sta = (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.sta_ssid[0] != '\0');
    if (want_sta) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    /* Live state updates are latency-sensitive; the default MIN_MODEM mode
     * defers STA RX until DTIM and turns 60 Hz polling into ~20 Hz delivery. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "disable modem sleep");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG, "ip handler");

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(want_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(apply_ap_config(), TAG, "ap cfg");
    if (want_sta) {
        ESP_RETURN_ON_ERROR(apply_sta_config(), TAG, "sta cfg");
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    s_started = true;
    ESP_LOGI(TAG, "AP %s password boost1234", s_ap_ssid);

    if (!want_sta) {
        ESP_LOGI(TAG, "SoftAP-only");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "waiting up to %u ms for STA DHCP…", (unsigned)timeout_ms);
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_BIT_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms ? timeout_ms : 1));
    if (bits & WIFI_BIT_GOT_IP) {
        ESP_LOGI(TAG, "LAN dashboard: http://%s/", s_sta_ip);
    } else {
        ESP_LOGW(TAG, "STA not associated yet; SoftAP still available");
    }
    return ESP_OK;
}

void boost_network_get_config(boost_net_config_t *out)
{
    if (!out || !s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cfg;
    xSemaphoreGive(s_lock);
}

void boost_network_get_status(boost_net_status_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        out->mode = s_cfg.mode;
        out->sta_enabled = s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.sta_ssid[0] != '\0';
        strlcpy(out->sta_ssid, s_cfg.sta_ssid, sizeof(out->sta_ssid));
        out->has_sta_pass = s_cfg.has_sta_pass;
        xSemaphoreGive(s_lock);
    }
    out->sta_connected = s_sta_got_ip;
    strlcpy(out->sta_ip, s_sta_ip, sizeof(out->sta_ip));
    strlcpy(out->ap_ssid, s_ap_ssid, sizeof(out->ap_ssid));
    strlcpy(out->ap_ip, "192.168.4.1", sizeof(out->ap_ip));
    out->rssi = s_rssi;
    if (s_sta_got_ip) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            out->rssi = ap_info.rssi;
            s_rssi = ap_info.rssi;
        }
    }
}

esp_err_t boost_network_update(const char *ssid, const char *password, bool keep_password,
                               boost_net_mode_t mode, bool have_mode)
{
    ESP_RETURN_ON_ERROR(boost_network_init(), TAG, "init");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    boost_net_config_t before = s_cfg;
    if (ssid) {
        if (strlen(ssid) >= sizeof(s_cfg.sta_ssid)) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(s_cfg.sta_ssid, ssid, sizeof(s_cfg.sta_ssid));
    }
    if (password && !(keep_password && password[0] == '\0')) {
        if (strlen(password) >= sizeof(s_cfg.sta_pass)) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        strlcpy(s_cfg.sta_pass, password, sizeof(s_cfg.sta_pass));
        s_cfg.has_sta_pass = s_cfg.sta_pass[0] != '\0';
    }
    if (have_mode) {
        s_cfg.mode = mode;
    }
    if (s_cfg.sta_ssid[0] == '\0') {
        s_cfg.mode = BOOST_NET_MODE_AP;
    } else if (!have_mode && s_cfg.mode == BOOST_NET_MODE_AP) {
        /* Setting an SSID while in AP-only upgrades to APSTA. */
        s_cfg.mode = BOOST_NET_MODE_APSTA;
    }
    const bool unchanged =
        before.mode == s_cfg.mode &&
        strcmp(before.sta_ssid, s_cfg.sta_ssid) == 0 &&
        strcmp(before.sta_pass, s_cfg.sta_pass) == 0;
    esp_err_t err = unchanged ? ESP_OK : save_locked();
    boost_net_config_t local = s_cfg;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_started || unchanged) {
        return ESP_OK;
    }

    const bool want_sta = local.mode == BOOST_NET_MODE_APSTA && local.sta_ssid[0] != '\0';
    if (want_sta && s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(want_sta ? WIFI_MODE_APSTA : WIFI_MODE_AP), TAG, "mode");
    ESP_RETURN_ON_ERROR(apply_ap_config(), TAG, "ap");
    if (want_sta) {
        ESP_RETURN_ON_ERROR(apply_sta_config(), TAG, "sta");
        s_sta_got_ip = false;
        s_sta_ip[0] = '\0';
        esp_wifi_disconnect();
        esp_wifi_connect();
    } else {
        s_sta_got_ip = false;
        s_sta_ip[0] = '\0';
        esp_wifi_disconnect();
    }
    return ESP_OK;
}

esp_err_t boost_network_scan(boost_wifi_scan_record_t *records, uint16_t max_records,
                             uint16_t *out_count)
{
    ESP_RETURN_ON_FALSE(records && out_count, ESP_ERR_INVALID_ARG, TAG, "scan args");
    *out_count = 0;
    if (max_records == 0) {
        return ESP_OK;
    }
    if (max_records > BOOST_WIFI_SCAN_MAX_RECORDS) {
        max_records = BOOST_WIFI_SCAN_MAX_RECORDS;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    boost_net_config_t cfg = s_cfg;
    const wifi_mode_t restore_mode =
        (cfg.mode == BOOST_NET_MODE_APSTA && cfg.sta_ssid[0] != '\0') ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    esp_err_t err = ESP_OK;

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            err = ESP_ERR_NO_MEM;
        }
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    if (err == ESP_OK) {
        wifi_scan_config_t scan = {
            .ssid = NULL,
            .bssid = NULL,
            .channel = 0,
            .show_hidden = false,
            .scan_type = WIFI_SCAN_TYPE_ACTIVE,
            .scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS,
            .scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS,
        };
        err = esp_wifi_scan_start(&scan, true);
    }
    if (err == ESP_OK) {
        uint16_t ap_count = max_records;
        wifi_ap_record_t aps[BOOST_WIFI_SCAN_MAX_RECORDS] = {0};
        err = esp_wifi_scan_get_ap_records(&ap_count, aps);
        if (err == ESP_OK) {
            for (uint16_t i = 0; i < ap_count && *out_count < max_records; ++i) {
                if (aps[i].ssid[0] == '\0') {
                    continue;
                }
                boost_wifi_scan_record_t *rec = &records[*out_count];
                memset(rec, 0, sizeof(*rec));
                strlcpy(rec->ssid, (const char *)aps[i].ssid, sizeof(rec->ssid));
                rec->rssi = aps[i].rssi;
                rec->authmode = aps[i].authmode;
                ++(*out_count);
            }
        }
    }

    esp_err_t restore_err = esp_wifi_set_mode(restore_mode);
    if (restore_err == ESP_OK) {
        apply_ap_config();
        if (restore_mode == WIFI_MODE_APSTA) {
            apply_sta_config();
            if (!s_sta_got_ip) {
                esp_wifi_connect();
            }
        }
    }
    xSemaphoreGive(s_lock);
    return err == ESP_OK ? restore_err : err;
}

esp_err_t boost_network_reconnect(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    boost_net_config_t cfg;
    boost_network_get_config(&cfg);
    if (cfg.mode != BOOST_NET_MODE_APSTA || cfg.sta_ssid[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    s_sta_got_ip = false;
    s_sta_ip[0] = '\0';
    esp_wifi_disconnect();
    return esp_wifi_connect();
}
