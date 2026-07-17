#include "boost_web.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "boost_model.h"
#include "generated_web_assets.h"

#define API_BASE "/api/v1"
#define AP_PASSWORD "boost1234"
#define HTTP_CHUNK 2048
#define MAX_JSON_BODY 4096
#define MAX_GIF_BYTES (2 * 1024 * 1024)
#define MEDIA_PATH "/spiffs/active.gif"
#define WEB_ROOT "/spiffs"

static const char *TAG = "boost_web";
static httpd_handle_t s_httpd;

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

static esp_err_t state_get(httpd_req_t *req)
{
    boost_state_t st;
    boost_model_get_state(&st);
    char json[384];
    snprintf(json, sizeof(json),
             "{\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
             "\"brightness\":%d,\"firmwareVersion\":\"%s\",\"uptimeMs\":%llu,"
             "\"epochMs\":%lld,\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\"}",
             (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
             st.brightness, st.firmware_version, (unsigned long long)st.uptime_ms,
             (long long)st.epoch_ms, st.timezone_offset_minutes, st.active_theme_id);
    return send_json(req, json);
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
    size_t n = boost_model_copy_logs(logs, BOOST_LOG_CAPACITY);
    set_common_headers(req, "text/csv");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"boost-gauge-log.csv\"");
    httpd_resp_sendstr_chunk(req, "tMs,psi,peakPsi,zone,demo\n");
    char row[96];
    for (size_t i = 0; i < n; ++i) {
        snprintf(row, sizeof(row), "%lu,%.2f,%.2f,%s,%d\n",
                 (unsigned long)logs[i].t_ms, (double)logs[i].psi,
                 (double)logs[i].peak_psi, logs[i].zone, logs[i].demo ? 1 : 0);
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
    boost_media_status_t st;
    boost_model_get_media_status(&st);
    char json[192];
    snprintf(json, sizeof(json),
             "{\"present\":%s,\"name\":\"%s\",\"size\":%u,\"uploadedAtMs\":%lu,"
             "\"playbackSupported\":%s,\"playback\":\"deferred\"}",
             st.present ? "true" : "false", st.name, (unsigned)st.size,
             (unsigned long)st.uploaded_at_ms, st.playback_supported ? "true" : "false");
    return send_json(req, json);
}

static esp_err_t media_delete(httpd_req_t *req)
{
    unlink(MEDIA_PATH);
    boost_media_status_t st = {
        .present = false,
        .playback_supported = false,
    };
    boost_model_set_media_status(&st);
    return media_status_get(req);
}

static esp_err_t media_post(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_GIF_BYTES) {
        return send_err(req, HTTPD_400, "gif_size");
    }
    FILE *f = fopen(MEDIA_PATH, "wb");
    if (f == NULL) {
        return send_err(req, HTTPD_500, "media_open");
    }
    char *buf = malloc(HTTP_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return send_err(req, HTTPD_500, "no_mem");
    }
    size_t received = 0;
    char header[6] = {0};
    bool header_ok = false;
    while (received < req->content_len) {
        size_t want = req->content_len - received;
        if (want > HTTP_CHUNK) {
            want = HTTP_CHUNK;
        }
        int n = httpd_req_recv(req, buf, want);
        if (n <= 0) {
            free(buf);
            fclose(f);
            unlink(MEDIA_PATH);
            return send_err(req, HTTPD_400, "upload_failed");
        }
        if (received < sizeof(header)) {
            size_t copy = sizeof(header) - received;
            if (copy > (size_t)n) {
                copy = (size_t)n;
            }
            memcpy(header + received, buf, copy);
            if (received + copy >= sizeof(header)) {
                header_ok = memcmp(header, "GIF87a", 6) == 0 || memcmp(header, "GIF89a", 6) == 0;
                if (!header_ok) {
                    free(buf);
                    fclose(f);
                    unlink(MEDIA_PATH);
                    return send_err(req, HTTPD_400, "not_gif");
                }
            }
        }
        fwrite(buf, 1, (size_t)n, f);
        received += (size_t)n;
    }
    free(buf);
    fclose(f);
    if (!header_ok) {
        unlink(MEDIA_PATH);
        return send_err(req, HTTPD_400, "not_gif");
    }
    boost_media_status_t st = {
        .size = received,
        .uploaded_at_ms = (uint32_t)(esp_timer_get_time() / 1000ULL),
        .present = true,
        .playback_supported = false,
    };
    strlcpy(st.name, "active.gif", sizeof(st.name));
    boost_model_set_media_status(&st);
    return media_status_get(req);
}

static esp_err_t ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL || req->content_len == 0) {
        return send_err(req, HTTPD_400, "ota_unavailable");
    }
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
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "ota_invalid");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500, "ota_boot");
    }
    return send_json(req, "{\"ok\":true,\"pendingReboot\":true}");
}

static esp_err_t events_get(httpd_req_t *req)
{
    set_common_headers(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    for (int i = 0; i < 600; ++i) {
        boost_state_t st;
        boost_model_get_state(&st);
        char event[384];
        snprintf(event, sizeof(event),
                 "data: {\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
                 "\"brightness\":%d,\"uptimeMs\":%llu,\"epochMs\":%lld,"
                 "\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\"}\n\n",
                 (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
                 st.brightness, (unsigned long long)st.uptime_ms, (long long)st.epoch_ms,
                 st.timezone_offset_minutes, st.active_theme_id);
        if (httpd_resp_sendstr_chunk(req, event) != ESP_OK) {
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return httpd_resp_sendstr_chunk(req, NULL);
}

static const char *mime_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == NULL) {
        return "application/octet-stream";
    }
    if (strcmp(dot, ".html") == 0) {
        return "text/html";
    }
    if (strcmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(dot, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    return "application/octet-stream";
}

static esp_err_t stream_file(httpd_req_t *req, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    set_common_headers(req, mime_for_path(path));
    char *buf = malloc(HTTP_CHUNK);
    if (buf == NULL) {
        fclose(f);
        return send_err(req, HTTPD_500, "no_mem");
    }
    while (true) {
        size_t n = fread(buf, 1, HTTP_CHUNK, f);
        if (n > 0 && httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            free(buf);
            fclose(f);
            return ESP_FAIL;
        }
        if (n < HTTP_CHUNK) {
            break;
        }
    }
    free(buf);
    fclose(f);
    return httpd_resp_sendstr_chunk(req, NULL);
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
    if (strstr(uri, "..") != NULL || strchr(uri, '?') != NULL) {
        return send_err(req, HTTPD_400, "bad_path");
    }

    const boost_web_asset_t *asset = boost_web_asset_find(uri);
    if (asset != NULL) {
        httpd_resp_set_type(req, asset->content_type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "ETag", asset->etag);
        httpd_resp_set_hdr(req, "Cache-Control", "public, max-age=3600");
        return httpd_resp_send(req, (const char *)asset->gzip_data, asset->gzip_size);
    }

    char path[160];
    const char *file_uri = strcmp(uri, "/") == 0 ? "/index.html" : uri;
    snprintf(path, sizeof(path), WEB_ROOT "%s", file_uri);
    if (stream_file(req, path) == ESP_OK) {
        return ESP_OK;
    }
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        return fallback_root_get(req);
    }
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

static esp_err_t mount_spiffs(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    struct stat st;
    if (stat(MEDIA_PATH, &st) == 0 && st.st_size > 0) {
        boost_media_status_t ms = {
            .size = (size_t)st.st_size,
            .present = true,
            .playback_supported = false,
        };
        strlcpy(ms.name, "active.gif", sizeof(ms.name));
        boost_model_set_media_status(&ms);
    }
    return ESP_OK;
}

static esp_err_t start_ap(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "BoostGauge-%02X%02X", mac[4], mac[5]);
    strlcpy((char *)ap.ap.password, AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.max_connection = 4;
    ap.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    if (strlen(AP_PASSWORD) == 0) {
        ap.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
    ESP_LOGI(TAG, "AP %s password %s", ap.ap.ssid, AP_PASSWORD);
    return ESP_OK;
}

esp_err_t boost_web_start(void)
{
    mount_spiffs();
    ESP_RETURN_ON_ERROR(start_ap(), TAG, "ap");

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 24;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");

    register_uri(API_BASE "/state", HTTP_GET, state_get);
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
    register_uri(API_BASE "/events", HTTP_GET, events_get);
    register_uri("/*", HTTP_GET, root_get);
    register_uri("/*", HTTP_OPTIONS, options_handler);
    ESP_LOGI(TAG, "HTTP API ready");
    return ESP_OK;
}
