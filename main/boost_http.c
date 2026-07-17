#include "boost_http.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/portmacro.h"
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "boost_brightness.h"
#include "boost_config.h"
#include "boost_gauge.h"
#include "boost_ota.h"
#include "boost_sim.h"
#include "boost_wifi.h"
#include "bsp/esp-bsp.h"

static const char *TAG = "boost_http";
static httpd_handle_t s_server;

/* Latest sample for SSE/status — written by gauge task via boost_http_set_sample. */
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

    for (int i = 0; i < 600; i++) { /* ~60s then client reconnects */
        boost_sample_t s = sample_copy();
        char line[192];
        snprintf(line, sizeof(line),
                 "data: {\"psi\":%.2f,\"peak\":%.2f,\"zone\":\"%s\",\"brightness\":%d,\"theme_id\":%u}\n\n",
                 s.psi,
                 s.peak_psi > 0 ? s.peak_psi : 0.0,
                 zone_name(s.psi),
                 boost_brightness_get(),
                 boost_config_get()->theme_id);
        if (httpd_resp_send_chunk(req, line, strlen(line)) != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static bool read_body(httpd_req_t *req, char *buf, size_t buflen)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)buflen) {
        return false;
    }
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) {
            return false;
        }
        got += r;
    }
    buf[got] = '\0';
    return true;
}

static esp_err_t api_brightness_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(j, "toggle"))) {
        boost_brightness_toggle_max_min();
        boost_config_set_brightness((uint8_t)boost_brightness_get());
    } else {
        cJSON *p = cJSON_GetObjectItem(j, "percent");
        if (cJSON_IsNumber(p)) {
            int pct = p->valueint;
            if (pct < 0) {
                pct = 0;
            }
            if (pct > 100) {
                pct = 100;
            }
            boost_config_set_brightness((uint8_t)pct);
        }
    }
    cJSON_Delete(j);
    return send_json(req, status_json());
}

static esp_err_t api_peak_reset_post(httpd_req_t *req)
{
    if (bsp_display_lock(100) == ESP_OK) {
        boost_gauge_reset_peak();
        bsp_display_unlock();
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t api_config_get(httpd_req_t *req)
{
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

static esp_err_t api_config_post(httpd_req_t *req)
{
    char body[512];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    boost_config_t cfg;
    boost_config_get_copy(&cfg);
    cJSON *it;
    if ((it = cJSON_GetObjectItem(j, "brightness")) && cJSON_IsNumber(it)) {
        cfg.brightness = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "theme_id")) && cJSON_IsNumber(it)) {
        cfg.theme_id = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "dim_enable")) && cJSON_IsNumber(it)) {
        cfg.dim_enable = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "dim_start_hour")) && cJSON_IsNumber(it)) {
        cfg.dim_start_hour = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "dim_start_min")) && cJSON_IsNumber(it)) {
        cfg.dim_start_min = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "dim_end_hour")) && cJSON_IsNumber(it)) {
        cfg.dim_end_hour = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "dim_end_min")) && cJSON_IsNumber(it)) {
        cfg.dim_end_min = (uint8_t)it->valueint;
    }
    if ((it = cJSON_GetObjectItem(j, "tz_offset_min")) && cJSON_IsNumber(it)) {
        cfg.tz_offset_min = (int16_t)it->valueint;
    }
    cJSON_Delete(j);

    if (!boost_config_set(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
        return ESP_FAIL;
    }
    if (bsp_display_lock(100) == ESP_OK) {
        boost_gauge_set_theme(cfg.theme_id);
        bsp_display_unlock();
    }
    return send_json(req, status_json());
}

static esp_err_t api_time_post(httpd_req_t *req)
{
    char body[128];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "json");
        return ESP_FAIL;
    }
    cJSON *epoch = cJSON_GetObjectItem(j, "epoch");
    cJSON *tz = cJSON_GetObjectItem(j, "tz_offset_min");
    if (!cJSON_IsNumber(epoch)) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "epoch");
        return ESP_FAIL;
    }
    struct timeval tv = {
        .tv_sec = (time_t)epoch->valuedouble,
        .tv_usec = 0,
    };
    settimeofday(&tv, NULL);

    if (cJSON_IsNumber(tz)) {
        boost_config_t cfg;
        boost_config_get_copy(&cfg);
        cfg.tz_offset_min = (int16_t)tz->valueint;
        boost_config_set(&cfg);
        /* POSIX TZ offset is inverted vs JS getTimezoneOffset semantics we store. */
        char tzbuf[32];
        int off = -cfg.tz_offset_min; /* minutes west of UTC for TZ */
        int h = off / 60;
        int m = abs(off % 60);
        if (m) {
            snprintf(tzbuf, sizeof(tzbuf), "UTC%+d:%02d", h, m);
        } else {
            snprintf(tzbuf, sizeof(tzbuf), "UTC%+d", h);
        }
        setenv("TZ", tzbuf, 1);
        tzset();
    }
    cJSON_Delete(j);
    ESP_LOGI(TAG, "time set to %ld", (long)tv.tv_sec);
    return send_json(req, status_json());
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

static esp_err_t api_theme_post(httpd_req_t *req)
{
    char body[64];
    if (!read_body(req, body, sizeof(body))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body");
        return ESP_FAIL;
    }
    cJSON *j = cJSON_Parse(body);
    cJSON *id = j ? cJSON_GetObjectItem(j, "id") : NULL;
    if (!cJSON_IsNumber(id)) {
        cJSON_Delete(j);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "id");
        return ESP_FAIL;
    }
    uint8_t theme = (uint8_t)id->valueint;
    cJSON_Delete(j);
    if (!boost_config_set_theme(theme)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "theme");
        return ESP_FAIL;
    }
    if (bsp_display_lock(100) == ESP_OK) {
        boost_gauge_set_theme(theme);
        bsp_display_unlock();
    }
    return send_json(req, status_json());
}

static esp_err_t api_gif_post(httpd_req_t *req)
{
    httpd_resp_set_status(req, "501 Not Implemented");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"gif storage not enabled yet\"}");
}

esp_err_t boost_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 30;
    config.send_wait_timeout = 30;
    config.max_uri_handlers = 16;
    config.stack_size = 8192;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get},
        {.uri = "/api/status", .method = HTTP_GET, .handler = api_status_get},
        {.uri = "/api/stream", .method = HTTP_GET, .handler = api_stream_get},
        {.uri = "/api/brightness", .method = HTTP_POST, .handler = api_brightness_post},
        {.uri = "/api/peak/reset", .method = HTTP_POST, .handler = api_peak_reset_post},
        {.uri = "/api/config", .method = HTTP_GET, .handler = api_config_get},
        {.uri = "/api/config", .method = HTTP_POST, .handler = api_config_post},
        {.uri = "/api/time", .method = HTTP_POST, .handler = api_time_post},
        {.uri = "/api/themes", .method = HTTP_GET, .handler = api_themes_get},
        {.uri = "/api/theme", .method = HTTP_POST, .handler = api_theme_post},
        {.uri = "/api/gif", .method = HTTP_POST, .handler = api_gif_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(s_server, &routes[i]);
    }
    boost_ota_register(s_server);

    ESP_LOGI(TAG, "HTTP control UI on http://192.168.4.1/");
    return ESP_OK;
}
