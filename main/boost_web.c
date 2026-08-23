#include "boost_web.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
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

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lvgl.h"

#include "nvs_flash.h"

#include "boost_model.h"
#include "boost_sensors.h"
#include "boost_gauge.h"
#include "boost_display.h"
#include "boost_network.h"
#include "boost_media_store.h"
#include "boost_obd.h"
#include "boost_app_ble.h"
#include "boost_tpms.h"
#include "boost_tpms_protocol.h"
#include "generated_web_assets.h"

#define API_BASE "/api/v1"
#define WS_STATE_PATH API_BASE "/state/ws"

#define HTTP_CHUNK 16384
#define MAX_JSON_BODY 4096
#define MAX_GIF_BYTES BOOST_MEDIA_STORE_MAX_BYTES
#define MAX_GIF_DIMENSION BOOST_MEDIA_STORE_MAX_DIMENSION


static const char *TAG = "boost_web";
static httpd_handle_t s_httpd;
#define STATE_WS_MAX_CLIENTS 3

/* Telemetry push cadence.
 *
 * The push loop is driven by task notifications from the sample task (see
 * boost_web_notify_sample(), called from sample_task in main.c) so a fresh
 * sample is framed and queued the moment it exists. Previously this loop slept
 * a flat STATE_WS_FRAME_MS between pushes, free-running and unsynchronised with
 * sample production, so every sample sat 0-50 ms (mean 25 ms) before transmit.
 *
 * STATE_WS_PUSH_DECIMATION is how many sample notifications are consumed per
 * outbound frame. The sample task runs at 62.5 Hz (16 ms period, main.c):
 *   1 -> 62.5 Hz, ~0 ms added latency, ~56 kB/s and ~190 async sends/s at 3 clients
 *   2 -> 31.25 Hz, ~16 ms added latency, half the httpd task load
 * Decimation rather than a wall-clock rate gate is deliberate: a gate whose
 * period is not a multiple of the 16 ms sample period aliases - a 20 ms gate
 * yields a 32 ms beat (31.25 Hz), not the 50 Hz it looks like.
 *
 * COUPLED to the browser: GAUGE_EMA_TAU_MS in web/app.js is sized for this
 * rate. Raising decimation here without raising tau there makes the needle
 * visibly step. Watch display.renderFps / display.worstRenderUs on
 * /api/v1/state when changing this - the physical gauge must not regress.
 */
#define STATE_WS_PUSH_DECIMATION 1

/* Fallback cadence used only when no sample notification arrives (sample task
 * stalled, or not started yet). Keeps clients fed rather than going silent. */
#define STATE_WS_FRAME_MS 50
/* `gen` is bumped every time a slot is released. It is what lets a slot be
 * reused while an async send for the previous occupant is still queued in the
 * httpd task: the stale completion no longer matches the slot, so it frees its
 * own ctx/payload and touches nothing. Matching on `fd` alone cannot do this -
 * the release sets `fd = -1`, so the completion never matches, and `inflight`
 * stays set forever. That is the slot leak. */
typedef struct {
    int fd;
    bool inflight;
    void *payload;
    uint32_t gen;
} state_ws_client_t;
typedef struct {
    int slot;
    int fd;
    uint32_t gen;
    void *payload;
} state_ws_send_ctx_t;
static state_ws_client_t s_state_ws_clients[STATE_WS_MAX_CLIENTS] = {
    { .fd = -1 }, { .fd = -1 }, { .fd = -1 },
};
/* Written once at startup, read from sample_task on the other core. */
static TaskHandle_t volatile s_state_ws_task;
static volatile bool s_media_upload_in_progress;
static portMUX_TYPE s_web_lock = portMUX_INITIALIZER_UNLOCKED;


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
             "{\"id\":\"%s\",\"name\":\"%s\",\"style\":\"%s\",\"colors\":{\"face\":\"#%06lx\","
             "\"track\":\"#%06lx\",\"text\":\"#%06lx\",\"muted\":\"#%06lx\","
             "\"vacuum\":\"#%06lx\",\"boost\":\"#%06lx\",\"overboost\":\"#%06lx\","
             "\"zero\":\"#%06lx\"},"
             "\"customized\":%s}",
             theme->id, theme->name, boost_style_name(theme->style),
             (unsigned long)theme->face, (unsigned long)theme->track,
             (unsigned long)theme->text, (unsigned long)theme->muted,
             (unsigned long)theme->vacuum, (unsigned long)theme->boost,
             (unsigned long)theme->overboost, (unsigned long)theme->zero,
             boost_theme_is_customized(theme->id) ? "true" : "false");
    strlcat(buf, tmp, len);
}

static int state_json(char *json, size_t len)
{
    boost_state_t st;
    boost_model_get_state(&st);
    boost_tpms_config_t tpms_cfg;
    boost_tpms_get_config(&tpms_cfg);
    return snprintf(json, len,
                    "{\"psi\":%.2f,\"peakPsi\":%.2f,\"zone\":\"%s\",\"demo\":%s,"
                    "\"brightness\":%d,\"firmwareVersion\":\"%s\",\"uptimeMs\":%llu,"
                    "\"epochMs\":%lld,\"timezoneOffsetMinutes\":%d,\"activeThemeId\":\"%s\",\"activePage\":%d,"
                    "\"display\":{\"renderFps\":%lu,\"gaugeDemandPerSecond\":%lu,\"flushesPerSecond\":%lu,\"pixelsPerSecond\":%lu,"
                    "\"worstRenderUs\":%lu,\"renderGapP50Us\":%lu,"
                    "\"renderGapMaxUs\":%lu,\"framesOverBudget\":%lu,"
                    "\"tePeriodUs\":%lu,\"teWaits\":%lu,\"teTimeouts\":%lu,\"teSkips\":%lu,"
                    "\"teScanlineWaits\":%lu},"
                    /* Raw sensor readings so a bench check against a known value
                     * (atmospheric ~101.3 kPa, gauge ~0 psi engine-off) is
                     * possible without the display. */
                    "\"sensors\":{\"adsPresent\":%s,\"bmpPresent\":%s,\"fault\":%s,"
                    "\"mapVolts\":%.4f,\"mapAbsKpa\":%.2f,\"ambientKpa\":%.2f},"
                    "\"tpms\":{\"status\":%d,\"lowPsi\":%.1f,\"wheels\":[{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s},{\"psi\":%.1f,\"valid\":%s}]},"
                    "\"obd\":{\"state\":%d,\"lastError\":%u,\"peer\":\"%s\",\"peerAddr\":\"%s\",\"uptimeMs\":%lu,\"ageMs\":%lu,\"valid\":%s,"
                    "\"lastReply\":\"%s\",\"protocol\":\"%s\","
                    "\"rpm\":%.1f,\"speedKph\":%.1f,\"coolantC\":%.1f,\"mapKpa\":%.1f,\"iatC\":%.1f,"
                    "\"throttlePct\":%.1f,\"mafGps\":%.1f,\"fuelPct\":%.1f,\"batteryV\":%.1f}}",
                    (double)st.psi, (double)st.peak_psi, st.zone, st.demo ? "true" : "false",
                    st.brightness, st.firmware_version, (unsigned long long)st.uptime_ms,
                    (long long)st.epoch_ms, st.timezone_offset_minutes, st.active_theme_id, st.active_page,
                    (unsigned long)st.display.render_fps,
                    (unsigned long)st.display.gauge_demand_per_second,
                    (unsigned long)st.display.flushes_per_second,
                    (unsigned long)st.display.pixels_per_second,
                    (unsigned long)st.display.worst_render_us,
                    (unsigned long)st.display.render_gap_p50_us,
                    (unsigned long)st.display.render_gap_max_us,
                    (unsigned long)st.display.frames_over_budget,
                    (unsigned long)st.display.te_period_us,
                    (unsigned long)st.display.te_waits,
                    (unsigned long)st.display.te_timeouts,
                    (unsigned long)st.display.te_skips,
                    (unsigned long)st.display.te_scanline_waits,
                    st.ads_present ? "true" : "false",
                    st.bmp_present ? "true" : "false",
                    st.sensor_fault ? "true" : "false",
                    (double)st.map_volts, (double)st.map_abs_kpa, (double)st.ambient_kpa,
                    st.tpms_status, (double)boost_tpms_protocol_kpa_to_psi(tpms_cfg.low_kpa),
                    (double)st.tpms_psi[0], st.tpms_valid[0] ? "true" : "false",
                    (double)st.tpms_psi[1], st.tpms_valid[1] ? "true" : "false",
                    (double)st.tpms_psi[2], st.tpms_valid[2] ? "true" : "false",
                    (double)st.tpms_psi[3], st.tpms_valid[3] ? "true" : "false",
                    st.obd_state, (unsigned)st.obd_last_error, st.obd_peer, st.obd_peer_addr,
                    (unsigned long)st.obd_uptime_ms, (unsigned long)st.obd_age_ms,
                    st.obd_valid ? "true" : "false",
                    st.obd_last_reply, st.obd_protocol,
                    (double)st.obd_rpm, (double)st.obd_speed_kph, (double)st.obd_coolant_c,
                    (double)st.obd_map_kpa, (double)st.obd_iat_c,
                    (double)st.obd_throttle_pct, (double)st.obd_maf_gps,
                    (double)st.obd_fuel_pct, (double)st.obd_battery_v);
}

static esp_err_t state_get(httpd_req_t *req)
{
    char json[1536];
    const int n = state_json(json, sizeof(json));
    return n > 0 && n < (int)sizeof(json) ? send_json(req, json) : ESP_FAIL;
}

/* Live I2C bus scan — a serial-free way to see what is actually on the sensor
 * bus. `busUp:false` means the bus never initialised; an empty `found` with the
 * bus up points at wiring/power/pull-ups or swapped SDA/SCL; 0x48 and 0x76
 * confirm the ADS1115 and BMP280. */
static esp_err_t sensors_scan_get(httpd_req_t *req)
{
    uint8_t found[32];
    const int count = boost_sensors_i2c_scan(found, (int)sizeof(found));
    char json[256];
    int off = snprintf(json, sizeof(json),
                       "{\"busUp\":%s,\"recoveries\":%lu,\"found\":[",
                       count < 0 ? "false" : "true",
                       (unsigned long)boost_sensors_recoveries());
    for (int i = 0; i < count && off < (int)sizeof(json) - 8; ++i) {
        off += snprintf(json + off, sizeof(json) - off, "%s\"0x%02X\"",
                        i ? "," : "", found[i]);
    }
    snprintf(json + off, sizeof(json) - off, "]}");
    return send_json(req, json);
}

/* Calibration diagnostics live on their own endpoint rather than on /state:
 * /state is rendered at the 62.5 Hz WebSocket cadence into a smaller buffer, and
 * these fields are only ever read by the Settings panel. Keeping them apart is
 * what stops the telemetry payload from growing for a once-a-service reading.
 *
 * Sourced from boost_sensors_get_sample(), so it reports the real sensor path
 * even while demo mode is driving the gauge. */
static long long age_ms_json(uint32_t age_ms)
{
    /* UINT32_MAX is the firmware's "never read successfully" sentinel; emitting
     * it literally would read as a 49-day-old sample in the dashboard. */
    return age_ms == UINT32_MAX ? -1 : (long long)age_ms;
}

static int sensors_calibration_json(char *json, size_t len)
{
    const boost_sample_t s = boost_sensors_get_sample();
    const boost_map_cal_t cal = boost_sensors_get_calibration();
    const bool cal_valid = cal.version != 0;
    return snprintf(json, len,
                    "{\"supplyVolts\":%.4f,"
                    "\"live\":{\"adsPresent\":%s,\"bmpPresent\":%s,\"fault\":%s,"
                    "\"mapVolts\":%.4f,\"mapAgeMs\":%lld,\"nominalKpa\":%.2f,"
                    "\"correctedKpa\":%.2f,\"bmpKpa\":%.2f,\"bmpAgeMs\":%lld,"
                    "\"bmpUpdates\":%lu,\"ambientIsFallback\":%s},"
                    "\"calibration\":{\"valid\":%s,\"version\":%u,"
                    "\"offsetKpa\":%.2f,\"offsetPsi\":%.3f,\"supplyVolts\":%.4f,"
                    "\"refMapVolts\":%.4f,\"refNominalKpa\":%.2f,"
                    "\"refBmpKpa\":%.2f,\"samples\":%u,\"epochMs\":%lld}}",
                    (double)boost_sensors_get_supply_volts(),
                    s.ads_present ? "true" : "false",
                    s.bmp_present ? "true" : "false",
                    s.sensor_fault ? "true" : "false",
                    (double)s.map_volts, age_ms_json(s.ads_age_ms),
                    (double)boost_sensors_nominal_kpa(s.map_volts),
                    (double)s.map_abs_kpa, (double)s.ambient_kpa,
                    age_ms_json(s.bmp_age_ms), (unsigned long)s.bmp_updates,
                    s.ambient_is_fallback ? "true" : "false",
                    cal_valid ? "true" : "false", (unsigned)cal.version,
                    cal_valid ? (double)cal.offset_kpa : 0.0,
                    cal_valid ? (double)(cal.offset_kpa * 0.145037738f) : 0.0,
                    cal_valid ? (double)cal.supply_volts : 0.0,
                    cal_valid ? (double)cal.ref_map_volts : 0.0,
                    cal_valid ? (double)cal.ref_nominal_kpa : 0.0,
                    cal_valid ? (double)cal.ref_bmp_kpa : 0.0,
                    cal_valid ? (unsigned)cal.samples : 0u,
                    cal_valid ? (long long)cal.epoch_ms : 0LL);
}

static esp_err_t sensors_calibration_send(httpd_req_t *req)
{
    char json[768];
    const int n = sensors_calibration_json(json, sizeof(json));
    return n > 0 && n < (int)sizeof(json) ? send_json(req, json) : ESP_FAIL;
}

static esp_err_t sensors_calibration_get(httpd_req_t *req)
{
    return sensors_calibration_send(req);
}

/* Blocks for roughly two seconds while boost_sensors_calibrate_atmosphere()
 * observes published snapshots. That is deliberate for a manual, operator-
 * triggered action, but it does occupy the single httpd task for the duration:
 * state polls and queued WebSocket frames wait behind it. Do not call it from
 * anything automatic. */
static esp_err_t sensors_calibration_post(httpd_req_t *req)
{
    /* Body is ignored per the API contract, but it still has to be drained or
     * the next request on this connection starts mid-body. */
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    free(body);
    const boost_cal_result_t result = boost_sensors_calibrate_atmosphere(NULL);
    if (result != BOOST_CAL_OK) {
        /* The code string comes from the sensor module so the table lives in
         * exactly one place. Only a persistence failure is a server fault; every
         * other rejection means the bench state is wrong for calibrating. */
        const char *status = (result == BOOST_CAL_ERR_PERSIST) ? HTTPD_500 : "409 Conflict";
        return send_err(req, status, boost_sensors_cal_error_code(result));
    }
    return sensors_calibration_send(req);
}

static esp_err_t sensors_supply_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    const cJSON *volts = cJSON_GetObjectItemCaseSensitive(root, "supplyVolts");
    if (!cJSON_IsNumber(volts)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_supply");
    }
    const double v = volts->valuedouble;
    cJSON_Delete(root);
    /* Range-check as a double before narrowing: NaN or 1e300 cast to float is
     * undefined, so the firmware's own bounds check would be judging a value
     * that never arrived intact. */
    if (!(v >= (double)BOOST_MAP_SUPPLY_MIN && v <= (double)BOOST_MAP_SUPPLY_MAX)) {
        return send_err(req, HTTPD_400, "invalid_supply");
    }
    const esp_err_t err = boost_sensors_set_supply_volts((float)v);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_err(req, HTTPD_400, "invalid_supply");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_500, "persist_failed");
    }
    return sensors_calibration_send(req);
}

/* Return a slot to the pool. A frame may still be queued in the httpd task at
 * this point; `ctx` and `ctx->payload` are owned by state_ws_send_done() and
 * MUST NOT be freed here. Once httpd_ws_send_data_async() has returned ESP_OK
 * the completion always runs exactly once (httpd_ws_send_cb calls it whether
 * the send succeeded or the socket was already gone), so dropping our pointer
 * to the ctx leaks nothing. Bumping `gen` is what makes that late completion
 * harmless and the slot immediately reusable. */
static void state_ws_release_locked(int slot)
{
    s_state_ws_clients[slot].fd = -1;
    s_state_ws_clients[slot].payload = NULL;
    s_state_ws_clients[slot].inflight = false;
    s_state_ws_clients[slot].gen++;
}

static void state_ws_send_done(esp_err_t err, int socket, void *arg)
{
    state_ws_send_ctx_t *ctx = (state_ws_send_ctx_t *)arg;
    if (ctx == NULL) return;
    portENTER_CRITICAL(&s_web_lock);
    if (ctx->slot >= 0 && ctx->slot < STATE_WS_MAX_CLIENTS) {
        state_ws_client_t *client = &s_state_ws_clients[ctx->slot];
        /* gen first: it rejects a completion for a previous occupant even when
         * the kernel has handed the same fd back out. payload == ctx is kept
         * so a stale completion inside the same generation still cannot clear
         * the accounting for a different frame. */
        if (client->gen == ctx->gen && client->fd == ctx->fd && client->payload == ctx) {
            client->payload = NULL;
            client->inflight = false;
            if (err != ESP_OK) state_ws_release_locked(ctx->slot);
        }
    }
    portEXIT_CRITICAL(&s_web_lock);
    free(ctx->payload);
    free(ctx);
}

static void state_ws_push(void *arg)
{
    (void)arg;
    /* Static: this runs only on the single boost_ws task, once per loop
     * iteration, so there is no reentrancy. Keeping the 1.5 KB serialization
     * buffer off the task stack is what stops the TPMS/OBD-augmented payload
     * from overflowing it (the stack was sized for the pre-TPMS 768 B buffer). */
    static char current[1536];
    const int n = state_json(current, sizeof(current));
    if (n <= 0 || n >= (int)sizeof(current)) return;
    for (int slot = 0; slot < STATE_WS_MAX_CLIENTS; ++slot) {
        portENTER_CRITICAL(&s_web_lock);
        const int fd = s_state_ws_clients[slot].fd;
        const uint32_t gen = s_state_ws_clients[slot].gen;
        const bool allowed = s_httpd != NULL && fd >= 0 && !s_state_ws_clients[slot].inflight;
        if (allowed) s_state_ws_clients[slot].inflight = true;
        portEXIT_CRITICAL(&s_web_lock);
        if (!allowed) continue;
        state_ws_send_ctx_t *ctx = calloc(1, sizeof(*ctx));
        char *json = malloc((size_t)n + 1U);
        if (ctx == NULL || json == NULL) {
            free(ctx); free(json);
            portENTER_CRITICAL(&s_web_lock);
            if (s_state_ws_clients[slot].gen == gen && s_state_ws_clients[slot].fd == fd &&
                s_state_ws_clients[slot].payload == NULL)
                s_state_ws_clients[slot].inflight = false;
            portEXIT_CRITICAL(&s_web_lock);
            continue;
        }
        memcpy(json, current, (size_t)n + 1U);
        ctx->slot = slot; ctx->fd = fd; ctx->gen = gen; ctx->payload = json;
        httpd_ws_frame_t frame = { .final = true, .type = HTTPD_WS_TYPE_TEXT,
                                   .payload = (uint8_t *)json, .len = (size_t)n };
        portENTER_CRITICAL(&s_web_lock);
        if (s_state_ws_clients[slot].gen != gen || s_state_ws_clients[slot].fd != fd ||
            !s_state_ws_clients[slot].inflight) {
            portEXIT_CRITICAL(&s_web_lock); free(json); free(ctx); continue;
        }
        s_state_ws_clients[slot].payload = ctx;
        httpd_handle_t httpd = s_httpd;
        portEXIT_CRITICAL(&s_web_lock);
        const esp_err_t err = httpd_ws_send_data_async(httpd, fd, &frame, state_ws_send_done, ctx);
        if (err != ESP_OK) {
            /* The work was never queued, so no completion will run: this path
             * owns the buffers. */
            portENTER_CRITICAL(&s_web_lock);
            if (s_state_ws_clients[slot].gen == gen && s_state_ws_clients[slot].fd == fd &&
                s_state_ws_clients[slot].payload == ctx) {
                state_ws_release_locked(slot);
            }
            portEXIT_CRITICAL(&s_web_lock);
            free(json); free(ctx);
        }
    }
}

void boost_web_notify_sample(void)
{
    TaskHandle_t task = s_state_ws_task;
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
}

static void state_ws_task(void *arg)
{
    (void)arg;
    uint32_t pending = 0;
    while (true) {
        /* Wake on a freshly published sample; fall back to the idle cadence so
         * a stalled producer cannot silence the socket entirely. */
        const uint32_t notified = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(STATE_WS_FRAME_MS));
        if (notified > 0U) {
            pending += notified;
            if (pending < (uint32_t)STATE_WS_PUSH_DECIMATION) {
                continue;
            }
        }
        pending = 0;

        portENTER_CRITICAL(&s_web_lock);
        const bool ready = !s_media_upload_in_progress && s_httpd != NULL;
        portEXIT_CRITICAL(&s_web_lock);
        if (ready) state_ws_push(NULL);
    }
}

static int state_ws_find_slot_locked(int fd)
{
    for (int i = 0; i < STATE_WS_MAX_CLIENTS; ++i) {
        if (s_state_ws_clients[i].fd == fd) return i;
    }
    return -1;
}

static esp_err_t state_ws_get(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        int slot = -1;
        portENTER_CRITICAL(&s_web_lock);
        slot = state_ws_find_slot_locked(fd);
        if (slot < 0) {
            for (int i = 0; i < STATE_WS_MAX_CLIENTS; ++i) {
                if (s_state_ws_clients[i].fd < 0 && !s_state_ws_clients[i].inflight) {
                    s_state_ws_clients[i].fd = fd;
                    slot = i;
                    break;
                }
            }
        }
        portEXIT_CRITICAL(&s_web_lock);
        if (slot < 0) {
            (void)httpd_sess_trigger_close(s_httpd, fd);
            return ESP_FAIL;
        }
        return ESP_OK;
    }

    uint8_t discard[128];
    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err == ESP_OK) {
        if (frame.len > sizeof(discard)) {
            err = ESP_ERR_INVALID_SIZE;
        } else if (frame.len > 0) {
            frame.payload = discard;
            err = httpd_ws_recv_frame(req, &frame, frame.len);
        }
    }
    if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_CLOSE) {
        frame.len = 0;
        frame.payload = NULL;
        err = httpd_ws_send_frame(req, &frame);
    } else if (err == ESP_OK && frame.type == HTTPD_WS_TYPE_PING) {
        frame.type = HTTPD_WS_TYPE_PONG;
        frame.len = 0;
        frame.payload = NULL;
        err = httpd_ws_send_frame(req, &frame);
    }
    if (frame.type == HTTPD_WS_TYPE_CLOSE || err != ESP_OK) {
        portENTER_CRITICAL(&s_web_lock);
        const int slot = state_ws_find_slot_locked(fd);
        if (slot >= 0) state_ws_release_locked(slot);
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
             "\"timezoneOffsetMinutes\":%d,\"timezoneTz\":\"%s\",\"activeThemeId\":\"%s\","
             "\"psiMin\":%.2f,\"psiMax\":%.2f,\"psiOverboost\":%.2f,\"zeroAngle\":%.2f,"
             "\"appBle\":%s}",
             cfg.brightness_high, cfg.brightness_low,
             cfg.dim_schedule.enabled ? "true" : "false",
             cfg.dim_schedule.start_minutes, cfg.dim_schedule.end_minutes,
             cfg.timezone_offset_minutes, cfg.timezone_tz, cfg.active_theme_id,
             (double)cfg.psi_min, (double)cfg.psi_max, (double)cfg.psi_overboost,
             (double)cfg.zero_angle, boost_app_ble_enabled() ? "true" : "false");
}

static esp_err_t config_get(httpd_req_t *req)
{
    char json[512];
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

static bool json_float(cJSON *obj, const char *name, float *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = (float)v->valuedouble;
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
    float ftmp;
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
    cJSON *tzstr = cJSON_GetObjectItemCaseSensitive(root, "timezoneTz");
    if (cJSON_IsString(tzstr)) {
        strlcpy(patch.timezone_tz, tzstr->valuestring, sizeof(patch.timezone_tz));
        fields |= BOOST_CONFIG_TZ_TZ;
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
    if (json_float(root, "psiMin", &ftmp)) {
        patch.psi_min = ftmp;
        fields |= BOOST_CONFIG_PSI_MIN;
    }
    if (json_float(root, "psiMax", &ftmp)) {
        patch.psi_max = ftmp;
        fields |= BOOST_CONFIG_PSI_MAX;
    }
    if (json_float(root, "psiOverboost", &ftmp)) {
        patch.psi_overboost = ftmp;
        fields |= BOOST_CONFIG_PSI_OVERBOOST;
    }
    if (json_float(root, "zeroAngle", &ftmp)) {
        patch.zero_angle = ftmp;
        fields |= BOOST_CONFIG_ZERO_ANGLE;
    }
    /* Companion-app BLE peripheral toggle. Runtime, persisted, no reboot
     * needed: enable advertises the GATT server, disable tears the link. */
    const cJSON *able = cJSON_GetObjectItemCaseSensitive(root, "appBle");
    if (cJSON_IsBool(able)) {
        boost_app_ble_set_enabled(cJSON_IsTrue(able));
    }
    cJSON_Delete(root);
    esp_err_t err = boost_model_update_config(&patch, fields);
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "invalid_config");
    }
    if (fields & (BOOST_CONFIG_PSI_MIN | BOOST_CONFIG_PSI_MAX | BOOST_CONFIG_PSI_OVERBOOST |
                  BOOST_CONFIG_ZERO_ANGLE | BOOST_CONFIG_THEME)) {
        if (boost_display_lock(1000) == ESP_OK) {
            if (fields & BOOST_CONFIG_THEME) {
                boost_gauge_apply_theme(boost_model_active_theme());
            }
            if (fields & (BOOST_CONFIG_PSI_MIN | BOOST_CONFIG_PSI_MAX | BOOST_CONFIG_PSI_OVERBOOST |
                          BOOST_CONFIG_ZERO_ANGLE)) {
                boost_gauge_apply_config();
            }
            boost_display_unlock();
        }
    }
    char json[512];
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
    cJSON *tzstr = cJSON_GetObjectItemCaseSensitive(root, "timezoneTz");
    if (!cJSON_IsNumber(epoch) || !cJSON_IsNumber(tz)) {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "invalid_time");
    }
    const char *tz_tz = (cJSON_IsString(tzstr) && tzstr->valuestring != NULL)
        ? tzstr->valuestring : NULL;
    esp_err_t err = boost_model_set_time((int64_t)epoch->valuedouble, tz->valueint, tz_tz);
    cJSON_Delete(root);
    if (err == ESP_ERR_INVALID_STATE) {
        /* The client disagrees with a valid DS3231: it is the buggy clock, not
         * a correction. Distinct status so the dashboard can explain itself. */
        return send_err(req, "409 Conflict", "clock_rejected");
    }
    if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "time_not_set");
    }
    boost_model_refresh_status();
    return state_get(req);

}

/* Upper bound on one theme object. append_theme_json() renders through a 384
 * byte scratch buffer, so nothing longer than that can reach the accumulator;
 * the +1 is the separating comma. */
#define THEME_JSON_MAX (384 + 1)

static esp_err_t themes_get(httpd_req_t *req)
{
    /* Sized from the theme count, not fixed. A 2048 byte buffer fit the four
     * original themes; the three neon themes took the payload to 2,047 bytes
     * and strlcat() truncated it silently, so the endpoint served invalid JSON
     * and the whole dashboard failed to parse it. strlcat cannot report that,
     * which is why the length is checked explicitly below. */
    const size_t cap = 1024 + boost_theme_count() * THEME_JSON_MAX;
    char *json = calloc(1, cap);
    if (json == NULL) {
        return send_err(req, HTTPD_500, "themes_alloc");
    }
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    snprintf(json, cap,
             "{\"activeThemeId\":\"%s\",\"bigDigitStaticBg\":%s,"
             "\"bigDigitColorText\":%s,\"bigDigitStaticColor\":\"#%06lx\","
             "\"bigDigitTextColor\":\"#%06lx\","
             "\"arcGradient\":%s,\"hudGradient\":%s,\"hudTrueBlack\":%s,\"neonMarqueeSpin\":%s,"
             "\"teSync\":%s,\"regionDBuf\":%s,\"teScanline\":%s,"
             "\"rotation\":%u,"
             "\"vaultFace\":\"#%06lx\",\"vaultVignette\":%u,\"vaultNeedleRed\":%s,"
              "\"vaultNeedleTail\":%s,\"neonLayout\":%u,\"neonFont\":%u,\"neonPreset\":%u,\"demoMode\":%s,\"demoFastSweep\":%s,"
             "\"tpmsBle\":%s,"
             "\"pixelShift\":%s,\"pixelShiftSec\":%u,\"themes\":[",
             cfg.active_theme_id,
             boost_theme_bigdigit_static_bg() ? "true" : "false",
             boost_theme_bigdigit_color_text() ? "true" : "false",
             (unsigned long)boost_theme_bigdigit_static_color(),
             (unsigned long)boost_theme_bigdigit_text_color(),
             boost_theme_arc_gradient() ? "true" : "false",
             boost_theme_hud_gradient() ? "true" : "false",
             boost_theme_hud_true_black() ? "true" : "false",
             boost_theme_neon_marquee_spin() ? "true" : "false",
             boost_theme_te_sync() ? "true" : "false",
             boost_theme_region_dbuf() ? "true" : "false",
             boost_theme_te_scanline() ? "true" : "false",
             (unsigned)boost_theme_rotation(),
             (unsigned long)boost_theme_vault_face(),
             (unsigned)boost_theme_vault_vignette_pct(),
             boost_theme_vault_needle_red() ? "true" : "false",
             boost_theme_vault_needle_tail() ? "true" : "false",
              (unsigned)boost_theme_neon_layout(),
              (unsigned)boost_theme_neon_font(),
              (unsigned)boost_theme_neon_preset(),
             boost_theme_demo_mode() ? "true" : "false",
             boost_sim_fast_sweep() ? "true" : "false",
             boost_theme_tpms_ble() ? "true" : "false",
             boost_theme_pixel_shift() ? "true" : "false",
             (unsigned)boost_theme_pixel_shift_sec());
    for (size_t i = 0; i < boost_theme_count(); ++i) {
        if (i > 0) {
            strlcat(json, ",", cap);
        }
        append_theme_json(json, cap, boost_theme_at(i));
    }
    strlcat(json, "]}", cap);
    /* Serving truncated JSON is worse than serving nothing: the dashboard's
     * parse fails and every theme control disappears with no clue why. */
    if (strlen(json) + 1 >= cap) {
        free(json);
        ESP_LOGE(TAG, "themes payload truncated at %u bytes", (unsigned)cap);
        return send_err(req, HTTPD_500, "themes_truncated");
    }
    const esp_err_t err = send_json(req, json);
    free(json);
    return err;
}

/* Parse "#rrggbb" / "rrggbb". Returns false rather than guessing, so a typo in
 * the dashboard cannot silently paint the gauge black. */
static bool parse_hex_color(const cJSON *item, uint32_t *out)
{
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    const char *p = item->valuestring;
    if (*p == '#') {
        ++p;
    }
    if (strlen(p) != 6) {
        return false;
    }
    char *end = NULL;
    const unsigned long v = strtoul(p, &end, 16);
    if (end == NULL || *end != '\0') {
        return false;
    }
    *out = (uint32_t)v & 0xFFFFFFu;
    return true;
}

static esp_err_t themes_config_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }

    const cJSON *flat = cJSON_GetObjectItemCaseSensitive(root, "bigDigitStaticBg");
    if (cJSON_IsBool(flat)) {
        boost_theme_set_bigdigit_static_bg(cJSON_IsTrue(flat));
    }

    const cJSON *px = cJSON_GetObjectItemCaseSensitive(root, "pixelShift");
    if (cJSON_IsBool(px)) {
        boost_theme_set_pixel_shift(cJSON_IsTrue(px));
    }

    const cJSON *pxsec = cJSON_GetObjectItemCaseSensitive(root, "pixelShiftSec");
    if (cJSON_IsNumber(pxsec)) {
        /* Range-check before the cast, not after: valuedouble can be NaN or
         * 1e300, and narrowing either of those to uint16_t is undefined —
         * the clamp inside boost_theme_set_pixel_shift_sec() would then be
         * clamping a number that never made it across intact. A value the
         * device would have to invent an answer for is a client bug, so it
         * gets a 400 rather than a silent substitution. */
        const double v = pxsec->valuedouble;
        if (!(v >= 1.0 && v <= 86400.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_pixel_shift_sec");
        }
        /* Inside that sane band the theme layer clamps to the supported
         * range; see BOOST_PXSHIFT_SEC_MIN/MAX. */
        boost_theme_set_pixel_shift_sec((uint16_t)(v > 65535.0 ? 65535.0 : v));
    }

    const cJSON *ctext = cJSON_GetObjectItemCaseSensitive(root, "bigDigitColorText");
    if (cJSON_IsBool(ctext)) {
        boost_theme_set_bigdigit_color_text(cJSON_IsTrue(ctext));
    }

    const cJSON *bcol = cJSON_GetObjectItemCaseSensitive(root, "bigDigitStaticColor");
    if (bcol != NULL) {
        uint32_t rgb = 0;
        if (!parse_hex_color(bcol, &rgb)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_color");
        }
        boost_theme_set_bigdigit_static_color(rgb);
    }

    const cJSON *btxc = cJSON_GetObjectItemCaseSensitive(root, "bigDigitTextColor");
    if (btxc != NULL) {
        uint32_t rgb = 0;
        if (!parse_hex_color(btxc, &rgb)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_color");
        }
        boost_theme_set_bigdigit_text_color(rgb);
    }

    const cJSON *ag = cJSON_GetObjectItemCaseSensitive(root, "arcGradient");
    if (cJSON_IsBool(ag)) {
        boost_theme_set_arc_gradient(cJSON_IsTrue(ag));
    }
    const cJSON *hg = cJSON_GetObjectItemCaseSensitive(root, "hudGradient");
    if (cJSON_IsBool(hg)) {
        boost_theme_set_hud_gradient(cJSON_IsTrue(hg));
    }
    const cJSON *hb = cJSON_GetObjectItemCaseSensitive(root, "hudTrueBlack");
    if (cJSON_IsBool(hb)) {
        boost_theme_set_hud_true_black(cJSON_IsTrue(hb));
    }
    const cJSON *nsp = cJSON_GetObjectItemCaseSensitive(root, "neonMarqueeSpin");
    if (cJSON_IsBool(nsp)) {
        boost_theme_set_neon_marquee_spin(cJSON_IsTrue(nsp));
    }

    const cJSON *te = cJSON_GetObjectItemCaseSensitive(root, "teSync");
    if (cJSON_IsBool(te)) {
        const bool on = cJSON_IsTrue(te);
        boost_theme_set_te_sync(on);
        boost_display_set_te(on);
    }

    const cJSON *rdb = cJSON_GetObjectItemCaseSensitive(root, "regionDBuf");
    if (cJSON_IsBool(rdb)) {
        const bool on = cJSON_IsTrue(rdb);
        boost_theme_set_region_dbuf(on);
        boost_display_set_region_dbuf(on);
    }

    const cJSON *tsl = cJSON_GetObjectItemCaseSensitive(root, "teScanline");
    if (cJSON_IsBool(tsl)) {
        const bool on = cJSON_IsTrue(tsl);
        boost_theme_set_te_scanline(on);
        boost_display_set_te_scanline(on);
    }

    /* Quarter turns only - see boost_theme.h for why an arbitrary angle is not
     * on offer. Rejected rather than snapped so a typo is visible. The adapter
     * takes rotation when the display is registered, so this needs a restart;
     * the response carries restartRequired so the dashboard can say so. */
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(root, "rotation");
    if (cJSON_IsNumber(rot)) {
        const double deg = rot->valuedouble;
        if (deg != 0 && deg != 90 && deg != 180 && deg != 270) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_rotation");
        }
        boost_theme_set_rotation((uint16_t)deg);
    }

    /* Flips both the data source and the on-face DEMO text: the gauge/sample
     * paths read boost_theme_demo_mode() every tick, so there is nothing else
     * to poke here. */
    const cJSON *demo = cJSON_GetObjectItemCaseSensitive(root, "demoMode");
    if (cJSON_IsBool(demo)) {
        boost_theme_set_demo_mode(cJSON_IsTrue(demo));
    }

    /* Persisted via the theme store alongside demoMode (NVS "demo_fast_sweep")
     * and re-applied at boot, so the sweep choice survives a reboot. Still a
     * separate flag from demoMode: see boost_theme_set_demo_fast_sweep(). */
    const cJSON *fsweep = cJSON_GetObjectItemCaseSensitive(root, "demoFastSweep");
    if (cJSON_IsBool(fsweep)) {
        boost_theme_set_demo_fast_sweep(cJSON_IsTrue(fsweep));
    }

    /* OBD2 BLE link for the TPMS page. Runtime, persisted, no reboot needed:
     * enable starts the BLE central, disable tears the link down. */
    const cJSON *tble = cJSON_GetObjectItemCaseSensitive(root, "tpmsBle");
    if (cJSON_IsBool(tble)) {
        boost_theme_set_tpms_ble(cJSON_IsTrue(tble));
        boost_obd_set_enabled(cJSON_IsTrue(tble));
    }

    const cJSON *vface = cJSON_GetObjectItemCaseSensitive(root, "vaultFace");
    if (vface != NULL) {
        uint32_t rgb = 0;
        if (!parse_hex_color(vface, &rgb)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_color");
        }
        boost_theme_set_vault_face(rgb);
    }
    const cJSON *vvig = cJSON_GetObjectItemCaseSensitive(root, "vaultVignette");
    if (cJSON_IsNumber(vvig)) {
        const double v = vvig->valuedouble;
        if (!(v >= 0.0 && v <= 90.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_vignette");
        }
        boost_theme_set_vault_vignette_pct((uint8_t)v);
    }
    const cJSON *vred = cJSON_GetObjectItemCaseSensitive(root, "vaultNeedleRed");
    if (cJSON_IsBool(vred)) {
        boost_theme_set_vault_needle_red(cJSON_IsTrue(vred));
    }
    const cJSON *vtail = cJSON_GetObjectItemCaseSensitive(root, "vaultNeedleTail");
    if (cJSON_IsBool(vtail)) {
        boost_theme_set_vault_needle_tail(cJSON_IsTrue(vtail));
    }
    const cJSON *nlay = cJSON_GetObjectItemCaseSensitive(root, "neonLayout");
    if (cJSON_IsNumber(nlay)) {
        const double v = nlay->valuedouble;
        if (!(v >= 0.0 && v <= 2.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_neon_layout");
        }
        boost_theme_set_neon_layout((boost_neon_layout_t)(int)v);
        if (boost_display_lock(1000) == ESP_OK) {
            boost_gauge_apply_theme(boost_model_active_theme());
            boost_display_unlock();
        }
    }

    const cJSON *nfont = cJSON_GetObjectItemCaseSensitive(root, "neonFont");
    if (cJSON_IsNumber(nfont)) {
        const double v = nfont->valuedouble;
        if (!(v == 0.0 || v == 1.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_neon_font");
        }
        boost_theme_set_neon_font((boost_neon_font_t)(int)v);
        if (boost_display_lock(1000) == ESP_OK) {
            boost_gauge_apply_theme(boost_model_active_theme());
            boost_display_unlock();
        }
    }

    const cJSON *np = cJSON_GetObjectItemCaseSensitive(root, "neonPreset");
    if (cJSON_IsNumber(np)) {
        const double v = np->valuedouble;
        /* 0..3: Violet, Miami, Toxic, Blood Moon. */
        if (!(v >= 0.0 && v <= 3.0)) {
            cJSON_Delete(root);
            return send_err(req, HTTPD_400, "invalid_neon_preset");
        }
        boost_theme_set_neon_preset((boost_neon_preset_t)(int)v);
        if (boost_display_lock(1000) == ESP_OK) {
            boost_gauge_apply_theme(boost_model_active_theme());
            boost_display_unlock();
        }
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(id)) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "reset"))) {
            if (!boost_theme_reset_colors(id->valuestring)) {
                cJSON_Delete(root);
                return send_err(req, HTTPD_404, "theme_not_found");
            }
        } else {
            const boost_theme_t *cur = boost_theme_find(id->valuestring);
            if (cur == NULL) {
                cJSON_Delete(root);
                return send_err(req, HTTPD_404, "theme_not_found");
            }
            /* Seed from current so a partial body edits only what it names. */
            boost_theme_colors_t colors = {
                .vacuum = cur->vacuum,
                .boost = cur->boost,
                .overboost = cur->overboost,
            };
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(root, "colors");
            if (cJSON_IsObject(c)) {
                const struct { const char *key; uint32_t *dst; } fields[] = {
                    { "vacuum", &colors.vacuum },
                    { "boost", &colors.boost },
                    { "overboost", &colors.overboost },
                };
                for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
                    const cJSON *item = cJSON_GetObjectItemCaseSensitive(c, fields[i].key);
                    if (item != NULL && !parse_hex_color(item, fields[i].dst)) {
                        cJSON_Delete(root);
                        return send_err(req, HTTPD_400, "invalid_color");
                    }
                }
            }
            boost_theme_set_colors(id->valuestring, &colors);
        }
    }
    cJSON_Delete(root);

    /* Rebuild only if the change can be seen right now. */
    if (boost_display_lock(1000) == ESP_OK) {
        boost_gauge_apply_theme(boost_model_active_theme());
        boost_display_unlock();
    }
    return themes_get(req);
}

static esp_err_t page_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) return send_err(req, HTTPD_400, "invalid_body");
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!cJSON_IsObject(root)) { cJSON_Delete(root); return send_err(req, HTTPD_400, "invalid_json"); }
    int page = -1;
    if (!json_int(root, "page", &page)) json_int(root, "activePage", &page);
    cJSON_Delete(root);
    if (page < 0 || page > 1) return send_err(req, HTTPD_400, "invalid_page");
    esp_err_t err = boost_model_set_active_page(page);
    if (err != ESP_OK) return send_err(req, "503 Service Unavailable", "display_unavailable");
    char json[64];
    snprintf(json, sizeof(json), "{\"ok\":true,\"activePage\":%d}", page);
    return send_json(req, json);
}

static void tpms_config_json(char *json, size_t len)
{
    boost_tpms_config_t cfg;
    boost_tpms_get_config(&cfg);
    snprintf(json, len,
             "{\"lowKpa\":%.1f,\"lowPsi\":%.1f,\"staleAfterMs\":%lu}",
             (double)cfg.low_kpa,
             (double)boost_tpms_protocol_kpa_to_psi(cfg.low_kpa),
             (unsigned long)cfg.stale_after_ms);
}

static esp_err_t tpms_config_get(httpd_req_t *req)
{
    char json[128];
    tpms_config_json(json, sizeof(json));
    return send_json(req, json);
}

static esp_err_t tpms_config_put(httpd_req_t *req)
{
    char *body = read_body(req, MAX_JSON_BODY);
    if (body == NULL) {
        return send_err(req, HTTPD_400, "invalid_body");
    }
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (root == NULL) {
        return send_err(req, HTTPD_400, "invalid_json");
    }
    boost_tpms_config_t cfg;
    boost_tpms_get_config(&cfg);
    bool ok = true;
    float ftmp;
    if (json_float(root, "lowKpa", &ftmp)) {
        if (!(ftmp >= 100.0f && ftmp <= 400.0f)) ok = false;
        else cfg.low_kpa = ftmp;
    } else if (json_float(root, "lowPsi", &ftmp)) {
        const float kpa = ftmp / BOOST_TPMS_KPA_TO_PSI;
        if (!(kpa >= 100.0f && kpa <= 400.0f)) ok = false;
        else cfg.low_kpa = kpa;
    }
    int tmp;
    if (ok && json_int(root, "staleAfterMs", &tmp)) {
        if (!(tmp >= 2000 && tmp <= 120000)) ok = false;
        else cfg.stale_after_ms = (uint32_t)tmp;
    }
    cJSON_Delete(root);
    if (!ok || !boost_tpms_set_config(&cfg)) {
        return send_err(req, HTTPD_400, "invalid_tpms_config");
    }
    /* The TPMS page colors straight from the snapshot each tick, so a new
     * threshold takes effect on the next update with no scene rebuild. */
    char json[128];
    tpms_config_json(json, sizeof(json));
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
    /* A theme is a whole face now, so the panel must rebuild immediately —
     * otherwise the picker only takes effect on the next boot. */
    if (boost_display_lock(1000) == ESP_OK) {
        boost_gauge_apply_theme(boost_model_active_theme());
        boost_display_unlock();
    }
    return themes_get(req);
}

#if LV_USE_SNAPSHOT
/* Debug aid: re-render the live screen into a PSRAM buffer and stream it as
 * raw little-endian RGB565 so the physical face can be inspected off-device.
 * Pairs with tools/fetch_panel_snapshot.py. */
static esp_err_t debug_snapshot_get(httpd_req_t *req)
{
    const uint32_t w = 466;
    const uint32_t h = 466;
    const uint32_t stride = w * 2;
    const size_t bytes = (size_t)stride * h;

    uint8_t *px = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (px == NULL) {
        return send_err(req, HTTPD_500, "snapshot_alloc");
    }

    lv_draw_buf_t db;
    if (lv_draw_buf_init(&db, w, h, LV_COLOR_FORMAT_RGB565, stride, px, bytes) != LV_RESULT_OK) {
        heap_caps_free(px);
        return send_err(req, HTTPD_500, "snapshot_buf");
    }

    lv_result_t res = LV_RESULT_INVALID;
    if (boost_display_lock(3000) == ESP_OK) {
        res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &db);
        boost_display_unlock();
    }
    if (res != LV_RESULT_OK) {
        heap_caps_free(px);
        return send_err(req, HTTPD_500, "snapshot_failed");
    }

    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t send_err_code = ESP_OK;
    const size_t chunk = 8192;
    for (size_t off = 0; off < bytes && send_err_code == ESP_OK; off += chunk) {
        const size_t n = (bytes - off) < chunk ? (bytes - off) : chunk;
        send_err_code = httpd_resp_send_chunk(req, (const char *)(px + off), n);
    }
    if (send_err_code == ESP_OK) {
        send_err_code = httpd_resp_send_chunk(req, NULL, 0);
    }
    heap_caps_free(px);
    return send_err_code;
}
#endif

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
        int offset_min = current.timezone_offset_minutes;
        if (epoch_ms > 0) {
            const time_t seconds = (time_t)(epoch_ms / 1000LL);
            struct tm local = {0};
            if (localtime_r(&seconds, &local) != NULL) {
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", &local);
                offset_min = boost_model_utc_offset_minutes_at(seconds);
            }
        }
        snprintf(row, sizeof(row), "%s,%d,%lld,%lu,%.2f,%.2f,%s,%d\n",
                 timestamp, offset_min, (long long)epoch_ms,
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
/* OTA sets the boot partition but cannot take effect until the device reboots,
 * and without this the only way to finish a remote update was to power-cycle
 * the panel by hand. Deferred so the response is actually delivered first. */
static void restart_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting on API request");
    esp_restart();
}

static esp_err_t restart_post(httpd_req_t *req)
{
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = restart_timer_cb,
            .name = "api_restart",
        };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            return send_err(req, HTTPD_500, "restart_failed");
        }
    }
    const esp_err_t err = esp_timer_start_once(timer, 400 * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return send_err(req, HTTPD_500, "restart_failed");
    }
    return send_json(req, "{\"ok\":true,\"restartingInMs\":400}");
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
    if (asset == NULL && strcmp(uri, "/index.html") == 0) asset = boost_web_asset_find("/");
    if (asset != NULL) {
        httpd_resp_set_type(req, asset->content_type);
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        httpd_resp_set_hdr(req, "ETag", asset->etag);
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, (const char *)asset->gzip_data, asset->gzip_size);
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
    int written = snprintf(json, len,
             "{\"mode\":\"%s\",\"staEnabled\":%s,\"staConnected\":%s,"
             "\"staSsid\":\"%s\",\"staIp\":\"%s\",\"apSsid\":\"%s\",\"apIp\":\"%s\","
             "\"rssi\":%d,\"hasPassword\":%s,\"saved\":[",
             st.mode == BOOST_NET_MODE_APSTA ? "apsta" : "ap",
             st.sta_enabled ? "true" : "false",
             st.sta_connected ? "true" : "false",
             ssid_e, st.sta_ip, ap_e, st.ap_ip, st.rssi,
             st.has_sta_pass ? "true" : "false");
    if (written < 0 || (size_t)written >= len) {
        return;
    }
    for (uint8_t i = 0; i < st.saved_count; ++i) {
        char s_esc[96];
        json_escape(st.saved[i].ssid, s_esc, sizeof(s_esc));
        char item[128];
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\"}", i == 0 ? "" : ",", s_esc);
        strlcat(json, item, len);
    }
    strlcat(json, "]}", len);
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

static esp_err_t network_delete(httpd_req_t *req)
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
    cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (cJSON_IsString(ssid_j)) {
        ssid = ssid_j->valuestring;
    }
    if (!ssid || ssid[0] == '\0') {
        cJSON_Delete(root);
        return send_err(req, HTTPD_400, "missing_ssid");
    }
    esp_err_t err = boost_network_delete_saved(ssid);
    cJSON_Delete(root);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_err(req, HTTPD_404, "not_found");
    } else if (err != ESP_OK) {
        return send_err(req, HTTPD_400, "delete_failed");
    }
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

/* Per-connection socket setup. esp_http_server writes a response in many small
 * send() calls - roughly one per HTTP header token, and two per WebSocket frame
 * (header, then payload) - so Nagle holds every fragment after the first until
 * the peer ACKs. Measured on this device: first 70 B at 2.6 ms, remaining 459 B
 * at 45-50 ms. TCP_NODELAY removes that stall. IDF only applies TCP_NODELAY
 * transiently around error responses, never to normal responses or WS frames,
 * and .open_fn is the only per-connection hook available. */
static esp_err_t socket_open_fn(httpd_handle_t hd, int sockfd)
{
    (void)hd;
    const int one = 1;
    if (setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) < 0) {
        ESP_LOGW(TAG, "TCP_NODELAY failed on fd %d (errno %d); latency will suffer",
                 sockfd, errno);
    }
    /* Never fail the connection over a socket option. */
    return ESP_OK;
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
    /* Headroom matters more than the few bytes it costs: register_uri() wraps
     * httpd_register_uri_handler() in ESP_ERROR_CHECK, so overflowing this
     * aborts during boot rather than degrading. The panel runs from 5 V with no
     * serial attached, and a boot abort precedes the HTTP server that OTA needs,
     * making that failure unrecoverable without pulling the dash. Keep a wide
     * margin over the registration count below. */
    cfg.max_uri_handlers = 40;
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 90;
    cfg.send_wait_timeout = 5;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    /* Disable Nagle per connection; see socket_open_fn. */
    cfg.open_fn = socket_open_fn;
    /* Keep HTTP off core 0, which carries the Wi-Fi driver task, the sample
     * task and (unpinned) the LVGL worker. Priority is deliberately left at the
     * default 5, below the LVGL adapter's 6 - reordering those risks display
     * stutter, whereas pinning is the safe lever. */
    cfg.core_id = 1;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd");
    register_uri(API_BASE "/state", HTTP_GET, state_get);
    register_uri(API_BASE "/sensors/scan", HTTP_GET, sensors_scan_get);
    register_uri(API_BASE "/sensors/calibration", HTTP_GET, sensors_calibration_get);
    register_uri(API_BASE "/sensors/calibration", HTTP_POST, sensors_calibration_post);
    register_uri(API_BASE "/sensors/supply", HTTP_PUT, sensors_supply_put);
    register_websocket_uri(WS_STATE_PATH, state_ws_get);
    register_uri(API_BASE "/config", HTTP_GET, config_get);
    register_uri(API_BASE "/config", HTTP_PUT, config_put);
    register_uri(API_BASE "/time", HTTP_POST, time_post);
    register_uri(API_BASE "/themes", HTTP_GET, themes_get);
    register_uri(API_BASE "/themes/active", HTTP_PUT, theme_active_put);
    register_uri(API_BASE "/themes/config", HTTP_PUT, themes_config_put);
    register_uri(API_BASE "/tpms/config", HTTP_GET, tpms_config_get);
    register_uri(API_BASE "/tpms/config", HTTP_PUT, tpms_config_put);
    register_uri(API_BASE "/page", HTTP_PUT, page_put);
    register_uri(API_BASE "/logs", HTTP_GET, logs_get);
    register_uri(API_BASE "/logs", HTTP_DELETE, logs_delete);
    register_uri(API_BASE "/logs.csv", HTTP_GET, logs_csv_get);
    register_uri(API_BASE "/media", HTTP_POST, media_post);
    register_uri(API_BASE "/media", HTTP_DELETE, media_delete);
    register_uri(API_BASE "/media/status", HTTP_GET, media_status_get);
    register_uri(API_BASE "/ota", HTTP_POST, ota_post);
    register_uri(API_BASE "/restart", HTTP_POST, restart_post);
    register_uri(API_BASE "/network", HTTP_GET, network_get);
    register_uri(API_BASE "/network", HTTP_PUT, network_put);
    register_uri(API_BASE "/network", HTTP_DELETE, network_delete);
    register_uri(API_BASE "/network/reconnect", HTTP_POST, network_reconnect_post);
    register_uri(API_BASE "/network/scan", HTTP_GET, network_scan_get);
#if LV_USE_SNAPSHOT
    register_uri(API_BASE "/debug/snapshot", HTTP_GET, debug_snapshot_get);
#endif
    register_uri("/*", HTTP_GET, root_get);
    register_uri("/*", HTTP_OPTIONS, options_handler);
    ESP_LOGI(TAG, "HTTP API ready");
    /* Co-located with the httpd task on core 1: it does the JSON render and the
     * malloc/free per frame, and hands straight off to httpd's async work queue. */
    /* Priority 2, below LVGL's swdraw threads at 4: at equal priority these two
     * round-robin, and since this task now wakes on every sample it would steal
     * slices from pixel rasterisation 62 times a second. */
    if (xTaskCreatePinnedToCore(state_ws_task, "boost_ws", 4096, NULL, 3,
                                (TaskHandle_t *)&s_state_ws_task, 1) != pdPASS) {
        ESP_LOGW(TAG, "live WebSocket task not started");
    }
    return ESP_OK;
}
