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
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "boost_app_ble.h"

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
#define NVS_KEY_SAVED_CNT "saved_cnt"
#define NVS_KEY_SAVED_SSID_PFX "s_ssid_"
#define NVS_KEY_SAVED_PASS_PFX "s_pass_"

#define WIFI_BIT_GOT_IP BIT0
#define WIFI_BIT_FAIL   BIT1
#define WIFI_SCAN_ACTIVE_MIN_MS 40
#define WIFI_SCAN_ACTIVE_MAX_MS 80
/* Backoff before re-attempting a dropped station association. AP and STA share
 * one radio, so an out-of-range SSID reconnects in a tight scan->fail loop that
 * repeatedly yanks the radio off the SoftAP channel; a 10 s backoff gives the
 * AP stable airtime while the saved SSID stays in NVS and is retried quietly. */
#define WIFI_SCAN_RETRY_DELAY_MS 30000

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
static TimerHandle_t s_reconnect_timer;
static uint8_t s_try_index = 0;
static uint8_t s_ap_clients = 0;
static TaskHandle_t s_scan_conn_task;

static esp_err_t apply_sta_config(void);

static void scan_and_connect_task(void *arg)
{
    (void)arg;
    
    /* If a client is connected to SoftAP or we already have an IP, skip scanning */
    if (s_ap_clients > 0 || s_sta_got_ip) {
        ESP_LOGI(TAG, "AP client connected (%d) or STA connected; skipping background scan", s_ap_clients);
        s_scan_conn_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "running background Wi-Fi scan for saved networks");
    
    wifi_scan_config_t scan_cfg = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active.min = WIFI_SCAN_ACTIVE_MIN_MS,
        .scan_time.active.max = WIFI_SCAN_ACTIVE_MAX_MS,
    };
    
    /* Perform blocking scan on the Wi-Fi driver */
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            wifi_ap_record_t *aps = calloc(ap_count, sizeof(wifi_ap_record_t));
            if (aps != NULL) {
                if (esp_wifi_scan_get_ap_records(&ap_count, aps) == ESP_OK) {
                    xSemaphoreTake(s_lock, portMAX_DELAY);
                    int best_match = -1;
                    /* Check saved networks against visible APs */
                    for (uint8_t s = 0; s < s_cfg.saved_count && best_match < 0; ++s) {
                        for (uint16_t a = 0; a < ap_count; ++a) {
                            if (aps[a].ssid[0] != '\0' && strcmp(s_cfg.saved[s].ssid, (char *)aps[a].ssid) == 0) {
                                best_match = s;
                                break;
                            }
                        }
                    }
                    if (best_match >= 0) {
                        ESP_LOGI(TAG, "scan matched saved network: %s", s_cfg.saved[best_match].ssid);
                        s_try_index = (uint8_t)best_match;
                        strlcpy(s_cfg.sta_ssid, s_cfg.saved[best_match].ssid, sizeof(s_cfg.sta_ssid));
                        strlcpy(s_cfg.sta_pass, s_cfg.saved[best_match].pass, sizeof(s_cfg.sta_pass));
                        s_cfg.has_sta_pass = s_cfg.saved[best_match].has_pass;
                        apply_sta_config();
                        xSemaphoreGive(s_lock);
                        esp_wifi_connect();
                    } else {
                        ESP_LOGI(TAG, "scan found no saved networks; will re-scan later");
                        xSemaphoreGive(s_lock);
                    }
                    free(aps);
                } else {
                    free(aps);
                }
            }
        }
    } else {
        ESP_LOGW(TAG, "scan_start failed: %d", err);
    }
    
    s_scan_conn_task = NULL;
    vTaskDelete(NULL);
}

static void reconnect_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    if (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.saved_count > 0 && !s_sta_got_ip && s_ap_clients == 0) {
        if (s_scan_conn_task == NULL) {
            xTaskCreate(scan_and_connect_task, "wifi_scan_conn", 4096, NULL, 3, &s_scan_conn_task);
        }
    }
}

static void defaults_from_secrets(boost_net_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = BOOST_NET_MODE_AP;
    if (BOOST_WIFI_STA_SSID[0] != '\0') {
        strlcpy(cfg->sta_ssid, BOOST_WIFI_STA_SSID, sizeof(cfg->sta_ssid));
        strlcpy(cfg->sta_pass, BOOST_WIFI_STA_PASS, sizeof(cfg->sta_pass));
        cfg->has_sta_pass = cfg->sta_pass[0] != '\0';
        cfg->mode = BOOST_NET_MODE_APSTA;
        cfg->saved_count = 1;
        strlcpy(cfg->saved[0].ssid, cfg->sta_ssid, sizeof(cfg->saved[0].ssid));
        strlcpy(cfg->saved[0].pass, cfg->sta_pass, sizeof(cfg->saved[0].pass));
        cfg->saved[0].has_pass = cfg->has_sta_pass;
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
        err = nvs_set_u8(h, NVS_KEY_SAVED_CNT, s_cfg.saved_count);
    }
    for (uint8_t i = 0; err == ESP_OK && i < BOOST_NET_MAX_SAVED; ++i) {
        char k_ssid[16], k_pass[16];
        snprintf(k_ssid, sizeof(k_ssid), "%s%u", NVS_KEY_SAVED_SSID_PFX, (unsigned)i);
        snprintf(k_pass, sizeof(k_pass), "%s%u", NVS_KEY_SAVED_PASS_PFX, (unsigned)i);
        if (i < s_cfg.saved_count) {
            err = nvs_set_str(h, k_ssid, s_cfg.saved[i].ssid);
            if (err == ESP_OK) {
                err = nvs_set_str(h, k_pass, s_cfg.saved[i].pass);
            }
        } else {
            nvs_erase_key(h, k_ssid);
            nvs_erase_key(h, k_pass);
        }
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

    uint8_t saved_cnt = 0;
    if (nvs_get_u8(h, NVS_KEY_SAVED_CNT, &saved_cnt) == ESP_OK) {
        if (saved_cnt > BOOST_NET_MAX_SAVED) {
            saved_cnt = BOOST_NET_MAX_SAVED;
        }
        s_cfg.saved_count = 0;
        for (uint8_t i = 0; i < saved_cnt; ++i) {
            char k_ssid[16], k_pass[16];
            snprintf(k_ssid, sizeof(k_ssid), "%s%u", NVS_KEY_SAVED_SSID_PFX, (unsigned)i);
            snprintf(k_pass, sizeof(k_pass), "%s%u", NVS_KEY_SAVED_PASS_PFX, (unsigned)i);
            char s_buf[sizeof(s_cfg.saved[0].ssid)] = {0};
            char p_buf[sizeof(s_cfg.saved[0].pass)] = {0};
            size_t s_len = sizeof(s_buf), p_len = sizeof(p_buf);
            if (nvs_get_str(h, k_ssid, s_buf, &s_len) == ESP_OK && s_buf[0] != '\0') {
                nvs_get_str(h, k_pass, p_buf, &p_len);
                strlcpy(s_cfg.saved[s_cfg.saved_count].ssid, s_buf, sizeof(s_cfg.saved[0].ssid));
                strlcpy(s_cfg.saved[s_cfg.saved_count].pass, p_buf, sizeof(s_cfg.saved[0].pass));
                s_cfg.saved[s_cfg.saved_count].has_pass = p_buf[0] != '\0';
                s_cfg.saved_count++;
            }
        }
    } else if (s_cfg.sta_ssid[0] != '\0') {
        /* Migration from single-credential NVS */
        s_cfg.saved_count = 1;
        strlcpy(s_cfg.saved[0].ssid, s_cfg.sta_ssid, sizeof(s_cfg.saved[0].ssid));
        strlcpy(s_cfg.saved[0].pass, s_cfg.sta_pass, sizeof(s_cfg.saved[0].pass));
        s_cfg.saved[0].has_pass = s_cfg.has_sta_pass;
    }

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
    strlcpy((char *)ap.ap.password, BOOST_AP_PASSWORD, sizeof(ap.ap.password));
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
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        ESP_LOGI(TAG, "SoftAP client connected, active clients: %d", s_ap_clients);
        if (s_reconnect_timer != NULL) {
            xTimerStop(s_reconnect_timer, 0);
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
        ESP_LOGI(TAG, "SoftAP client disconnected, active clients: %d", s_ap_clients);
        if (s_ap_clients == 0 && !s_sta_got_ip && s_reconnect_timer != NULL) {
            xTimerReset(s_reconnect_timer, 0);
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        /* If configured with saved networks, do a quiet scan rather than blind connect */
        if (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.saved_count > 0 && s_ap_clients == 0) {
            if (s_scan_conn_task == NULL) {
                xTaskCreate(scan_and_connect_task, "wifi_scan_conn", 4096, NULL, 3, &s_scan_conn_task);
            }
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "STA disconnected reason=%d", ev ? ev->reason : -1);
        s_sta_got_ip = false;
        s_sta_ip[0] = '\0';
        s_rssi = 0;
        if (s_cfg.mode == BOOST_NET_MODE_APSTA && s_cfg.saved_count > 0) {
            if (s_reconnect_timer == NULL) {
                s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(WIFI_SCAN_RETRY_DELAY_MS),
                                                 pdFALSE, NULL, reconnect_timer_cb);
            }
            if (s_reconnect_timer != NULL) {
                xTimerReset(s_reconnect_timer, 0);
            }
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
        if (s_reconnect_timer != NULL) {
            xTimerStop(s_reconnect_timer, 0);
        }
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            s_rssi = ap_info.rssi;
        }
        ESP_LOGI(TAG, "STA got IP %s", s_sta_ip);
        ESP_LOGI(TAG, "BOOST_WEB_IP=%s", s_sta_ip);
        boost_app_ble_set_sta_ip(s_sta_ip);
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
        out->saved_count = s_cfg.saved_count;
        for (uint8_t i = 0; i < s_cfg.saved_count; ++i) {
            strlcpy(out->saved[i].ssid, s_cfg.saved[i].ssid, sizeof(out->saved[i].ssid));
        }
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

    /* Update saved network pool */
    if (s_cfg.sta_ssid[0] != '\0') {
        int found_idx = -1;
        for (uint8_t i = 0; i < s_cfg.saved_count; ++i) {
            if (strcmp(s_cfg.saved[i].ssid, s_cfg.sta_ssid) == 0) {
                found_idx = i;
                break;
            }
        }
        if (found_idx >= 0) {
            /* Update existing entry and move to front (most recently used) */
            boost_saved_entry_t entry = s_cfg.saved[found_idx];
            if (password && !(keep_password && password[0] == '\0')) {
                strlcpy(entry.pass, s_cfg.sta_pass, sizeof(entry.pass));
                entry.has_pass = s_cfg.has_sta_pass;
            }
            for (int i = found_idx; i > 0; --i) {
                s_cfg.saved[i] = s_cfg.saved[i - 1];
            }
            s_cfg.saved[0] = entry;
        } else {
            /* Insert new entry at index 0 */
            if (s_cfg.saved_count < BOOST_NET_MAX_SAVED) {
                s_cfg.saved_count++;
            }
            for (int i = s_cfg.saved_count - 1; i > 0; --i) {
                s_cfg.saved[i] = s_cfg.saved[i - 1];
            }
            strlcpy(s_cfg.saved[0].ssid, s_cfg.sta_ssid, sizeof(s_cfg.saved[0].ssid));
            strlcpy(s_cfg.saved[0].pass, s_cfg.sta_pass, sizeof(s_cfg.saved[0].pass));
            s_cfg.saved[0].has_pass = s_cfg.has_sta_pass;
        }
        s_try_index = 0;
    }

    const bool unchanged =
        before.mode == s_cfg.mode &&
        strcmp(before.sta_ssid, s_cfg.sta_ssid) == 0 &&
        strcmp(before.sta_pass, s_cfg.sta_pass) == 0 &&
        before.saved_count == s_cfg.saved_count;
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

esp_err_t boost_network_delete_saved(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(boost_network_init(), TAG, "init");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int found_idx = -1;
    for (uint8_t i = 0; i < s_cfg.saved_count; ++i) {
        if (strcmp(s_cfg.saved[i].ssid, ssid) == 0) {
            found_idx = i;
            break;
        }
    }
    if (found_idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    for (uint8_t i = found_idx; i + 1 < s_cfg.saved_count; ++i) {
        s_cfg.saved[i] = s_cfg.saved[i + 1];
    }
    s_cfg.saved_count--;
    memset(&s_cfg.saved[s_cfg.saved_count], 0, sizeof(s_cfg.saved[0]));

    if (strcmp(s_cfg.sta_ssid, ssid) == 0) {
        if (s_cfg.saved_count > 0) {
            strlcpy(s_cfg.sta_ssid, s_cfg.saved[0].ssid, sizeof(s_cfg.sta_ssid));
            strlcpy(s_cfg.sta_pass, s_cfg.saved[0].pass, sizeof(s_cfg.sta_pass));
            s_cfg.has_sta_pass = s_cfg.saved[0].has_pass;
        } else {
            s_cfg.sta_ssid[0] = '\0';
            s_cfg.sta_pass[0] = '\0';
            s_cfg.has_sta_pass = false;
            s_cfg.mode = BOOST_NET_MODE_AP;
        }
    }
    s_try_index = 0;
    esp_err_t err = save_locked();
    boost_net_config_t local = s_cfg;
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_started) {
        return ESP_OK;
    }
    const bool want_sta = local.mode == BOOST_NET_MODE_APSTA && local.sta_ssid[0] != '\0';
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
    /* Only disturb the radio if we are not already in a mode that can scan.
     * Re-applying the STA config on a live association forces a disconnect,
     * which is what used to knock the gauge off Wi-Fi after a scan. */
    wifi_mode_t current_mode = WIFI_MODE_NULL;
    const bool mode_known = (esp_wifi_get_mode(&current_mode) == ESP_OK);
    const bool needs_mode_change = !mode_known || current_mode != WIFI_MODE_APSTA;

    if (err == ESP_OK && needs_mode_change) {
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
    ESP_LOGI(TAG, "scan: mode_change=%d err=0x%x", needs_mode_change, err);
    if (err == ESP_OK) {
        uint16_t ap_count = max_records;
        wifi_ap_record_t aps[BOOST_WIFI_SCAN_MAX_RECORDS] = {0};
        err = esp_wifi_scan_get_ap_records(&ap_count, aps);
        if (err == ESP_OK) {
        ESP_LOGI(TAG, "scan: %u APs visible", ap_count);
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

    /* If the mode was never changed, the association is still intact — leave it
     * completely alone. Reapplying config here is what caused the dropout. */
    esp_err_t restore_err = ESP_OK;
    if (needs_mode_change) {
        restore_err = esp_wifi_set_mode(restore_mode);
        if (restore_err == ESP_OK) {
            apply_ap_config();
            if (restore_mode == WIFI_MODE_APSTA) {
                apply_sta_config();
                if (!s_sta_got_ip) {
                    esp_wifi_connect();
                }
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
