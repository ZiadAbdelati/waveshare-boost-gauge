#include "boost_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "boost_brightness.h"
#include "boost_config.h"
#include "boost_gauge.h"
#include "boost_ota.h"
#include "boost_sim.h"
#include "boost_wifi.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "boost_http";
static httpd_handle_t s_server;
static boost_sample_t s_sample;
static portMUX_TYPE s_sample_mux = portMUX_INITIALIZER_UNLOCKED;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[] asm("_binary_index_html_end");

void boost_http_set_sample(const boost_sample_t *s)
{
    if (!s) {
        return;
    }
    portENTER_CRITICAL(&s_sample_mux);
    s_sample = *s;
    portEXIT_CRITICAL(&s_sample_mux);
}

static boost_sample_t sample_copy(void)
{
    boost_sample_t s;
    portENTER_CRITICAL(&s_sample_mux);
    s = s_sample;
    portEXIT_CRITICAL(&s_sample_mux);
    return s;
}

static const char *zone_name(float psi)
{
    if (psi >= 18.0f) return "OVER";
    if (psi >= 0.35f) return "BOOST";
    if (psi > -0.35f) return "ATMO";
    return "VAC";
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "json");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, txt);
    free(txt);
    return err;
}

static cJSON *status_json(void)
{
    boost_sample_t s = sample_copy();
    const boost_config_t *cfg = boost_config_get();
    char ssid[32];
    boost_wifi_get_ssid(ssid, sizeof(ssid));

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "version", BOOST_FW_VERSION_STR);
    cJSON_AddNumberToObject(o, "psi", s.psi);
    cJSON_AddNumberToObject(o, "peak", s.peak_psi > 0 ? s.peak_psi : 0);
    cJSON_AddStringToObject(o, "zone", zone_name(s.psi));
    cJSON_AddBoolToObject(o, "demo", s.demo);
    cJSON_AddNumberToObject(o, "brightness", boost_brightness_get());
    cJSON_AddNumberToObject(o, "theme_id", cfg->theme_id);
    cJSON_AddStringToObject(o, "theme", boost_theme_name(cfg->theme_id));
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddNumberToObject(o, "free_heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(esp_timer_get_time() / 1000000ULL));
    cJSON_AddNumberToObject(o, "time_epoch", (double)time(NULL));
    cJSON_AddNumberToObject(o, "tz_offset_min", cfg->tz_offset_min);
    cJSON_AddNumberToObject(o, "ui_ready", boost_gauge_is_ready() ? 1 : 0);
    cJSON_AddNumberToObject(o, "ui_psi", boost_gauge_last_psi());

    cJSON *dim = cJSON_CreateObject();
    cJSON_AddNumberToObject(dim, "enable", cfg->dim_enable);
    cJSON_AddNumberToObject(dim, "start_hour", cfg->dim_start_hour);
    cJSON_AddNumberToObject(dim, "start_min", cfg->dim_start_min);
    cJSON_AddNumberToObject(dim, "end_hour", cfg->dim_end_hour);
    cJSON_AddNumberToObject(dim, "end_min", cfg->dim_end_min);
    cJSON_AddItemToObject(o, "dim", dim);
    return o;
}

static int query_int(httpd_req_t *req, const char *key, int def)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return def;
    }
    char val[32];
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) {
        return def;
    }
    return atoi(val);
}

static bool query_has(httpd_req_t *req, const char *key)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return false;
    }
    char val[8];
    return httpd_query_key_value(q, key, val, sizeof(val)) == ESP_OK;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t api_status(httpd_req_t *req)
{
    return send_json(req, status_json());
}

static esp_err_t api_brightness(httpd_req_t *req)
{
    ESP_LOGI(TAG, "brightness method=%d", (int)req->method);
    if (query_has(req, "toggle")) {
        boost_brightness_toggle_max_min();
        boost_config_set_brightness((uint8_t)boost_brightness_get());
        return send_json(req, status_json());
    }
    int pct = query_int(req, "percent", -1);
    if (pct >= 0) {
        if (pct > 100) pct = 100;
        boost_config_set_brightness((uint8_t)pct);
    }
    return send_json(req, status_json());
}

static esp_err_t api_peak_reset(httpd_req_t *req)
{
    if (bsp_display_lock(200) == ESP_OK) {
        boost_gauge_reset_peak();
        bsp_display_unlock();
    } else {
        boost_sim_reset_peak();
    }
    return send_json(req, status_json());
}

static esp_err_t api_theme(httpd_req_t *req)
{
    int id = query_int(req, "id", -1);
    if (id < 0 || id >= BOOST_THEME_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
        return ESP_FAIL;
    }
    boost_config_set_theme((uint8_t)id);
    if (bsp_display_lock(200) == ESP_OK) {
        boost_gauge_set_theme((uint8_t)id);
        bsp_display_unlock();
    }
    return send_json(req, status_json());
}

static esp_err_t api_time(httpd_req_t *req)
{
    int epoch = query_int(req, "epoch", 0);
    int tz = query_int(req, "tz_offset_min", 0);
    if (epoch <= 100000) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch");
        return ESP_FAIL;
    }
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    settimeofday(&tv, NULL);

    boost_config_t cfg;
    boost_config_get_copy(&cfg);
    cfg.tz_offset_min = (int16_t)tz;
    boost_config_set(&cfg);

    int off = -cfg.tz_offset_min;
    int h = off / 60;
    int m = abs(off % 60);
    char tzbuf[32];
    if (m) snprintf(tzbuf, sizeof(tzbuf), "UTC%+d:%02d", h, m);
    else snprintf(tzbuf, sizeof(tzbuf), "UTC%+d", h);
    setenv("TZ", tzbuf, 1);
    tzset();
    ESP_LOGI(TAG, "time set %d %s", epoch, tzbuf);
    return send_json(req, status_json());
}

static esp_err_t api_config(httpd_req_t *req)
{
    if (query_has(req, "dim_enable") || query_has(req, "dim_start_hour")) {
        boost_config_t cfg;
        boost_config_get_copy(&cfg);
        if (query_has(req, "dim_enable")) cfg.dim_enable = (uint8_t)query_int(req, "dim_enable", 0);
        if (query_has(req, "dim_start_hour")) cfg.dim_start_hour = (uint8_t)query_int(req, "dim_start_hour", 21);
        if (query_has(req, "dim_start_min")) cfg.dim_start_min = (uint8_t)query_int(req, "dim_start_min", 0);
        if (query_has(req, "dim_end_hour")) cfg.dim_end_hour = (uint8_t)query_int(req, "dim_end_hour", 7);
        if (query_has(req, "dim_end_min")) cfg.dim_end_min = (uint8_t)query_int(req, "dim_end_min", 0);
        boost_config_set(&cfg);
        return send_json(req, status_json());
    }
    const boost_config_t *c = boost_config_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "brightness", c->brightness);
    cJSON_AddNumberToObject(o, "theme_id", c->theme_id);
    cJSON_AddNumberToObject(o, "dim_enable", c->dim_enable);
    cJSON_AddNumberToObject(o, "dim_start_hour", c->dim_start_hour);
    cJSON_AddNumberToObject(o, "dim_start_min", c->dim_start_min);
    cJSON_AddNumberToObject(o, "dim_end_hour", c->dim_end_hour);
    cJSON_AddNumberToObject(o, "dim_end_min", c->dim_end_min);
    cJSON_AddNumberToObject(o, "tz_offset_min", c->tz_offset_min);
    cJSON_AddNumberToObject(o, "units", c->units);
    return send_json(req, o);
}

static esp_err_t api_themes(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "themes");
    for (int i = 0; i < BOOST_THEME_COUNT; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id", i);
        cJSON_AddStringToObject(t, "name", boost_theme_name((uint8_t)i));
        cJSON_AddItemToArray(arr, t);
    }
    return send_json(req, o);
}

static esp_err_t api_ping(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"api\":\"v1\"}");
}

static esp_err_t register_any(httpd_handle_t server, const char *uri, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri = uri,
        .method = HTTP_ANY,
        .handler = handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &u);
    ESP_LOGI(TAG, "register %s -> %s", uri, esp_err_to_name(err));
    return err;
}

esp_err_t boost_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 16;
    config.max_open_sockets = 7;
    config.stack_size = 8192;
    /* Keep URI matching simple and exact. */
    config.uri_match_fn = NULL;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    register_any(s_server, "/", root_get);
    register_any(s_server, "/api/ping", api_ping);
    register_any(s_server, "/api/status", api_status);
    register_any(s_server, "/api/brightness", api_brightness);
    register_any(s_server, "/api/peak/reset", api_peak_reset);
    register_any(s_server, "/api/config", api_config);
    register_any(s_server, "/api/time", api_time);
    register_any(s_server, "/api/themes", api_themes);
    register_any(s_server, "/api/theme", api_theme);
    boost_ota_register(s_server);

    ESP_LOGI(TAG, "HTTP ready http://192.168.4.1/");
    return ESP_OK;
}
