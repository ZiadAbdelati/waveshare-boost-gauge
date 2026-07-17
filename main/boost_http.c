#include "boost_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    if (psi >= 18.0f) {
        return "OVER";
    }
    if (psi >= 0.35f) {
        return "BOOST";
    }
    if (psi > -0.35f) {
        return "ATMO";
    }
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

    cJSON *dim = cJSON_CreateObject();
    cJSON_AddNumberToObject(dim, "enable", cfg->dim_enable);
    cJSON_AddNumberToObject(dim, "start_hour", cfg->dim_start_hour);
    cJSON_AddNumberToObject(dim, "start_min", cfg->dim_start_min);
    cJSON_AddNumberToObject(dim, "end_hour", cfg->dim_end_hour);
    cJSON_AddNumberToObject(dim, "end_min", cfg->dim_end_min);
    cJSON_AddItemToObject(o, "dim", dim);
    return o;
}

static esp_err_t root_get(httpd_req_t *req)
{
    const size_t len = (size_t)(index_html_end - index_html_start);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html_start, len);
}

static esp_err_t api_status_get(httpd_req_t *req)
{
    return send_json(req, status_json());
}

static esp_err_t api_stream_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");

    for (int i = 0; i < 50; i++) {
        boost_sample_t s = sample_copy();
        char line[192];
        snprintf(line, sizeof(line),
                 "data: {\"psi\":%.2f,\"peak\":%.2f,\"zone\":\"%s\",\"brightness\":%d,\"theme_id\":%u}\n\n",
                 (double)s.psi,
                 (double)(s.peak_psi > 0 ? s.peak_psi : 0.0f),
                 zone_name(s.psi),
                 boost_brightness_get(),
                 boost_config_get()->theme_id);
        if (httpd_resp_send_chunk(req, line, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static int query_int(httpd_req_t *req, const char *key, int def)
{
    char q[128];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return def;
    }
    char val[32];
    if (httpd_query_key_value(q, key, val, sizeof(val)) != ESP_OK) {
        return def;
    }
    return atoi(val);
}

static esp_err_t api_brightness_get(httpd_req_t *req)
{
    char q[128] = {0};
    httpd_req_get_url_query_str(req, q, sizeof(q));
    ESP_LOGI(TAG, "brightness query: %s", q);

    char tog[8] = {0};
    if (httpd_query_key_value(q, "toggle", tog, sizeof(tog)) == ESP_OK &&
        (tog[0] == '1' || tog[0] == 't' || tog[0] == 'T')) {
        boost_brightness_toggle_max_min();
        boost_config_set_brightness((uint8_t)boost_brightness_get());
        return send_json(req, status_json());
    }

    int pct = query_int(req, "percent", -1);
    if (pct >= 0) {
        if (pct > 100) {
            pct = 100;
        }
        boost_config_set_brightness((uint8_t)pct);
    }
    return send_json(req, status_json());
}

static esp_err_t api_peak_reset_get(httpd_req_t *req)
{
    boost_gauge_reset_peak();
    return send_json(req, status_json());
}

static esp_err_t api_theme_get(httpd_req_t *req)
{
    int id = query_int(req, "id", -1);
    if (id < 0 || id >= BOOST_THEME_COUNT) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
        return ESP_FAIL;
    }
    if (!boost_config_set_theme((uint8_t)id)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "theme");
        return ESP_FAIL;
    }
    boost_gauge_set_theme((uint8_t)id);
    return send_json(req, status_json());
}

static esp_err_t api_time_get(httpd_req_t *req)
{
    int epoch = query_int(req, "epoch", 0);
    int tz = query_int(req, "tz_offset_min", 0);
    if (epoch <= 0) {
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
    if (m) {
        snprintf(tzbuf, sizeof(tzbuf), "UTC%+d:%02d", h, m);
    } else {
        snprintf(tzbuf, sizeof(tzbuf), "UTC%+d", h);
    }
    setenv("TZ", tzbuf, 1);
    tzset();
    ESP_LOGI(TAG, "time set epoch=%d tz=%s", epoch, tzbuf);
    return send_json(req, status_json());
}

static esp_err_t api_config_get(httpd_req_t *req)
{
    /* Optional query updates for dim schedule (GET-friendly). */
    char q[160] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && q[0]) {
        boost_config_t cfg;
        boost_config_get_copy(&cfg);
        char val[16];
        if (httpd_query_key_value(q, "dim_enable", val, sizeof(val)) == ESP_OK) {
            cfg.dim_enable = (uint8_t)atoi(val);
        }
        if (httpd_query_key_value(q, "dim_start_hour", val, sizeof(val)) == ESP_OK) {
            cfg.dim_start_hour = (uint8_t)atoi(val);
        }
        if (httpd_query_key_value(q, "dim_start_min", val, sizeof(val)) == ESP_OK) {
            cfg.dim_start_min = (uint8_t)atoi(val);
        }
        if (httpd_query_key_value(q, "dim_end_hour", val, sizeof(val)) == ESP_OK) {
            cfg.dim_end_hour = (uint8_t)atoi(val);
        }
        if (httpd_query_key_value(q, "dim_end_min", val, sizeof(val)) == ESP_OK) {
            cfg.dim_end_min = (uint8_t)atoi(val);
        }
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

static esp_err_t api_themes_get(httpd_req_t *req)
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

static esp_err_t api_gif_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "501 Not Implemented");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"gif storage not enabled yet\"}");
}

static esp_err_t register_uri(httpd_handle_t server, const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    esp_err_t err = httpd_register_uri_handler(server, &u);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "registered %s method=%d", uri, (int)method);
    }
    return err;
}

esp_err_t boost_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.max_uri_handlers = 20;
    config.max_open_sockets = 7;
    config.stack_size = 10240;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    /* Prefer GET for controls — avoids 405 from picky mobile stacks / proxies. */
    register_uri(s_server, "/", HTTP_GET, root_get);
    register_uri(s_server, "/api/status", HTTP_GET, api_status_get);
    register_uri(s_server, "/api/stream", HTTP_GET, api_stream_get);
    register_uri(s_server, "/api/brightness", HTTP_GET, api_brightness_get);
    register_uri(s_server, "/api/peak/reset", HTTP_GET, api_peak_reset_get);
    register_uri(s_server, "/api/config", HTTP_GET, api_config_get);
    register_uri(s_server, "/api/time", HTTP_GET, api_time_get);
    register_uri(s_server, "/api/themes", HTTP_GET, api_themes_get);
    register_uri(s_server, "/api/theme", HTTP_GET, api_theme_get);
    register_uri(s_server, "/api/gif", HTTP_GET, api_gif_get);
    boost_ota_register(s_server);

    ESP_LOGI(TAG, "HTTP control UI on http://192.168.4.1/");
    return ESP_OK;
}
