#include "boost_obd.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "boost_obd_ble.h"
#include "boost_obd_elm.h"
#include "boost_model.h"
#include "boost_tpms.h"
#include "boost_tpms_protocol.h"

static const char *TAG = "boost_obd";

#define OBD_STALE_MS      15000u  /* web readout considered stale after this */
#define OBD_HEADER_TPMS   "720"   /* Mazda MX-5 ND TPMS module diagnostic ID */
#define OBD_HEADER_ECU    "7DF"   /* ISO 15765-4 functional request (mode 01) */
#define OBD_SETTLE_MS     20      /* vLinker serial settle after each reply */

static SemaphoreHandle_t s_lock;          /* guards s_state */
static TaskHandle_t s_task;
static volatile bool s_enabled;

static boost_obd_state_t s_state;
static float s_last_rpm;
static float s_last_speed;
static float s_last_coolant;
static float s_last_map;
static float s_last_iat;
static float s_last_throttle;
static float s_last_maf;
static float s_last_fuel;
static float s_last_battery;

/* --- decoders ------------------------------------------------------------ */

static float dec_temp(const uint8_t *d, size_t n) { return n >= 1 ? (float)d[0] - 40.0f : NAN; }
static float dec_kpa(const uint8_t *d, size_t n)  { return n >= 1 ? (float)d[0] : NAN; }
static float dec_rpm(const uint8_t *d, size_t n)
{
    return n >= 2 ? ((float)((d[0] << 8) | d[1])) / 4.0f : NAN;
}
static float dec_speed(const uint8_t *d, size_t n) { return n >= 1 ? (float)d[0] : NAN; }
static float dec_pct(const uint8_t *d, size_t n)  { return n >= 1 ? (float)d[0] * 100.0f / 255.0f : NAN; }
static float dec_gps(const uint8_t *d, size_t n)
{
    return n >= 2 ? ((float)((d[0] << 8) | d[1])) / 100.0f : NAN;
}

typedef struct {
    uint8_t pid;
    const char *cmd;
    float *slot;
    float (*dec)(const uint8_t *d, size_t n);
} obd_pid_def_t;

static obd_pid_def_t s_pids[] = {
    { 0x0C, "010C", &s_last_rpm,      dec_rpm },
    { 0x0D, "010D", &s_last_speed,    dec_speed },
    { 0x05, "0105", &s_last_coolant,  dec_temp },
    { 0x0B, "010B", &s_last_map,      dec_kpa },
    { 0x0F, "010F", &s_last_iat,      dec_temp },
    { 0x11, "0111", &s_last_throttle, dec_pct },
    { 0x10, "0110", &s_last_maf,      dec_gps },
    { 0x2F, "012F", &s_last_fuel,     dec_pct },
};
#define OBD_PID_COUNT (sizeof(s_pids) / sizeof(s_pids[0]))

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ELM replies may carry spaces, stray CR/LF or vendor noise; decode only the
 * hex nibbles and skip everything else. */
static size_t hex_to_bytes(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    int hi = -1;
    for (; *s != '\0' && n < max; ++s) {
        const int v = hex_val(*s);
        if (v < 0) continue;
        if (hi < 0) {
            hi = v;
        } else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    return n;
}

static void bytes_to_hex(const uint8_t *b, size_t n, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = digits[b[i] >> 4];
        out[i * 2 + 1] = digits[b[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

static bool find_mode_bytes(const uint8_t *b, size_t n, uint8_t mode, uint8_t pid,
                            const uint8_t **data, size_t *data_len)
{
    for (size_t i = 0; i + 2 <= n; ++i) {
        if (b[i] == mode && b[i + 1] == pid) {
            *data = &b[i + 2];
            *data_len = n - i - 2;
            return true;
        }
    }
    return false;
}

static bool reply_is_error(const char *reply)
{
    return strstr(reply, "NO DATA") != NULL || strstr(reply, "CAN ERROR") != NULL ||
           strstr(reply, "UNABLE") != NULL || strstr(reply, "BUS ERROR") != NULL ||
           strstr(reply, "?") != NULL;
}

static bool query_pid(const obd_pid_def_t *def, uint32_t timeout_ms)
{
    char reply[96];
    if (!boost_obd_elm_request(def->cmd, reply, sizeof(reply), timeout_ms)) return false;
    if (reply[0] == '\0' || reply_is_error(reply)) {
        ESP_LOGI(TAG, "PID %s -> '%s'", def->cmd, reply);
        return false;
    }

    uint8_t bytes[64];
    const size_t n = hex_to_bytes(reply, bytes, sizeof(bytes));
    const uint8_t *data;
    size_t data_len = 0;
    if (!find_mode_bytes(bytes, n, 0x41, def->pid, &data, &data_len)) {
        ESP_LOGI(TAG, "PID %s: undecoded '%s'", def->cmd, reply);
        return false;
    }

    const float v = def->dec(data, data_len);
    if (isnan(v)) return false;
    *def->slot = v;
    return true;
}

static bool query_battery(uint32_t timeout_ms)
{
    char reply[32];
    if (!boost_obd_elm_request("ATRV", reply, sizeof(reply), timeout_ms)) return false;
    if (reply[0] == '\0' || reply_is_error(reply)) return false;
    char *end = NULL;
    const float v = strtof(reply, &end);
    if (end == reply) return false;
    s_last_battery = v;
    return true;
}

static bool query_tpms_did(uint16_t did, uint16_t *out_raw, uint32_t timeout_ms)
{
    uint8_t req[3];
    if (!boost_tpms_protocol_make_read_did(did, req)) return false;
    char cmd[8];
    bytes_to_hex(req, sizeof(req), cmd);

    char reply[96];
    if (!boost_obd_elm_request(cmd, reply, sizeof(reply), timeout_ms)) return false;
    if (reply[0] == '\0' || reply_is_error(reply)) {
        ESP_LOGI(TAG, "DID %04X -> '%s'", did, reply);
        return false;
    }

    uint8_t bytes[64];
    const size_t n = hex_to_bytes(reply, bytes, sizeof(bytes));
    uint16_t rdid = 0, raw = 0;
    if (!boost_tpms_protocol_parse_uds_response(bytes, n, &rdid, &raw)) return false;
    if (rdid != did) return false;
    *out_raw = raw;
    return true;
}

/* --- state publication ---------------------------------------------------- */

static void publish_state(void)
{
    const boost_obd_ble_state_t bst = boost_obd_ble_state();
    const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
    boost_obd_state_t st;
    memset(&st, 0, sizeof(st));
    st.uptime_ms = boost_obd_ble_uptime_ms();
    st.last_error = boost_obd_ble_last_error();
    strlcpy(st.peer, boost_obd_ble_peer_name(), sizeof(st.peer));
    strlcpy(st.peer_addr, boost_obd_ble_peer_addr(), sizeof(st.peer_addr));
    switch (bst) {
    case BOOST_OBD_BLE_READY: st.state = 3; break;
    case BOOST_OBD_BLE_SCANNING: st.state = 1; break;
    case BOOST_OBD_BLE_CONNECTING:
    case BOOST_OBD_BLE_DISCOVERING: st.state = 2; break;
    case BOOST_OBD_BLE_DISCONNECTED: st.state = 4; break;
    default: st.state = 0; break;
    }

    const uint32_t elm_last = boost_obd_elm_last_reply_ms();
    const uint32_t age = (elm_last != 0 && now_ms >= elm_last) ? now_ms - elm_last : 0;
    st.age_ms = age;
    st.valid = elm_last != 0 && age <= OBD_STALE_MS;
    st.rpm = s_last_rpm;
    st.speed_kph = s_last_speed;
    st.coolant_c = s_last_coolant;
    st.map_kpa = s_last_map;
    st.iat_c = s_last_iat;
    st.throttle_pct = s_last_throttle;
    st.maf_gps = s_last_maf;
    st.fuel_pct = s_last_fuel;
    st.battery_v = s_last_battery;

    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = st;
    if (s_lock != NULL) xSemaphoreGive(s_lock);
    boost_model_publish_obd(&st);
}

/* --- poll loop ------------------------------------------------------------ */

static void poll_task(void *arg)
{
    (void)arg;
    const uint16_t dids[4] = {
        BOOST_TPMS_DID_FL, BOOST_TPMS_DID_FR, BOOST_TPMS_DID_RL, BOOST_TPMS_DID_RR,
    };
    uint16_t pending_raw[4] = { 0, 0, 0, 0 };
    uint8_t pending_mask = 0;
    unsigned init_idx = 0;
    unsigned pid_idx = 0;
    unsigned tpms_idx = 0;
    bool elm_ready = false;
    bool header_is_tpms = false;   /* true when the ELM header is set to 0x720 */
    unsigned phase = 0;            /* 0 = TPMS DID, 1 = mode-01 PID */

    for (;;) {
        if (!s_enabled) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (boost_obd_ble_state() != BOOST_OBD_BLE_READY) {
            elm_ready = false;
            init_idx = 0;         /* re-run the ELM init on the next link */
            header_is_tpms = false;
            pending_mask = 0;
            boost_obd_elm_reset();
            publish_state();
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }

        if (!elm_ready) {
            static const struct { const char *cmd; uint32_t tmo; } k_init[] = {
                { "ATZ", 2500 }, { "ATE0", 500 }, { "ATL0", 500 },
                { "ATS0", 500 }, { "ATH0", 500 }, { "ATSP0", 1000 },
            };
            if (init_idx < sizeof(k_init) / sizeof(k_init[0])) {
                char reply[96];
                const bool ok = boost_obd_elm_request(k_init[init_idx].cmd, reply,
                                                      sizeof(reply), k_init[init_idx].tmo);
                vTaskDelay(pdMS_TO_TICKS(OBD_SETTLE_MS));
                if (!ok) {
                    ESP_LOGW(TAG, "init step '%s' failed; re-arming", k_init[init_idx].cmd);
                    init_idx = 0;
                    boost_obd_elm_reset();
                    vTaskDelay(pdMS_TO_TICKS(500));
                } else {
                    init_idx++;
                }
                continue;
            }
            elm_ready = true;
            ESP_LOGI(TAG, "ELM init complete");
            continue;
        }

        char reply[96];
        const bool want_tpms_header = (phase == 0);
        if (want_tpms_header != header_is_tpms) {
            /* Explicitly set the header for each phase. A bare "ATSH" reset is
             * not reliable on the FD+ - it leaves the header at the previous
             * value, so mode-01 PIDs would be sent to 0x720 (the TPMS module)
             * and never answered. */
            const char *hc = want_tpms_header ? "ATSH " OBD_HEADER_TPMS
                                              : "ATSH " OBD_HEADER_ECU;
            if (boost_obd_elm_request(hc, reply, sizeof(reply), 500)) {
                header_is_tpms = want_tpms_header;
            }
            vTaskDelay(pdMS_TO_TICKS(OBD_SETTLE_MS));
            continue; /* the header switch owns this iteration */
        }

        bool ok;
        if (phase == 0) {
            ok = query_tpms_did(dids[tpms_idx], &pending_raw[tpms_idx], 2000);
        } else {
            ok = (pid_idx == OBD_PID_COUNT) ? query_battery(2000)
                                            : query_pid(&s_pids[pid_idx], 2000);
        }

        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
        if (ok) {
            if (phase == 0) {
                pending_mask |= (uint8_t)(1u << tpms_idx);
                if (pending_mask == 0x0F) {
                    pending_mask = 0;
                    boost_tpms_publish_raw(now_ms, pending_raw);
                }
            }
        }
        /* Always advance so a permanently failing DID/PID cannot starve the
         * rest of the rotation. A DID that never answers simply never sets its
         * mask bit, so the TPMS service ages to STALE rather than showing a
         * fabricated value. */
        if (phase == 0) {
            tpms_idx = (tpms_idx + 1u) % 4u;
            phase = 1;
        } else {
            if (pid_idx == OBD_PID_COUNT) pid_idx = 0;
            else pid_idx++;
            phase = 0;
        }

        publish_state();
        vTaskDelay(pdMS_TO_TICKS(OBD_SETTLE_MS + 60));
    }
}

/* --- public API ----------------------------------------------------------- */

void boost_obd_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    boost_obd_elm_init();
    if (xTaskCreate(poll_task, "boost_obd", 4096, NULL, 4, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "poll task create failed");
    }
    ESP_LOGI(TAG, "OBD driver idle (enabled=%d)", s_enabled ? 1 : 0);
}

void boost_obd_set_enabled(bool enabled)
{
    if (enabled == s_enabled) return;
    s_enabled = enabled;
    if (enabled) {
        ESP_LOGI(TAG, "enabling BLE OBD link");
        boost_obd_ble_start();
    } else {
        ESP_LOGI(TAG, "disabling BLE OBD link");
        boost_obd_ble_stop();
    }
    publish_state();
}

bool boost_obd_enabled(void) { return s_enabled; }

void boost_obd_get_state(boost_obd_state_t *out)
{
    if (out == NULL) return;
    if (s_lock != NULL) xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    if (s_lock != NULL) xSemaphoreGive(s_lock);
}
