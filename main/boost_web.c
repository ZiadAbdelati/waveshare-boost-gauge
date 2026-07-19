#include "boost_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>


#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"

#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"

#include "boost_model.h"
#include "boost_gauge.h"
#include "boost_network.h"
#include "boost_media_store.h"
#include "generated_web_assets.h"

#define API_BASE "/api/v1"
#define WS_STATE_PATH API_BASE "/state/ws"

#define HTTP_CHUNK 16384
#define MAX_JSON_BODY 4096
#define MAX_GIF_BYTES BOOST_MEDIA_STORE_MAX_BYTES
#define MAX_GIF_DIMENSION BOOST_MEDIA_STORE_MAX_DIMENSION


static const char *TAG = "boost_web";
static httpd_handle_t s_httpd;
static int s_state_ws_fd = -1;
static TaskHandle_t s_state_ws_task;
static volatile bool s_media_upload_in_progress;
static portMUX_TYPE s_web_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_state_ws_inflight;
static void *s_state_ws_payload;


/* State is protected because telemetry task and HTTPD callbacks share it. */

static esp_err_t set_common_headers(httpd_req_t *req, const char *type)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,PUT,POST,DELETE,OPTIONS");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    set_common_headers(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", msg);
    return send_json(req, body);
}

static char *read_body(httpd_req_t *req, size_t max_len)
{
    if (req->content_len == 0) {
        return calloc(1, 1);
    }
    if (req->content_len > max_len) {
        return NULL;
    }
    char *buf = calloc(1, req->content_len + 1);
    if (buf == NULL) {
        return NULL;
    }
    size_t off = 0;
    while (off < req->content_len) {
        int n = httpd_req_recv(req, buf + off, req->content_len - off);
        if (n <= 0) {
            free(buf);
            return NULL;
        }
        off += (size_t)n;
    }
    buf[off] = '\0';
    return buf;
}

static void append_theme_json(char *buf, size_t len, const boost_theme_t *theme)
{
    char tmp[384];
    snprintf(tmp, sizeof(tmp),
             "{\"id\":\"%s\",\"name\":\"%s\",\"colors\":{\"face\":\"#%06lx\","
             "\"track\":\"#%06lx\",\"text\":\"#%06lx\",\"muted\":\"#%06lx\","
             "\"vacuum\":\"#%06lx\",\"boost\":\"#%06lx\",\"overboost\":\"#%06lx\","
             "\"zero\":\"#%06lx\"},\"brightnessHigh\":%d,\"brightnessLow\":%d}",
             theme->id, theme->name, (unsigned long)theme->face, (unsigned long)theme->track,
             (unsigned long)theme->text, (unsigned long)theme->muted,
             (unsigned long)theme->vacuum, (unsigned long)theme->boost,
             (unsigned long)theme->overboost, (unsigned long)theme->zero,
             theme->brightness_high, theme->brightness_low);
    strlcat(buf, tmp, len);
}

static int state_json(char *json, size_t len)
{
    boost_state_t st;
    boost_model_get_state(&st);
    return snprintf(json, len,
                    "{\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
                    "\"brightness\":%d,\"firmwareVersion\":\"%s\",\"uptimeMs\":%llu,"
                    "\"epochMs\":%lld,\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\","
                    "\"display\":{\"renderFps\":%lu,\"flushesPerSecond\":%lu,\"pixelsPerSecond\":%lu}}",
                    (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
                    st.brightness, st.firmware_version, (unsigned long long)st.uptime_ms,
                    (long long)st.epoch_ms, st.timezone_offset_minutes, st.active_theme_id,
                    (unsigned long)st.display.render_fps, (unsigned long)st.display.flushes_per_second,
                    (unsigned long)st.display.pixels_per_second);
}

static esp_err_t state_get(httpd_req_t *req)
{
    char json[512];
    const int n = state_json(json, sizeof(json));
    return n > 0 && n < (int)sizeof(json) ? send_json(req, json) : ESP_FAIL;
}

static void state_ws_send_done(esp_err_t err, int socket, void *arg)
{
    portENTER_CRITICAL(&s_web_lock);
    if (s_state_ws_payload == arg) {
        s_state_ws_payload = NULL;
        s_state_ws_inflight = false;
    }
    if (err != ESP_OK && s_state_ws_fd == socket) {
        s_state_ws_fd = -1;
    }
    portEXIT_CRITICAL(&s_web_lock);
    free(arg);
}

static void state_ws_push(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_web_lock);
    const int fd = s_state_ws_fd;
    const bool allowed = s_httpd != NULL && fd >= 0 && !s_state_ws_inflight;
    if (allowed) {
        s_state_ws_inflight = true;
    }
    portEXIT_CRITICAL(&s_web_lock);
    if (!allowed) {
        return;
    }

    char *json = malloc(512);
    if (json == NULL) {
        portENTER_CRITICAL(&s_web_lock);
        s_state_ws_inflight = false;
        portEXIT_CRITICAL(&s_web_lock);
        return;
    }
    const int n = state_json(json, 512);
    if (n <= 0 || n >= 512) {
        free(json);
        portENTER_CRITICAL(&s_web_lock);
        s_state_ws_inflight = false;
        portEXIT_CRITICAL(&s_web_lock);
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len = (size_t)n,
    };
    portENTER_CRITICAL(&s_web_lock);
    s_state_ws_payload = json;
    portEXIT_CRITICAL(&s_web_lock);
    const esp_err_t err = httpd_ws_send_data_async(s_httpd, fd, &frame, state_ws_send_done, json);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_web_lock);
        if (s_state_ws_payload == json) {
            s_state_ws_payload = NULL;
            s_state_ws_inflight = false;
        }
        if (s_state_ws_fd == fd) {
            s_state_ws_fd = -1;
        }
        portEXIT_CRITICAL(&s_web_lock);
        free(json);
    }
}

static void state_ws_task(void *arg)
{
    (void)arg;
    while (true) {
        portENTER_CRITICAL(&s_web_lock);
        const bool ready = !s_media_upload_in_progress && s_state_ws_fd >= 0 &&
                           s_httpd != NULL && !s_state_ws_inflight;
        portEXIT_CRITICAL(&s_web_lock);
        if (ready) {
            state_ws_push(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t state_ws_get(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        portENTER_CRITICAL(&s_web_lock);
        const int old_fd = s_state_ws_fd;
        s_state_ws_fd = fd;
        portEXIT_CRITICAL(&s_web_lock);
        if (old_fd >= 0 && old_fd != fd) {
            (void)httpd_sess_trigger_close(s_httpd, old_fd);
        }
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        frame.len = 0;
        frame.payload = NULL;
        err = httpd_ws_send_frame(req, &frame);
    } else if (frame.type == HTTPD_WS_TYPE_PING) {
        frame.type = HTTPD_WS_TYPE_PONG;
        frame.len = 0;
        frame.payload = NULL;
        err = httpd_ws_send_frame(req, &frame);
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE || err != ESP_OK) {
        portENTER_CRITICAL(&s_web_lock);
        if (s_state_ws_fd == fd) {
            s_state_ws_fd = -1;
        }
        portEXIT_CRITICAL(&s_web_lock);
    }
    return err;
}



static void config_json(char *json, size_t len)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    snprintf(json, len,
             "{\"brightnessHigh\":%d,\"brightnessLow\":%d,"
             "\"dimSchedule\":{\"enabled\":%s,\"startMinutes\":%d,\"endMinutes\":%d},"
             "\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\"}",
             cfg.brightness_high, cfg.brightness_low,
             cfg.dim_schedule.enabled ? "true" : "false",
             cfg.dim_schedule.start_minutes, cfg.dim_schedule.end_minutes,
             cfg.timezone_offset_minutes, cfg.active_theme_id);
}

static esp_err_t config_get(httpd_req_t *req)
{
    char json[256];
    config_json(json, sizeof(json));
    return send_json(req, json);
}

static bool json_int(cJSON *obj, const char *name, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valueint;
    return true;
}

static esp_err_t config_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_json");
    }
    boost_config_t patch;
    boost_model_get_config(&patch);
    uint32_t fields = 0;
    int tmp;
    if (json_int(root, "brightnessHigh", &tmp)) {
        patch.brightness_high = tmp;
        fields |= BOOST_CONFIG_BRIGHTNESS_HIGH;
    }
    if (json_int(root, "brightnessLow", &tmp)) {
        patch.brightness_low = tmp;
        fields |= BOOST_CONFIG_BRIGHTNESS_LOW;
    }
    if (json_int(root, "timezoneOffsetMinutes", &tmp)) {
        patch.timezone_offset_minutes = tmp;
        fields |= BOOST_CONFIG_TZ_OFFSET;
    }
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "activeThemeId");
    if (cJSON_IsString(id)) {
        strlcpy(patch.active_theme_id, id->valuestring, sizeof(patch.active_theme_id));
        fields |= BOOST_CONFIG_THEME;
    }
    cJSON *sched = cJSON_GetObjectItemCaseSensitive(root, "dimSchedule");
    if (cJSON_IsObject(sched)) {
        cJSON *enabled = cJSON_GetObjectItemCaseSensitive(sched, "enabled");
        if (cJSON_IsBool(enabled)) {
            patch.dim_schedule.enabled = cJSON_IsTrue(enabled);
            fields |= BOOST_CONFIG_DIM_ENABLED;
        }
        if (json_int(sched, "startMinutes", &tmp)) {
            patch.dim_schedule.start_minutes = tmp;
            fields |= BOOST_CONFIG_DIM_START;
        }
        if (json_int(sched, "endMinutes", &tmp)) {
            patch.dim_schedule.end_minutes = tmp;
            fields |= BOOST_CONFIG_DIM_END;
        }
    }
    cJSON_Delete(root);
    esp_err_t err = boost_model_update_config(&patch, fields);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "invalid_config");
    }
    char json[256];
    config_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t time_post(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *epoch = cJSON_GetObjectItemCaseSensitive(root, "epochMs");
    cJSON *tz = cJSON_GetObjectItemCaseSensitive(root, "timezoneOffsetMinutes");
    if (!cJSON_IsNumber(epoch) || !cJSON_IsNumber(tz)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_time");
    }
    esp_err_t err = boost_model_set_time((int64_t)epoch->valuedouble, tz->valueint);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "time_not_set");
    }
    boost_model_refresh_status();
    return state_get(req);

}

static esp_err_t themes_get(httpd_req_t *req)
{
    char json[2048] = {0};
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    snprintf(json, sizeof(json), "{\"activeThemeId\":\"%s\",\"themes\":[", cfg.active_theme_id);
    for (size_t i = 0; i < boost_theme_count(); ++i) {
        if (i > 0) {
            strlcat(json, ",", sizeof(json));
        }
        append_theme_json(json, sizeof(json), boost_theme_at(i));
    }
    strlcat(json, "]}", sizeof(json));
    return send_json(req, json);
}

static esp_err_t theme_active_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!cJSON_IsString(id)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_theme");
    }
    esp_err_t err = boost_model_set_active_theme(id->valuestring);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_404, "theme_not_found");
    }
    return themes_get(req);
}

static esp_err_t logs_get(httpd_req_t *req)
{
    size_t limit = 300;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[16];
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) {
            int parsed = atoi(val);
            if (parsed > 0) {
                limit = (size_t)parsed;
            }
        }
    }
    if (limit > BOOST_LOG_CAPACITY) {
        limit = BOOST_LOG_CAPACITY;
    }
    boost_log_sample_t *logs = calloc(limit, sizeof(*logs));
    if (logs == NULL) {
        return send_err(req, HTTPD_500, "no_mem");
    }
    size_t n = boost_model_copy_logs(logs, limit);
    set_common_headers(req, "application/json");
    httpd_resp_sendstr_chunk(req, "{\"samples\":[");
    char row[128];
    for (size_t i = 0; i < n; ++i) {
        snprintf(row, sizeof(row), "%s{\"tMs\":%lu,\"psi\":%.2f,\"peakPsi\":%.2f,"
                 "\"zone\":\"%s\",\"demo\":%s}",
                 i ? "," : "", (unsigned long)logs[i].t_ms, (double)logs[i].psi,
                 (double)logs[i].peak_psi, logs[i].zone, logs[i].demo ? "true" : "false");
        httpd_resp_sendstr_chunk(req, row);
    }
    free(logs);
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t logs_csv_get(httpd_req_t *req)
{
    boost_log_sample_t *logs = calloc(BOOST_LOG_CAPACITY, sizeof(*logs));
    if (logs == NULL) {
        return send_err(req, HTTPD_500, "no_mem");
    }
    const size_t n = boost_model_copy_logs(logs, BOOST_LOG_CAPACITY);
    boost_state_t current;
    boost_model_get_state(&current);
    set_common_headers(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"boost-gauge-log.csv\"");
    httpd_resp_sendstr_chunk(req, "timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo\n");
    char row[144];
    for (size_t i = 0; i < n; ++i) {
        const int64_t epoch_ms = current.epoch_ms > 0
            ? current.epoch_ms - ((int64_t)current.uptime_ms - (int64_t)logs[i].t_ms)
            : 0;
        char timestamp[40] = "";
        if (epoch_ms > 0) {
            const time_t seconds = (time_t)(epoch_ms / 1000LL) + current.timezone_offset_minutes * 60;
            struct tm local = {0};
            if (gmtime_r(&seconds, &local) != NULL) {
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &local);
                const int offset = current.timezone_offset_minutes;
                const int abs_offset = offset < 0 ? -offset : offset;
                snprintf(timestamp + strlen(timestamp), sizeof(timestamp) - strlen(timestamp),
                         "%c%02d:%02d", offset < 0 ? '-' : '+', abs_offset / 60, abs_offset % 60);
            }
        }
        snprintf(row, sizeof(row), "%s,%d,%lld,%lu,%.2f,%.2f,%s,%d\n",
                 timestamp, current.timezone_offset_minutes, (long long)epoch_ms,
                 (unsigned long)logs[i].t_ms, (double)logs[i].psi, (double)logs[i].peak_psi,
                 logs[i].zone, logs[i].demo ? 1 : 0);
        httpd_resp_sendstr_chunk(req, row);
    }
    free(logs);
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t logs_delete(httpd_req_t *req)
{
    boost_model_clear_logs();
    return send_json(req, "{\"ok\":true}");
}

static esp_err_t media_status_get(httpd_req_t *req)
{
    boost_media_store_status_t st = {0};
    if (boost_media_store_status(&st) != ESP_OK) return send_err(req, HTTPD_500, "media_status");
    char json[192];
    snprintf(json, sizeof(json),
             "{\"present\":%s,\"name\":\"active.gif\",\"size\":%u,\"uploadedAtMs\":%lu,"
             "\"playbackSupported\":%s,\"playback\":\"%s\"}",
             st.present ? "true" : "false", (unsigned)st.size,
             (unsigned long)st.uploaded_at_ms, st.present ? "true" : "false",
             st.present ? "active" : "unavailable");
    return send_json(req, json);
}

static esp_err_t media_delete(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_web_lock);
    const bool busy = s_media_upload_in_progress;
    portEXIT_CRITICAL(&s_web_lock);
    if (busy) return send_err(req, "409 Conflict", "media_upload_in_progress");
    boost_gauge_media_delete();
    if (boost_media_store_delete() != ESP_OK) return send_err(req, HTTPD_500, "media_delete");
    return media_status_get(req);
}

static esp_err_t media_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_GIF_BYTES) return send_err(req, HTTPD_400, "gif_size");
    portENTER_CRITICAL(&s_web_lock);
    if (s_media_upload_in_progress) {
        portEXIT_CRITICAL(&s_web_lock);
        return send_err(req, "409 Conflict", "media_upload_in_progress");
    }
    s_media_upload_in_progress = true;
    portEXIT_CRITICAL(&s_web_lock);

    esp_err_t result = ESP_OK;
    const char *error_status = HTTPD_500;
    const char *error_msg = "media_begin";
    char *buf = NULL;
    bool begun = false;
    size_t received = 0;
    uint8_t header[10] = {0};
    bool header_ok = false;

    boost_gauge_media_delete();
    result = boost_media_store_begin(req->content_len);
    if (result != ESP_OK) goto cleanup;
    begun = true;
    buf = malloc(HTTP_CHUNK);
    if (buf == NULL) {
        error_msg = "no_mem";
        goto cleanup;
    }
    while (received < req->content_len) {
        size_t want = req->content_len - received;
        if (want > HTTP_CHUNK) want = HTTP_CHUNK;
        const int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            result = ESP_FAIL;
            error_status = HTTPD_400;
            error_msg = "upload_failed";
            goto cleanup;
        }
        if (received < sizeof(header)) {
            size_t copy = sizeof(header) - received;
            if (copy > (size_t)n) copy = (size_t)n;
            memcpy(header + received, buf, copy);
            if (received + copy == sizeof(header)) {
                const unsigned width = (unsigned)header[6] | ((unsigned)header[7] << 8);
                const unsigned height = (unsigned)header[8] | ((unsigned)header[9] << 8);
                header_ok = (memcmp(header, "GIF87a", 6) == 0 || memcmp(header, "GIF89a", 6) == 0) &&
                            width > 0 && height > 0 && width <= MAX_GIF_DIMENSION && height <= MAX_GIF_DIMENSION;
                if (!header_ok) {
                    result = ESP_ERR_INVALID_ARG;
                    error_status = HTTPD_400;
                    error_msg = "gif_dimensions";
                    goto cleanup;
                }
            }
        }
        result = boost_media_store_write(buf, (size_t)n);
        if (result != ESP_OK) {
            error_msg = "media_write";
            goto cleanup;
        }
        received += (size_t)n;
    }
    if (!header_ok) {
        result = ESP_ERR_INVALID_ARG;
        error_status = HTTPD_400;
        error_msg = "not_gif";
        goto cleanup;
    }
    result = boost_media_store_commit();
    if (result != ESP_OK) {
        error_msg = "media_commit";
        goto cleanup;
    }
    begun = false;
    if (!boost_gauge_media_load()) {
        result = ESP_FAIL;
        error_msg = "media_load";
        goto cleanup;
    }

cleanup:
    free(buf);
    if (begun) boost_media_store_abort();
    if (result != ESP_OK) {
        boost_media_store_status_t old = {0};
        if (boost_media_store_status(&old) == ESP_OK && old.present) (void)boost_gauge_media_load();
        portENTER_CRITICAL(&s_web_lock);
        s_media_upload_in_progress = false;
        portEXIT_CRITICAL(&s_web_lock);
        return send_err(req, error_status, error_msg);
    }
    portENTER_CRITICAL(&s_web_lock);
    s_media_upload_in_progress = false;
    portEXIT_CRITICAL(&s_web_lock);
    return media_status_get(req);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL || req->content_len == 0) {
        return send_err(req, HTTPD_400, "ota_unavailable");
    }
    ESP_LOGI(TAG, "OTA receiving %u bytes", (unsigned)req->content_len);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &ota);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500, "ota_begin");
    }
    char *buf = malloc(HTTP_CHUNK);
    if (buf == NULL) {
        esp_ota_abort(ota);
        return send_err(req, HTTPD_500, "no_mem");
    }
    size_t received = 0;
    while (received < req->content_len) {
        size_t want = req->content_len - received;
        if (want > HTTP_CHUNK) {
            want = HTTP_CHUNK;
        }
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            free(buf);
            esp_ota_abort(ota);
            return send_err(req, HTTPD_400, "ota_recv");
        }
        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(ota);
            return send_err(req, HTTPD_400, "ota_write");
        }
        received += (size_t)n;
    }
    free(buf);
    ESP_LOGI(TAG, "OTA wrote %u bytes; validating image", (unsigned)received);

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "ota_invalid");
    }
    ESP_LOGI(TAG, "OTA image validated");

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500, "ota_boot");
    }
    ESP_LOGI(TAG, "OTA boot partition selected");
    char response[96];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"pendingReboot\":false,\"bytesWritten\":%u,\"restartRequired\":true}",
             (unsigned)received);
    return send_json(req, response);
}
static esp_err_t events_get(httpd_req_t *req)
{
    /* Retained for API compatibility. HTTPD has one request worker, so a
     * long-lived response would starve the control APIs. The dashboard uses
     * short-interval polling instead. */
    boost_state_t st;
    boost_model_get_state(&st);
    char event[448];
    int n = snprintf(event, sizeof(event),
             "data: {\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
             "\"brightness\":%d,\"firmwareVersion\":\"%s\",\"uptimeMs\":%llu,\"epochMs\":%lld,"
             "\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\"}\n\n",
             (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
             st.brightness, st.firmware_version ? st.firmware_version : "",
             (unsigned long long)st.uptime_ms, (long long)st.epoch_ms,
             st.timezone_offset_minutes, st.active_theme_id);
    set_common_headers(req, "text/event-stream");
    httpd_resp_set_hdr(req, "retry", "1000");
    return (n > 0 && n < (int)sizeof(event))
        ? httpd_resp_send(req, event, n)
        : ESP_FAIL;
}


static esp_err_t fallback_root_get(httpd_req_t *req)
{
    static const char html[] =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Boost Gauge</title><style>body{margin:0;background:#090A0D;color:#E8ECF2;"
        "font:16px system-ui,sans-serif}.wrap{display:grid;gap:18px;padding:22px;"
        "grid-template-columns:minmax(260px,5fr) minmax(240px,3fr)}.g{aspect-ratio:1;"
        "border:18px solid #20242C;border-radius:50%;display:grid;place-items:center}"
        ".psi{font-size:74px;font-weight:800}.rail{display:grid;gap:10px}"
        "button{padding:10px;background:#20242C;color:#E8ECF2;border:1px solid #3A3F4A}"
        "pre{white-space:pre-wrap;color:#8C95A3}@media(max-width:760px){.wrap{display:block}}</style>"
        "<div class=wrap><div class=g><div><div id=z>ATMO</div><div class=psi id=p>+0.0</div>"
        "<div>PSI</div></div></div><div class=rail><h1>Boost Gauge</h1><pre id=s></pre>"
        "<button onclick='syncTime()'>Sync time</button></div></div><script>"
        "async function syncTime(){await fetch('/api/v1/time',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({epochMs:Date.now(),timezoneOffsetMinutes:-new Date().getTimezoneOffset()})})}"
        "function draw(x){p.textContent=(x.psi>=0?'+':'')+x.psi.toFixed(1);z.textContent=x.zone;"
        "s.textContent=JSON.stringify(x,null,2)}"
        "new EventSource('/api/v1/events').onmessage=e=>draw(JSON.parse(e.data));"
        "fetch('/api/v1/state').then(r=>r.json()).then(draw)</script>";
    set_common_headers(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t root_get(httpd_req_t *req)
{
    const char *uri = req->uri;
    char path_only[160];
    const char *q = strchr(uri, '?');
    if (q != NULL) {
        size_t n = (size_t)(q - uri);
        if (n >= sizeof(path_only)) return send_err(req, HTTPD_400, "bad_path");
        memcpy(path_only, uri, n);
        path_only[n] = '\0';
        uri = path_only;
    }
    if (strstr(uri, "..") != NULL) return send_err(req, HTTPD_400, "bad_path");
    const boost_web_asset_t *asset = boost_web_asset_find(uri);
    if (asset != NULL) {
        httpd_resp_set_type(req, asset->content_type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "ETag", asset->etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)asset->gzip_data, asset->gzip_size);
    }
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) return fallback_root_get(req);
    return send_err(req, HTTPD_404, "not_found");
}

static esp_err_t options_handler(httpd_req_t *req)
{
    set_common_headers(req, "text/plain");
    return httpd_resp_sendstr(req, "");
}

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri = uri,
        .method = method,
        .handler = handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u));
}

static void register_websocket_uri(const char *uri, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t u = {
        .uri = uri,
        .method = HTTP_GET,
        .handler = handler,
        .user_ctx = NULL,
        .is_websocket = true,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &u));
}


static void json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    if (!in) {
        if (out_len) out[0] = '\0';
        return;
    }
    for (size_t i = 0; in[i] && o + 2 < out_len; ++i) {
        char c = in[i];
        if (c == '"' || c == '\\') {
            if (o + 3 >= out_len) break;
            out[o++] = '\\';
            out[o++] = c;
        } else if ((unsigned char)c < 0x20) {
            continue;
        } else {
            out[o++] = c;
        }
    }
    out[o] = '\0';
}

static void network_status_json(char *json, size_t len)
{
    boost_net_status_t st;
    boost_network_get_status(&st);
    char ssid_e[96];
    char ap_e[64];
    json_escape(st.sta_ssid, ssid_e, sizeof(ssid_e));
    json_escape(st.ap_ssid, ap_e, sizeof(ap_e));
    snprintf(json, len,
             "{\"mode\":\"%s\",\"staEnabled\":%s,\"staConnected\":%s,"
             "\"staSsid\":\"%s\",\"staIp\":\"%s\",\"apSsid\":\"%s\",\"apIp\":\"%s\","
             "\"rssi\":%d,\"hasPassword\":%s}",
             st.mode == BOOST_NET_MODE_APSTA ? "apsta" : "ap",
             st.sta_enabled ? "true" : "false",
             st.sta_connected ? "true" : "false",
             ssid_e, st.sta_ip, ap_e, st.ap_ip, st.rssi,
             st.has_sta_pass ? "true" : "false");
}

static esp_err_t network_scan_get(httpd_req_t *req)
{
    boost_wifi_scan_record_t records[BOOST_WIFI_SCAN_MAX_RECORDS] = {0};
    uint16_t count = 0;
    esp_err_t err = boost_network_scan(records, BOOST_WIFI_SCAN_MAX_RECORDS, &count);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "scan_failed");
    }

    char json[1536];
    strlcpy(json, "{\"networks\":[", sizeof(json));
    for (uint16_t i = 0; i < count; ++i) {
        char ssid_e[96];
        char item[160];
        json_escape(records[i].ssid, ssid_e, sizeof(ssid_e));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i == 0 ? "" : ",", ssid_e, records[i].rssi, records[i].authmode);
        strlcat(json, item, sizeof(json));
    }
    strlcat(json, "]}", sizeof(json));
    return send_json(req, json);
}

static esp_err_t network_get(httpd_req_t *req)
{
    char json[512];
    network_status_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t network_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_json");
    }

    const char *ssid = NULL;
    const char *password = NULL;
    bool keep_password = true;
    bool have_mode = false;
    boost_net_mode_t mode = BOOST_NET_MODE_AP;

    cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (cJSON_IsString(ssid_j)) {
        ssid = ssid_j->valuestring;
    }
    cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (cJSON_IsString(pass_j)) {
        password = pass_j->valuestring;
        keep_password = false;
    }
    cJSON *keep_j = cJSON_GetObjectItemCaseSensitive(root, "keepPassword");
    if (cJSON_IsBool(keep_j)) {
        keep_password = cJSON_IsTrue(keep_j);
        if (keep_password) {
            password = "";
        }
    }
    cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(root, "mode");
    if (cJSON_IsString(mode_j) && mode_j->valuestring) {
        have_mode = true;
        if (strcmp(mode_j->valuestring, "apsta") == 0) {
            mode = BOOST_NET_MODE_APSTA;
        } else if (strcmp(mode_j->valuestring, "ap") == 0) {
            mode = BOOST_NET_MODE_AP;
        } else {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_mode");
        }
    }

    esp_err_t err = boost_network_update(ssid, password, keep_password, mode, have_mode);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "network_update_failed");
    }
    /* Do not block the HTTP worker waiting for association; client polls /network. */
    char json[512];
    network_status_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t network_reconnect_post(httpd_req_t *req)
{
    esp_err_t err = boost_network_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        return send_err(req, HTTPD_400, "reconnect_failed");
    }
    char json[512];
    network_status_json(json, sizeof(json));
    return send_json(req, json);
}

esp_err_t boost_web_start(void)
{
    ESP_RETURN_ON_ERROR(boost_media_store_init(), TAG, "media");
    boost_media_store_status_t media = {0};
    if (boost_media_store_status(&media) == ESP_OK && media.present) {
        (void)boost_gauge_media_load();
    }
    ESP_RETURN_ON_ERROR(boost_network_start(25000), TAG, "wifi");
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 10240;
    cfg.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    cfg.max_uri_handlers = 32;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 90;
    cfg.send_wait_timeout = 5;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");
    register_uri(API_BASE "/state", HTTP_GET, state_get);
    register_websocket_uri(WS_STATE_PATH, state_ws_get);
    register_uri(API_BASE "/config", HTTP_GET, config_get);
    register_uri(API_BASE "/config", HTTP_PUT, config_put);
    register_uri(API_BASE "/time", HTTP_POST, time_post);
    register_uri(API_BASE "/themes", HTTP_GET, themes_get);
    register_uri(API_BASE "/themes/active", HTTP_PUT, theme_active_put);
    register_uri(API_BASE "/logs", HTTP_GET, logs_get);
    register_uri(API_BASE "/logs", HTTP_DELETE, logs_delete);
    register_uri(API_BASE "/logs.csv", HTTP_GET, logs_csv_get);
    register_uri(API_BASE "/media", HTTP_POST, media_post);
    register_uri(API_BASE "/media", HTTP_DELETE, media_delete);
    register_uri(API_BASE "/media/status", HTTP_GET, media_status_get);
    register_uri(API_BASE "/ota", HTTP_POST, ota_post);
    register_uri(API_BASE "/network", HTTP_GET, network_get);
    register_uri(API_BASE "/network", HTTP_PUT, network_put);
    register_uri(API_BASE "/network/reconnect", HTTP_POST, network_reconnect_post);
    register_uri(API_BASE "/network/scan", HTTP_GET, network_scan_get);
    register_uri(API_BASE "/events", HTTP_GET, events_get);
    register_uri("/*", HTTP_GET, root_get);
    register_uri("/*", HTTP_OPTIONS, options_handler);
    ESP_LOGI(TAG, "HTTP API ready");
    if (xTaskCreate(state_ws_task, "boost_ws", 3072, NULL, 3, &s_state_ws_task) != pdPASS) {
        ESP_LOGW(TAG, "live WebSocket task not started");
    }
    return ESP_OK;
}
