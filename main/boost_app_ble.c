#include "boost_app_ble.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "boost_display.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_network.h"
#include "boost_obd_ble.h"
#include "boost_sensors.h"
#include "boost_sim.h"
#include "boost_theme.h"
#include "boost_tpms.h"
#include "boost_json.h"
#include "boost_tpms_protocol.h"

/*
 * Dual-role companion GATT server. Threading follows the same shape as the
 * OBD central in boost_obd_ble.c: the NimBLE host task owns its event loop,
 * while every GAP/advertising call and every GATT notification made by THIS
 * module is serialized on the single "boost_app_ble" driver task through a
 * fixed FIFO. GATT access callbacks (read/write Control, read Log/Status/
 * Device-Info) run on the NimBLE host task, which the spec accepts; they do
 * no GAP work and enqueue responses instead of calling the stack directly
 * (the one exception is the Log and Control read caches, which are host-task
 * buffers consumed synchronously by the same host task).
 *
 * The service set is registered BEFORE the shared host task starts (see
 * boost_app_ble_init() and main.c), so ble_hs_start() registers it
 * authoritatively - there is no runtime service mutation and therefore no
 * race with a central connection.
 */

/* Build the BLE-only implementation whenever the shared NimBLE host path is
 * compiled in. With BOOST_TPMS_BLE_ENABLED=n or BT disabled the firmware is a
 * BLE-less image (the same modes the OBD central supports); the public API
 * then degrades to inert stubs so main.c/boost_web.c need no #ifdefs. */
#if CONFIG_BT_ENABLED && CONFIG_BT_NIMBLE_ENABLED && CONFIG_BOOST_TPMS_BLE_ENABLED
#define APP_BLE_BUILT 1
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_id.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_l2cap.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#else
#define APP_BLE_BUILT 0
#endif

#if CONFIG_BT_NIMBLE_NVS_PERSIST
/* NimBLE's store/config package omits the init from its public header; the
 * ESP-IDF sample apps forward-declare it the same way. Wired to the persisted
 * bond store so LE-SC pairings survive reboots (companion-app GATT contract). */
void ble_store_config_init(void);
#endif

#if APP_BLE_BUILT

static const char *TAG = "boost_app_ble";

/* Control request/response cap from the companion-app GATT spec (480 B
 * payload). Oversize requests/responses are answered with status 413
 * {"error":"too_large"}. */
#define APP_BLE_CTRL_MAX      480u
/* Responses mirror the HTTP JSON (full /themes ~1.7 KB, full /state ~1 KB),
 * so a response is not bounded by the 480-byte request cap. The response is
 * still fragmented to the ATT MTU on the wire and reassembled by the client;
 * 4096 keeps every current HTTP payload plus headroom. */
#define APP_BLE_CTRL_RESP_MAX 4096u
/* Preferred ATT MTU: 480-byte notifications need MTU >= 483. 517 is the
 * conventional "512-byte value" size and stays under BLE_ATT_MTU_MAX (527). */
#define APP_BLE_MTU           256u // Intel hackintosh (NUC8) wedges on 517 during SC MTU exchange; 256 is iOS-safe and Intel-stable
/* Advertising interval: 160-250 ms in 0.625 ms units. */
#define APP_BLE_ADV_ITVL_MIN  256u
#define APP_BLE_ADV_ITVL_MAX  400u
#define APP_BLE_ADV_RETRY_MS  250u
#define APP_BLE_ADV_RETRY_MAX 10u
/* One status sample must fit a single notification after the 480-byte rule;
 * the /state-shaped mirror is deliberately compact. */
#define APP_BLE_STATUS_BUF    1280u

#define APP_BLE_NAME         "BoostGauge"

#define APP_BLE_QUEUE_LEN    8
#define APP_BLE_TASK_STACK   6144
#define APP_BLE_TASK_PRIO    6

/* Persistence mirrors tpmsBle: same NVS namespace ("boost"), same u8/0-or-1
 * format, default OFF. It lives in this module because boost_theme.c is not
 * part of this change's file set. */
#define APP_NVS_NS           "boost"
#define APP_NVS_KEY          "app_ble"

/* Service b6a00000-0000-4000-8000-00000000b6a0 and characteristics
 * b6a00001..b6a00004, as full 128-bit little-endian byte arrays (the NimBLE
 * ble_uuid128_t convention, verified against ble_uuid_to_str()/known
 * examples). */
static const ble_uuid128_t s_uuid_svc =
    BLE_UUID128_INIT(0xa0, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0xa0, 0xb6);
static const ble_uuid128_t s_uuid_control =
    BLE_UUID128_INIT(0xa0, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x00, 0x00, 0x01, 0x00, 0xa0, 0xb6);
static const ble_uuid128_t s_uuid_status =
    BLE_UUID128_INIT(0xa0, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x00, 0x00, 0x02, 0x00, 0xa0, 0xb6);
static const ble_uuid128_t s_uuid_log =
    BLE_UUID128_INIT(0xa0, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x00, 0x00, 0x03, 0x00, 0xa0, 0xb6);
static const ble_uuid128_t s_uuid_dev_info =
    BLE_UUID128_INIT(0xa0, 0xb6, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80,
                     0x00, 0x40, 0x00, 0x00, 0x04, 0x00, 0xa0, 0xb6);

typedef enum {
    APP_EV_START = 1,
    APP_EV_STOP,
    APP_EV_CONNECTED,
    APP_EV_DISCONNECTED,
    APP_EV_SUBSCRIBE,
    APP_EV_TX,          /* heap response payload (owned) */
    APP_EV_CALIBRATE,   /* async /sensors/calibration POST (blocks ~2 s) */
    APP_EV_LOGS,        /* async /logs GET (copy+JSON, must not block host) */
    APP_EV_SCAN,        /* async /network/scan GET (Wi-Fi scan blocks for seconds) */
    APP_EV_ADV_RETRY,
} app_ble_ev_type_t;

typedef struct {
    uint8_t type;
    uint16_t conn_handle;
    uint16_t attr_handle;
    uint8_t cur_notify;
    uint32_t id;
    char *payload;       /* heap-owned for APP_EV_TX */
    size_t logs_limit;   /* for APP_EV_LOGS */
} app_ble_ev_t;

typedef struct {
    const char *path;
    const char *method;
    int (*handle)(const cJSON *body, char *out, size_t cap);
} app_ble_route_t;

static QueueHandle_t s_evq;
static TaskHandle_t s_task;
static bool s_init_done;
static bool s_enabled;              /* persisted toggle (default off) */
static bool s_want_adv;             /* live "should advertise" state */
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_ctl_subscribed;
/* Negotiated ATT MTU for the connected phone (default = spec minimum). */
static uint16_t s_att_mtu = 23;

/* Handles filled by the GATT registration callback. */
static uint16_t s_ctl_val_handle;
static uint16_t s_status_val_handle;
static uint16_t s_log_val_handle;
static uint16_t s_dev_info_val_handle;

static char s_device_info[128];
static char s_ble_sta_ip[32] = "";
static size_t s_pending_logs_limit = 0;

static void app_device_info_rebuild(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char ip_part[48];
    if (s_ble_sta_ip[0] != '\0') {
        snprintf(ip_part, sizeof(ip_part), ",\"ip\":\"%s\"", s_ble_sta_ip);
    } else {
        ip_part[0] = '\0';
    }
    snprintf(s_device_info, sizeof(s_device_info),
             "{\"name\":\"BoostGauge\",\"fw\":\"%s\",\"api\":1%s}",
             desc != NULL ? desc->version : "unknown", ip_part);
}

void boost_app_ble_set_sta_ip(const char *ip)
{
    if (ip != NULL) {
        snprintf(s_ble_sta_ip, sizeof(s_ble_sta_ip), "%s", ip);
    }
    app_device_info_rebuild();
}

/* Per-connection log CSV cache ("generate on demand into a heap buffer").
 * Built by the first Log read of a connection and freed on disconnect. */
static char *s_log_cache;
static size_t s_log_cache_len;
static uint16_t s_log_cache_conn;

static volatile bool s_host_synced;

static void app_ble_register_host_config(void);
static void app_ble_on_sync(void);

static void app_driver_task(void *arg);
static int app_gap_event(struct ble_gap_event *event, void *arg);

/* --- persistence -------------------------------------------------------- */

static bool app_ble_nvs_read_enabled(void)
{
    bool enabled = false;
    nvs_handle_t h;
    if (nvs_open(APP_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, APP_NVS_KEY, &v) == ESP_OK) {
        enabled = (v != 0);
    }
    nvs_close(h);
    return enabled;
}

static void app_ble_nvs_persist(bool enabled)
{
    nvs_handle_t h;
    if (nvs_open(APP_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, APP_NVS_KEY, enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

/* --- response helpers ---------------------------------------------------- */

/* Build the full Control notification payload {"id":...,"status":...,"body":...}.
 * The response body may exceed the 480-byte request cap (it is fragmented to
 * the ATT MTU and reassembled by the client); only the request write path is
 * bounded by APP_BLE_CTRL_MAX. Heap-owned; caller frees. */
static char *app_ble_make_response(uint32_t id, int status, const char *body)
{
    const size_t body_len = body != NULL ? strlen(body) : 0;
    const size_t need = 64 + body_len;   /* envelope scratch */
    char *tmp = malloc(need);
    if (tmp == NULL) {
        return NULL;
    }
    int n = snprintf(tmp, need, "{\"id\":%lu,\"status\":%d,\"body\":%s}",
                     (unsigned long)id, status, body != NULL ? body : "{}");
    if (n < 0 || (size_t)n >= need) {
        free(tmp);
        tmp = NULL;
    }
    if (tmp != NULL && (size_t)n > APP_BLE_CTRL_RESP_MAX) {
        free(tmp);
        n = snprintf(NULL, 0, "{\"id\":%lu,\"status\":413,\"body\":{\"error\":\"too_large\"}}",
                     (unsigned long)id);
        tmp = malloc((size_t)n + 1U);
        if (tmp != NULL) {
            snprintf(tmp, (size_t)n + 1U,
                     "{\"id\":%lu,\"status\":413,\"body\":{\"error\":\"too_large\"}}",
                     (unsigned long)id);
        }
    }
    return tmp;
}

static void app_ble_enqueue_tx(uint16_t conn_handle, char *payload)
{
    if (payload == NULL) {
        return;
    }
    app_ble_ev_t ev = { 0 };
    ev.type = APP_EV_TX;
    ev.conn_handle = conn_handle;
    ev.payload = payload;
    if (s_evq == NULL || xQueueSend(s_evq, &ev, 0) != pdTRUE) {
        free(payload);
    }
}

/* Send one logical GATT message as consecutive notification packets of at
 * most ATT_MTU-3 payload bytes. The companion-app GATT contract (and the
 * iOS 185-byte MTU cap) require the peripheral to fragment across packets;
 * NimBLE delivers each ble_gatts_notify_custom() call as one ATT
 * notification, so a 480-byte response on a small MTU needs this loop.
 * Runs only on the driver task, so packets stay serialized per connection. */
static void app_notify_fragmented(uint16_t conn_handle, uint16_t attr_handle,
                                  const char *data, size_t len)
{
    uint16_t chunk = (s_att_mtu >= 3U) ? (uint16_t)(s_att_mtu - 3U) : 20U;
    size_t off = 0;
    while (off < len) {
        size_t n = len - off;
        if (n > chunk) {
            n = chunk;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data + off, (uint16_t)n);
        if (om == NULL) {
            ESP_LOGW(TAG, "notify mbuf alloc failed (attr 0x%04x)", attr_handle);
            return;
        }
        if (ble_gatts_notify_custom(conn_handle, attr_handle, om) != 0) {
            ESP_LOGW(TAG, "notify failed (attr 0x%04x)", attr_handle);
            return;
        }
        off += n;
        if (off < len) {
            /* Keep arrival order clean on the phone and leave the host queue
             * room to breathe between packets. */
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

/* --- JSON building (mirrors boost_web.c shapes) -------------------------- */

static bool app_json_int(const cJSON *obj, const char *name, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = v->valueint;
    return true;
}

static bool app_json_float(const cJSON *obj, const char *name, float *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    *out = (float)v->valuedouble;
    return true;
}

static bool app_parse_hex_color(const cJSON *item, uint32_t *out)
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



/* --- route handlers (mirror boost_web.c semantics) ----------------------- */

static int route_state(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    /* Full /state mirror (tpms/sensors/display/obd): responses are no longer
     * capped at 480 bytes — they fragment to the ATT MTU and the client
     * reassembles, so the Control route matches the Status characteristic. */
    const int n = boost_json_state(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_config_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const int n = boost_json_config(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_config_put(const cJSON *body, char *out, size_t cap)
{
    if (body == NULL) {
        snprintf(out, cap, "{\"error\":\"invalid_json\"}");
        return 400;
    }
    boost_config_t patch;
    boost_model_get_config(&patch);
    uint32_t fields = 0;
    int tmp;
    float ftmp;
    if (app_json_int(body, "brightnessHigh", &tmp)) {
        patch.brightness_high = tmp;
        fields |= BOOST_CONFIG_BRIGHTNESS_HIGH;
    }
    if (app_json_int(body, "brightnessLow", &tmp)) {
        patch.brightness_low = tmp;
        fields |= BOOST_CONFIG_BRIGHTNESS_LOW;
    }
    if (app_json_int(body, "timezoneOffsetMinutes", &tmp)) {
        patch.timezone_offset_minutes = tmp;
        fields |= BOOST_CONFIG_TZ_OFFSET;
    }
    const cJSON *tzstr = cJSON_GetObjectItemCaseSensitive(body, "timezoneTz");
    if (cJSON_IsString(tzstr)) {
        strlcpy(patch.timezone_tz, tzstr->valuestring, sizeof(patch.timezone_tz));
        fields |= BOOST_CONFIG_TZ_TZ;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "activeThemeId");
    if (cJSON_IsString(id)) {
        strlcpy(patch.active_theme_id, id->valuestring, sizeof(patch.active_theme_id));
        fields |= BOOST_CONFIG_THEME;
    }
    const cJSON *sched = cJSON_GetObjectItemCaseSensitive(body, "dimSchedule");
    if (cJSON_IsObject(sched)) {
        const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(sched, "enabled");
        if (cJSON_IsBool(enabled)) {
            patch.dim_schedule.enabled = cJSON_IsTrue(enabled);
            fields |= BOOST_CONFIG_DIM_ENABLED;
        }
        if (app_json_int(sched, "startMinutes", &tmp)) {
            patch.dim_schedule.start_minutes = tmp;
            fields |= BOOST_CONFIG_DIM_START;
        }
        if (app_json_int(sched, "endMinutes", &tmp)) {
            patch.dim_schedule.end_minutes = tmp;
            fields |= BOOST_CONFIG_DIM_END;
        }
    }
    if (app_json_float(body, "psiMin", &ftmp)) {
        patch.psi_min = ftmp;
        fields |= BOOST_CONFIG_PSI_MIN;
    }
    if (app_json_float(body, "psiMax", &ftmp)) {
        patch.psi_max = ftmp;
        fields |= BOOST_CONFIG_PSI_MAX;
    }
    if (app_json_float(body, "psiOverboost", &ftmp)) {
        patch.psi_overboost = ftmp;
        fields |= BOOST_CONFIG_PSI_OVERBOOST;
    }
    if (app_json_float(body, "zeroAngle", &ftmp)) {
        patch.zero_angle = ftmp;
        fields |= BOOST_CONFIG_ZERO_ANGLE;
    }
    const cJSON *able = cJSON_GetObjectItemCaseSensitive(body, "appBle");
    if (cJSON_IsBool(able)) {
        boost_app_ble_set_enabled(cJSON_IsTrue(able));
    }
    if (boost_model_update_config(&patch, fields) != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"invalid_config\"}");
        return 400;
    }
    /* Mirror config_put(): rebuild the live scene for what changed. */
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
    const int n = boost_json_config(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_themes_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const int n = boost_json_themes(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_theme_active_put(const cJSON *body, char *out, size_t cap)
{
    const cJSON *id = body != NULL
        ? cJSON_GetObjectItemCaseSensitive(body, "id") : NULL;
    if (!cJSON_IsString(id)) {
        snprintf(out, cap, "{\"error\":\"invalid_theme\"}");
        return 400;
    }
    if (boost_model_set_active_theme(id->valuestring) != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"theme_not_found\"}");
        return 404;
    }
    if (boost_display_lock(1000) == ESP_OK) {
        boost_gauge_apply_theme(boost_model_active_theme());
        boost_display_unlock();
    }
    const int n = boost_json_themes(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_themes_config_put(const cJSON *body, char *out, size_t cap)
{
    if (body == NULL) {
        snprintf(out, cap, "{\"error\":\"invalid_json\"}");
        return 400;
    }
    /* Same setters themes_config_put() calls; see boost_web.c. */
    const cJSON *flat = cJSON_GetObjectItemCaseSensitive(body, "bigDigitStaticBg");
    if (cJSON_IsBool(flat)) {
        boost_theme_set_bigdigit_static_bg(cJSON_IsTrue(flat));
    }
    const cJSON *px = cJSON_GetObjectItemCaseSensitive(body, "pixelShift");
    if (cJSON_IsBool(px)) {
        boost_theme_set_pixel_shift(cJSON_IsTrue(px));
    }
    const cJSON *pxsec = cJSON_GetObjectItemCaseSensitive(body, "pixelShiftSec");
    if (cJSON_IsNumber(pxsec)) {
        const double v = pxsec->valuedouble;
        if (v >= 1.0 && v <= 86400.0) {
            boost_theme_set_pixel_shift_sec((uint16_t)(v > 65535.0 ? 65535.0 : v));
        }
    }
    const cJSON *ctext = cJSON_GetObjectItemCaseSensitive(body, "bigDigitColorText");
    if (cJSON_IsBool(ctext)) {
        boost_theme_set_bigdigit_color_text(cJSON_IsTrue(ctext));
    }
    uint32_t rgb = 0;
    const cJSON *bcol = cJSON_GetObjectItemCaseSensitive(body, "bigDigitStaticColor");
    if (bcol != NULL && app_parse_hex_color(bcol, &rgb)) {
        boost_theme_set_bigdigit_static_color(rgb);
    }
    const cJSON *btxc = cJSON_GetObjectItemCaseSensitive(body, "bigDigitTextColor");
    if (btxc != NULL && app_parse_hex_color(btxc, &rgb)) {
        boost_theme_set_bigdigit_text_color(rgb);
    }
    const cJSON *ag = cJSON_GetObjectItemCaseSensitive(body, "arcGradient");
    if (cJSON_IsBool(ag)) {
        boost_theme_set_arc_gradient(cJSON_IsTrue(ag));
    }
    const cJSON *hg = cJSON_GetObjectItemCaseSensitive(body, "hudGradient");
    if (cJSON_IsBool(hg)) {
        boost_theme_set_hud_gradient(cJSON_IsTrue(hg));
    }
    const cJSON *hb = cJSON_GetObjectItemCaseSensitive(body, "hudTrueBlack");
    if (cJSON_IsBool(hb)) {
        boost_theme_set_hud_true_black(cJSON_IsTrue(hb));
    }
    const cJSON *nsp = cJSON_GetObjectItemCaseSensitive(body, "neonMarqueeSpin");
    if (cJSON_IsBool(nsp)) {
        boost_theme_set_neon_marquee_spin(cJSON_IsTrue(nsp));
    }
    const cJSON *te = cJSON_GetObjectItemCaseSensitive(body, "teSync");
    if (cJSON_IsBool(te)) {
        const bool on = cJSON_IsTrue(te);
        boost_theme_set_te_sync(on);
        boost_display_set_te(on);
    }
    const cJSON *rdb = cJSON_GetObjectItemCaseSensitive(body, "regionDBuf");
    if (cJSON_IsBool(rdb)) {
        const bool on = cJSON_IsTrue(rdb);
        boost_theme_set_region_dbuf(on);
        boost_display_set_region_dbuf(on);
    }
    const cJSON *tsl = cJSON_GetObjectItemCaseSensitive(body, "teScanline");
    if (cJSON_IsBool(tsl)) {
        const bool on = cJSON_IsTrue(tsl);
        boost_theme_set_te_scanline(on);
        boost_display_set_te_scanline(on);
    }
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(body, "rotation");
    if (cJSON_IsNumber(rot)) {
        const double deg = rot->valuedouble;
        if (deg == 0 || deg == 90 || deg == 180 || deg == 270) {
            boost_theme_set_rotation((uint16_t)deg);
        }
    }
    const cJSON *demo = cJSON_GetObjectItemCaseSensitive(body, "demoMode");
    if (cJSON_IsBool(demo)) {
        boost_theme_set_demo_mode(cJSON_IsTrue(demo));
    }
    const cJSON *fsweep = cJSON_GetObjectItemCaseSensitive(body, "demoFastSweep");
    if (cJSON_IsBool(fsweep)) {
        boost_theme_set_demo_fast_sweep(cJSON_IsTrue(fsweep));
    }
    const cJSON *tble = cJSON_GetObjectItemCaseSensitive(body, "tpmsBle");
    if (cJSON_IsBool(tble)) {
        boost_theme_set_tpms_ble(cJSON_IsTrue(tble));
        boost_obd_set_enabled(cJSON_IsTrue(tble));
    }
    const cJSON *vface = cJSON_GetObjectItemCaseSensitive(body, "vaultFace");
    if (vface != NULL && app_parse_hex_color(vface, &rgb)) {
        boost_theme_set_vault_face(rgb);
    }
    const cJSON *vvig = cJSON_GetObjectItemCaseSensitive(body, "vaultVignette");
    if (cJSON_IsNumber(vvig)) {
        const double v = vvig->valuedouble;
        if (v >= 0.0 && v <= 90.0) {
            boost_theme_set_vault_vignette_pct((uint8_t)v);
        }
    }
    const cJSON *vred = cJSON_GetObjectItemCaseSensitive(body, "vaultNeedleRed");
    if (cJSON_IsBool(vred)) {
        boost_theme_set_vault_needle_red(cJSON_IsTrue(vred));
    }
    const cJSON *vtail = cJSON_GetObjectItemCaseSensitive(body, "vaultNeedleTail");
    if (cJSON_IsBool(vtail)) {
        boost_theme_set_vault_needle_tail(cJSON_IsTrue(vtail));
    }
    const cJSON *nlay = cJSON_GetObjectItemCaseSensitive(body, "neonLayout");
    if (cJSON_IsNumber(nlay)) {
        const double v = nlay->valuedouble;
        if (v >= 0.0 && v <= 2.0) {
            boost_theme_set_neon_layout((boost_neon_layout_t)(int)v);
        }
    }
    const cJSON *np = cJSON_GetObjectItemCaseSensitive(body, "neonPreset");
    if (cJSON_IsNumber(np)) {
        const double v = np->valuedouble;
        if (v >= 0.0 && v <= 3.0) {
            boost_theme_set_neon_preset((boost_neon_preset_t)(int)v);
        }
    }
    const cJSON *nfont = cJSON_GetObjectItemCaseSensitive(body, "neonFont");
    if (cJSON_IsNumber(nfont)) {
        const double v = nfont->valuedouble;
        if (v >= 0.0 && v <= 1.0) {
            boost_theme_set_neon_font((boost_neon_font_t)(int)v);
        }
    }
    const cJSON *tid = cJSON_GetObjectItemCaseSensitive(body, "id");
    if (cJSON_IsString(tid)) {
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(body, "reset"))) {
            if (!boost_theme_reset_colors(tid->valuestring)) {
                snprintf(out, cap, "{\"error\":\"theme_not_found\"}");
                return 404;
            }
        } else {
            const boost_theme_t *cur = boost_theme_find(tid->valuestring);
            if (cur == NULL) {
                snprintf(out, cap, "{\"error\":\"theme_not_found\"}");
                return 404;
            }
            boost_theme_colors_t colors = {
                .vacuum = cur->vacuum,
                .boost = cur->boost,
                .overboost = cur->overboost,
            };
            const cJSON *c = cJSON_GetObjectItemCaseSensitive(body, "colors");
            if (cJSON_IsObject(c)) {
                const struct { const char *key; uint32_t *dst; } f[] = {
                    { "vacuum", &colors.vacuum },
                    { "boost", &colors.boost },
                    { "overboost", &colors.overboost },
                };
                for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); ++i) {
                    const cJSON *item = cJSON_GetObjectItemCaseSensitive(c, f[i].key);
                    if (item != NULL && !app_parse_hex_color(item, f[i].dst)) {
                        snprintf(out, cap, "{\"error\":\"invalid_color\"}");
                        return 400;
                    }
                }
            }
            boost_theme_set_colors(tid->valuestring, &colors);
        }
    }
    /* Mirror themes_config_put(): rebuild the scene after any change. */
    if (boost_display_lock(1000) == ESP_OK) {
        boost_gauge_apply_theme(boost_model_active_theme());
        boost_display_unlock();
    }
    /* Echo the full /themes payload so the client can fold the response back
     * into local state (mirroring the HTTP PUT), not just {"ok":true}. */
    const int n = boost_json_themes(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_tpms_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const int n = boost_json_tpms_config(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_tpms_put(const cJSON *body, char *out, size_t cap)
{
    if (body == NULL) {
        snprintf(out, cap, "{\"error\":\"invalid_json\"}");
        return 400;
    }
    boost_tpms_config_t cfg;
    boost_tpms_get_config(&cfg);
    bool ok = true;
    float ftmp;
    if (app_json_float(body, "lowKpa", &ftmp)) {
        if (!(ftmp >= 100.0f && ftmp <= 400.0f)) ok = false;
        else cfg.low_kpa = ftmp;
    } else if (app_json_float(body, "lowPsi", &ftmp)) {
        const float kpa = ftmp / BOOST_TPMS_KPA_TO_PSI;
        if (!(kpa >= 100.0f && kpa <= 400.0f)) ok = false;
        else cfg.low_kpa = kpa;
    }
    int tmp;
    if (ok && app_json_int(body, "staleAfterMs", &tmp)) {
        if (!(tmp >= 2000 && tmp <= 120000)) ok = false;
        else cfg.stale_after_ms = (uint32_t)tmp;
    }
    if (!ok || !boost_tpms_set_config(&cfg)) {
        snprintf(out, cap, "{\"error\":\"invalid_tpms_config\"}");
        return 400;
    }
    const int n = boost_json_tpms_config(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_cal_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const int n = boost_json_calibration(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

/* Mirror of the HTTP PUT /sensors/supply handler (boost_web.c): NVS-persisted
 * MAP sensor supply voltage, range-checked in the setter. Sync route — no
 * flash erase or radio blocking, same class as the /config PUTs. */
static int route_supply_put(const cJSON *body, char *out, size_t cap)
{
    const cJSON *volts = cJSON_GetObjectItemCaseSensitive(body, "supplyVolts");
    if (!cJSON_IsNumber(volts)) {
        snprintf(out, cap, "{\"error\":\"invalid_supply\"}");
        return 400;
    }
    const double v = volts->valuedouble;
    /* Range-check as a double before narrowing (same reason as the HTTP path):
     * NaN/overflow narrowing is undefined, so never hand the raw float down. */
    if (!(v >= (double)BOOST_MAP_SUPPLY_MIN && v <= (double)BOOST_MAP_SUPPLY_MAX)) {
        snprintf(out, cap, "{\"error\":\"invalid_supply\"}");
        return 400;
    }
    const esp_err_t err = boost_sensors_set_supply_volts((float)v);
    if (err == ESP_ERR_INVALID_ARG) {
        snprintf(out, cap, "{\"error\":\"invalid_supply\"}");
        return 400;
    }
    if (err != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"persist_failed\"}");
        return 500;
    }
    const int n = boost_json_calibration(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_time_post(const cJSON *body, char *out, size_t cap)
{
    const cJSON *epoch = body != NULL
        ? cJSON_GetObjectItemCaseSensitive(body, "epochMs") : NULL;
    const cJSON *tz = body != NULL
        ? cJSON_GetObjectItemCaseSensitive(body, "timezoneOffsetMinutes") : NULL;
    const cJSON *tzstr = body != NULL
        ? cJSON_GetObjectItemCaseSensitive(body, "timezoneTz") : NULL;
    if (!cJSON_IsNumber(epoch) || !cJSON_IsNumber(tz)) {
        snprintf(out, cap, "{\"error\":\"invalid_time\"}");
        return 400;
    }
    const char *tz_tz = (cJSON_IsString(tzstr) && tzstr->valuestring != NULL)
        ? tzstr->valuestring : NULL;
    const esp_err_t err = boost_model_set_time((int64_t)epoch->valuedouble,
                                                tz->valueint, tz_tz);
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(out, cap, "{\"error\":\"clock_rejected\"}");
        return 409;
    }
    if (err != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"time_not_set\"}");
        return 400;
    }
    boost_model_refresh_status();
    const int n = boost_json_state(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static void app_restart_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting on BLE API request");
    esp_restart();
}

static int route_restart_post(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    static esp_timer_handle_t timer;
    if (timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = app_restart_timer_cb,
            .name = "ble_api_restart",
        };
        if (esp_timer_create(&args, &timer) != ESP_OK) {
            snprintf(out, cap, "{\"error\":\"restart_failed\"}");
            return 500;
        }
    }
    const esp_err_t err = esp_timer_start_once(timer, 400 * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        snprintf(out, cap, "{\"error\":\"restart_failed\"}");
        return 500;
    }
    snprintf(out, cap, "{\"ok\":true,\"restartingInMs\":400}");
    return 200;
}

static int route_logs_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    // Compact: ~22 B per sample {"tMs":123,"psi":12.34} vs 96 B verbose.
    // With 4096 cap we fit ~128 compact points (4096/32) — 1500 raw (5m)
    // decimated to 128 gives ~2.1 pts/triangle vs 0.7 at 42 verbose.
    // LogSample decodes missing zone/demo with defaults, so compact is safe.
    const size_t max_fit = (cap / 32) > 2 ? (cap / 32) : 2;
    size_t want = s_pending_logs_limit;
    s_pending_logs_limit = 0;
    if (want == 0) want = max_fit;
    if (want > 5000) want = 5000;
    const size_t alloc = want > max_fit ? want : max_fit;
    boost_log_sample_t *samples = heap_caps_malloc(
        alloc * sizeof(*samples), MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        samples = malloc(alloc * sizeof(*samples));
    }
    if (samples == NULL) {
        snprintf(out, cap, "{\"error\":\"no_mem\"}");
        return 500;
    }
    const size_t n = boost_model_copy_logs(samples, want);
    size_t stride_n = n;
    if (n > max_fit) {
        stride_n = max_fit;
        for (size_t i = 0; i < stride_n; ++i) {
            size_t src = i * n / stride_n;
            if (src >= n) src = n - 1;
            if (i != src) samples[i] = samples[src];
        }
    }
    size_t used = (size_t)snprintf(out, cap, "{\"samples\":[");
    for (size_t i = 0; i < stride_n; ++i) {
        const boost_log_sample_t *s = &samples[i];
        int line = snprintf(out + used, cap - used,
            "%s{\"tMs\":%lu,\"psi\":%.2f}",
            i == 0 ? "" : ",",
            (unsigned long)s->t_ms, (double)s->psi);
        if (line < 0 || (size_t)line >= cap - used) break;
        used += (size_t)line;
    }
    snprintf(out + used, cap - used, "]}");
    free(samples);
    return 200;
}

static int route_network_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const int n = boost_json_network_status(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

/* Mirror boost_web.c network_put(): NVS + immediate status return, cheap
 * enough to run on the NimBLE host task like the other inline routes. */
static int route_network_put(const cJSON *body, char *out, size_t cap)
{
    if (body == NULL) {
        snprintf(out, cap, "{\"error\":\"invalid_json\"}");
        return 400;
    }
    const char *ssid = NULL;
    const char *password = NULL;
    bool keep_password = true;
    bool have_mode = false;
    boost_net_mode_t mode = BOOST_NET_MODE_AP;

    const cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    if (cJSON_IsString(ssid_j)) {
        ssid = ssid_j->valuestring;
    }
    const cJSON *pass_j = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (cJSON_IsString(pass_j)) {
        password = pass_j->valuestring;
        keep_password = false;
    }
    const cJSON *keep_j = cJSON_GetObjectItemCaseSensitive(body, "keepPassword");
    if (cJSON_IsBool(keep_j)) {
        keep_password = cJSON_IsTrue(keep_j);
        if (keep_password) {
            password = "";
        }
    }
    const cJSON *mode_j = cJSON_GetObjectItemCaseSensitive(body, "mode");
    if (cJSON_IsString(mode_j) && mode_j->valuestring) {
        have_mode = true;
        if (strcmp(mode_j->valuestring, "apsta") == 0) {
            mode = BOOST_NET_MODE_APSTA;
        } else if (strcmp(mode_j->valuestring, "ap") == 0) {
            mode = BOOST_NET_MODE_AP;
        } else {
            snprintf(out, cap, "{\"error\":\"invalid_mode\"}");
            return 400;
        }
    }

    const esp_err_t err = boost_network_update(ssid, password, keep_password,
                                               mode, have_mode);
    if (err != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"network_update_failed\"}");
        return 400;
    }
    const int n = boost_json_network_status(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

/* Mirror boost_web.c network_delete(): saved-network removal + status echo. */
static int route_network_delete(const cJSON *body, char *out, size_t cap)
{
    if (body == NULL) {
        snprintf(out, cap, "{\"error\":\"invalid_json\"}");
        return 400;
    }
    const cJSON *ssid_j = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const char *ssid = cJSON_IsString(ssid_j) ? ssid_j->valuestring : NULL;
    if (ssid == NULL || ssid[0] == '\0') {
        snprintf(out, cap, "{\"error\":\"missing_ssid\"}");
        return 400;
    }
    const esp_err_t err = boost_network_delete_saved(ssid);
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(out, cap, "{\"error\":\"not_found\"}");
        return 404;
    } else if (err != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"delete_failed\"}");
        return 400;
    }
    const int n = boost_json_network_status(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

static int route_network_reconnect_post(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    const esp_err_t err = boost_network_reconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        snprintf(out, cap, "{\"error\":\"reconnect_failed\"}");
        return 400;
    }
    const int n = boost_json_network_status(out, cap);
    return n > 0 && n < (int)cap ? 200 : 500;
}

/* Scanning takes seconds on the Wi-Fi radio, so this runs only on the driver
 * task via APP_EV_SCAN, never inline on the NimBLE host task. */
static int route_network_scan_get(const cJSON *body, char *out, size_t cap)
{
    (void)body;
    /* Heap-allocate the records array: a large stack buffer in this task
     * previously caused a boot-loop stack overflow panic. */
    boost_wifi_scan_record_t *records = heap_caps_malloc(
        BOOST_WIFI_SCAN_MAX_RECORDS * sizeof(*records), MALLOC_CAP_SPIRAM);
    if (records == NULL) {
        records = malloc(BOOST_WIFI_SCAN_MAX_RECORDS * sizeof(*records));
    }
    if (records == NULL) {
        snprintf(out, cap, "{\"error\":\"no_mem\"}");
        return 500;
    }
    memset(records, 0, BOOST_WIFI_SCAN_MAX_RECORDS * sizeof(*records));
    uint16_t count = 0;
    const esp_err_t err = boost_network_scan(records, BOOST_WIFI_SCAN_MAX_RECORDS,
                                             &count);
    if (err != ESP_OK) {
        free(records);
        snprintf(out, cap, "{\"error\":\"scan_failed\"}");
        return 400;
    }
    size_t off = 0;
    int n = snprintf(out, cap, "{\"networks\":[");
    if (n > 0) {
        off = (size_t)n;
    }
    for (uint16_t i = 0; i < count && off < cap; ++i) {
        char ssid_e[96];
        char item[160];
        boost_json_escape(records[i].ssid, ssid_e, sizeof(ssid_e));
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":%d}",
                 i == 0 ? "" : ",", ssid_e, records[i].rssi, records[i].authmode);
        n = snprintf(out + off, cap - off, "%s", item);
        if (n < 0) {
            break;
        }
        off += (size_t)n;
    }
    if (off + 2U < cap) {
        (void)snprintf(out + off, cap - off, "]}");
    }
    free(records);
    return 200;
}

static int route_page_put(const cJSON *body, char *out, size_t cap)
{
    int page = -1;
    if (body == NULL || !app_json_int(body, "page", &page)) {
        if (body != NULL) app_json_int(body, "activePage", &page);
    }
    if (page < 0 || page > 1) {
        snprintf(out, cap, "{\"error\":\"invalid_page\"}");
        return 400;
    }
    if (boost_model_set_active_page(page) != ESP_OK) {
        snprintf(out, cap, "{\"error\":\"display_unavailable\"}");
        return 503;
    }
    snprintf(out, cap, "{\"ok\":true,\"activePage\":%d}", page);
    return 200;
}

static const app_ble_route_t s_routes[] = {
    { "/state", "GET", route_state },
    { "/config", "GET", route_config_get },
    { "/config", "PUT", route_config_put },
    { "/themes", "GET", route_themes_get },
    { "/themes/active", "PUT", route_theme_active_put },
    { "/themes/config", "PUT", route_themes_config_put },
    { "/tpms/config", "GET", route_tpms_get },
    { "/tpms/config", "PUT", route_tpms_put },
    { "/sensors/calibration", "GET", route_cal_get },
    { "/sensors/supply", "PUT", route_supply_put },
    { "/time", "POST", route_time_post },
    { "/restart", "POST", route_restart_post },
    { "/logs", "GET", route_logs_get },
    { "/network", "GET", route_network_get },
    { "/network", "PUT", route_network_put },
    { "/network", "DELETE", route_network_delete },
    { "/network/scan", "GET", route_network_scan_get },
    { "/network/reconnect", "POST", route_network_reconnect_post },
    { "/page", "PUT", route_page_put },
};

/* --- Control write handling (NimBLE host task) --------------------------- */

static void app_control_handle(uint16_t conn_handle, uint32_t id,
                               const char *request)
{
    cJSON *root = cJSON_Parse(request);
    if (root == NULL) {
        app_ble_enqueue_tx(conn_handle,
                           app_ble_make_response(id, 400, "{\"error\":\"invalid_json\"}"));
        return;
    }
    const cJSON *path = cJSON_GetObjectItemCaseSensitive(root, "path");
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    if (!cJSON_IsString(path) || path->valuestring == NULL ||
        !cJSON_IsString(method) || method->valuestring == NULL) {
        cJSON_Delete(root);
        app_ble_enqueue_tx(conn_handle,
                           app_ble_make_response(id, 400, "{\"error\":\"invalid_request\"}"));
        return;
    }

    /* Async routes that block (flash/NVS/I2C or copy+JSON) — run on the
     * driver task rather than stalling the NimBLE host event loop. */
    if (strcmp(path->valuestring, "/sensors/calibration") == 0 &&
        strcmp(method->valuestring, "POST") == 0) {
        app_ble_ev_t ev = { 0 };
        ev.type = APP_EV_CALIBRATE;
        ev.conn_handle = conn_handle;
        ev.id = id;
        if (s_evq == NULL || xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            app_ble_enqueue_tx(conn_handle,
                               app_ble_make_response(id, 503, "{\"error\":\"busy\"}"));
        }
        cJSON_Delete(root);
        return;
    }
    if (strncmp(path->valuestring, "/logs", 5) == 0 &&
        strcmp(method->valuestring, "GET") == 0) {
        const char *q = strchr(path->valuestring, '?');
        size_t lim = 0;
        if (q != NULL) {
            const char *p = strstr(q, "limit=");
            if (p != NULL) {
                long l = strtol(p + 6, NULL, 10);
                if (l > 0 && l < 10000) lim = (size_t)l;
            }
        }
        app_ble_ev_t ev = { 0 };
        ev.type = APP_EV_LOGS;
        ev.conn_handle = conn_handle;
        ev.id = id;
        ev.logs_limit = lim;
        if (s_evq == NULL || xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            app_ble_enqueue_tx(conn_handle,
                               app_ble_make_response(id, 503, "{\"error\":\"busy\"}"));
        }
        cJSON_Delete(root);
        return;
    }
    if (strncmp(path->valuestring, "/network/scan", 13) == 0 &&
        strcmp(method->valuestring, "GET") == 0) {
        /* boost_network_scan() blocks for seconds on the Wi-Fi radio — run it
         * on the driver task, never on the NimBLE host event loop. */
        app_ble_ev_t ev = { 0 };
        ev.type = APP_EV_SCAN;
        ev.conn_handle = conn_handle;
        ev.id = id;
        if (s_evq == NULL || xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            app_ble_enqueue_tx(conn_handle,
                               app_ble_make_response(id, 503, "{\"error\":\"busy\"}"));
        }
        cJSON_Delete(root);
        return;
    }

    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (body != NULL && !cJSON_IsObject(body)) {
        cJSON_Delete(root);
        app_ble_enqueue_tx(conn_handle,
                           app_ble_make_response(id, 400, "{\"error\":\"invalid_body\"}"));
        return;
    }

    int status = 404;
    char *resp_body = malloc(APP_BLE_CTRL_RESP_MAX);
    if (resp_body == NULL) {
        cJSON_Delete(root);
        app_ble_enqueue_tx(conn_handle,
                           app_ble_make_response(id, 500, "{\"error\":\"no_mem\"}"));
        return;
    }
    resp_body[0] = '\0';
    bool routed = false;
    char route_path[96];
    strlcpy(route_path, path->valuestring, sizeof(route_path));
    char *qmark = strchr(route_path, '?');
    if (qmark != NULL) *qmark = '\0';
    for (size_t i = 0; i < sizeof(s_routes) / sizeof(s_routes[0]); ++i) {
        if (strcmp(s_routes[i].path, route_path) == 0 &&
            strcmp(s_routes[i].method, method->valuestring) == 0) {
            status = s_routes[i].handle(body, resp_body, APP_BLE_CTRL_RESP_MAX);
            routed = true;
            break;
        }
    }
    if (!routed) {
        snprintf(resp_body, APP_BLE_CTRL_RESP_MAX, "{\"error\":\"not_found\"}");
        status = 404;
    }
    cJSON_Delete(root);
    app_ble_enqueue_tx(conn_handle, app_ble_make_response(id, status, resp_body));
    free(resp_body);
}

static int app_control_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        ESP_LOGI(TAG, "control write: %u B from handle %u", (unsigned)len, conn_handle);
        if (len > APP_BLE_CTRL_MAX) {
            /* Can only happen with a peer that negotiated a very large MTU;
             * answer with the spec'd 413 and drop the rest. */
            app_ble_enqueue_tx(conn_handle,
                app_ble_make_response(0, 413, "{\"error\":\"too_large\"}"));
            return 0;
        }
        char req[APP_BLE_CTRL_MAX + 1];
        os_mbuf_copydata(ctxt->om, 0, len, (uint8_t *)req);
        req[len] = '\0';

        cJSON *root = cJSON_Parse(req);
        const cJSON *idj = root != NULL
            ? cJSON_GetObjectItemCaseSensitive(root, "id") : NULL;
        uint32_t id = 0;
        if (root == NULL || !cJSON_IsNumber(idj) || idj->valuedouble < 0.0 ||
            idj->valuedouble > (double)UINT32_MAX) {
            cJSON_Delete(root);
            app_ble_enqueue_tx(conn_handle,
                app_ble_make_response(0, 400, "{\"error\":\"invalid_request\"}"));
            return 0;
        }
        id = (uint32_t)idj->valuedouble;
        cJSON_Delete(root);
        app_control_handle(conn_handle, id, req);
        return 0;
    }
    /* Control is write+notify only; the stack rejects reads before this
     * callback for unflagged reads, but never hand out a bogus value. */
    return BLE_ATT_ERR_UNLIKELY;
}

/* --- Status / Device-Info / Log reads (NimBLE host task) ----------------- */

static int app_status_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    char json[APP_BLE_STATUS_BUF];
    const int n = boost_json_state(json, sizeof(json));
    if (n <= 0 || n >= (int)sizeof(json)) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ctxt->offset >= (uint16_t)n) {
        /* CoreBluetooth probes the next blob offset even after a bounded
         * value; a zero-length chunk terminates its assembled read cleanly. */
        return 0;
    }
    const int rc = os_mbuf_append(ctxt->om, json + ctxt->offset,
                                  (uint16_t)n - ctxt->offset);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int app_dev_info_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    ESP_LOGI(TAG, "device info read: conn=%u offset=%u len=%u",
             (unsigned)conn_handle, (unsigned)ctxt->offset,
             (unsigned)strlen(s_device_info));
    const size_t len = strlen(s_device_info);
    if (ctxt->offset >= len) {
        return BLE_ATT_ERR_INVALID_OFFSET;
    }
    const int rc = os_mbuf_append(ctxt->om, s_device_info + ctxt->offset,
                                  (uint16_t)(len - ctxt->offset));
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define APP_LOG_MAGIC   "BGL1\n"
#define APP_LOG_HEADER  "t_ms,psi,peak_psi,zone,demo\n"

static void app_log_cache_free(void)
{
    if (s_log_cache != NULL) {
        free(s_log_cache);
        s_log_cache = NULL;
    }
    s_log_cache_len = 0;
    s_log_cache_conn = BLE_HS_CONN_HANDLE_NONE;
}

/*
 * Build the "BGL1"+header+samples CSV on demand into a (preferably PSRAM)
 * heap buffer, cached for the connection. The ATT Read Blob offset field is
 * 16-bit, so a client cannot address past 64 KB: the BLE Log characteristic
 * BLE reads therefore expose a bounded recent diagnostic window and
 * /logs.csv keeps serving the full one-hour ring.
 */
#define APP_LOG_MAX_SAMPLES 8u

static char *app_log_cache_build(size_t *out_len)
{
    const size_t sample_bytes = APP_LOG_MAX_SAMPLES * sizeof(boost_log_sample_t);
    boost_log_sample_t *samples = heap_caps_malloc(sample_bytes, MALLOC_CAP_SPIRAM);
    if (samples == NULL) {
        samples = malloc(sample_bytes);
    }
    if (samples == NULL) {
        return NULL;
    }
    const size_t n = boost_model_copy_logs(samples, APP_LOG_MAX_SAMPLES);
    const size_t head_len = strlen(APP_LOG_MAGIC) + strlen(APP_LOG_HEADER);
    const size_t cap = head_len + n * 64u + 16u;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = malloc(cap);
    }
    if (buf == NULL) {
        free(samples);
        return NULL;
    }
    size_t off = 0;
    const int h = snprintf(buf + off, cap - off, "%s%s", APP_LOG_MAGIC, APP_LOG_HEADER);
    if (h > 0) {
        off += (size_t)h;
    }
    for (size_t i = 0; i < n && off < cap; ++i) {
        const int w = snprintf(buf + off, cap - off,
                               "%lu,%.2f,%.2f,%s,%d\n",
                               (unsigned long)samples[i].t_ms,
                               (double)samples[i].psi,
                               (double)samples[i].peak_psi,
                               samples[i].zone,
                               samples[i].demo ? 1 : 0);
        if (w <= 0) {
            break;
        }
        off += (size_t)w;
    }
    free(samples);
    *out_len = off;
    return buf;
}

static int app_log_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (s_log_cache == NULL || conn_handle != s_log_cache_conn) {
        app_log_cache_free();
        size_t len = 0;
        s_log_cache = app_log_cache_build(&len);
        if (s_log_cache == NULL) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        s_log_cache_len = len;
        s_log_cache_conn = conn_handle;
    }
    if ((size_t)ctxt->offset >= s_log_cache_len) {
        /* Offset past the end: short read (zero bytes) ends the transfer,
         * per the companion-app Log contract. */
        return 0;
    }
    size_t remain = s_log_cache_len - (size_t)ctxt->offset;
    if (remain > 0xFFFFu) {
        remain = 0xFFFFu;
    }
    const int rc = os_mbuf_append(ctxt->om, s_log_cache + ctxt->offset,
                                  (uint16_t)remain);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* --- GATT service table --------------------------------------------------- */

static int app_svc_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (attr_handle == s_ctl_val_handle) {
        return app_control_access(conn_handle, attr_handle, ctxt, arg);
    }
    if (attr_handle == s_status_val_handle) {
        return app_status_access(conn_handle, attr_handle, ctxt, arg);
    }
    if (attr_handle == s_log_val_handle) {
        return app_log_access(conn_handle, attr_handle, ctxt, arg);
    }
    if (attr_handle == s_dev_info_val_handle) {
        return app_dev_info_access(conn_handle, attr_handle, ctxt, arg);
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svc_defs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* Control: write-with-response + notify, ENCRYPTED. */
                .uuid = &s_uuid_control.u,
                .access_cb = app_svc_access,
                .val_handle = &s_ctl_val_handle,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY |
                         BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                /* Status: read-only. Companion clients poll Control /state for
                 * live state; the 1 Hz notify broadcast had no subscribers. */
                .uuid = &s_uuid_status.u,
                .access_cb = app_svc_access,
                .val_handle = &s_status_val_handle,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {
                /* Log: read-only ENCRYPTED, offset long-reads. */
                .uuid = &s_uuid_log.u,
                .access_cb = app_svc_access,
                .val_handle = &s_log_val_handle,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC,
            },
            {
                /* Device info: read. */
                .uuid = &s_uuid_dev_info.u,
                .access_cb = app_svc_access,
                .val_handle = &s_dev_info_val_handle,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
    { 0 },
};

/* --- GAP ---------------------------------------------------------------- */

static int app_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    app_ble_ev_t ev = { 0 };
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ev.type = APP_EV_CONNECTED;
            ev.conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected (handle %u)",
                     (unsigned)event->connect.conn_handle);
        } else {
            ev.type = APP_EV_DISCONNECTED;
            ev.conn_handle = event->connect.conn_handle;
            ESP_LOGW(TAG, "connect attempt failed: status=0x%04x",
                     (unsigned)event->connect.status);
        }
        if (s_evq != NULL && xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            ESP_LOGE(TAG, "connect event queue full");
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ev.type = APP_EV_DISCONNECTED;
        ev.conn_handle = event->disconnect.conn.conn_handle;
        ESP_LOGW(TAG, "phone disconnected (handle %u, reason %u)",
                 (unsigned)event->disconnect.conn.conn_handle,
                 (unsigned)event->disconnect.reason);
        if (s_evq != NULL && xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            ESP_LOGE(TAG, "disconnect event queue full");
        }
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ev.type = APP_EV_SUBSCRIBE;
        ev.conn_handle = event->subscribe.conn_handle;
        ev.attr_handle = event->subscribe.attr_handle;
        ev.cur_notify = event->subscribe.cur_notify;
        if (s_evq != NULL && xQueueSend(s_evq, &ev, 0) != pdTRUE) {
            ESP_LOGE(TAG, "subscribe event queue full");
        }
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Unreachable with Just Works (BLE_HS_IO_NO_INPUT_OUTPUT, MITM off);
         * kept as a diagnostic in case the security config ever changes. */
        ESP_LOGW(TAG, "unexpected passkey action %u",
                 (unsigned)event->passkey.params.action);
        return 0;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.channel_id == BLE_L2CAP_CID_ATT) {
            s_att_mtu = event->mtu.value;
        }
        ESP_LOGI(TAG, "MTU updated: %u (cid=%u)",
                 (unsigned)event->mtu.value, (unsigned)event->mtu.channel_id);
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption changed: handle %u status %u (0=encrypted)",
                 (unsigned)event->enc_change.conn_handle,
                 (unsigned)event->enc_change.status);
        if (event->enc_change.status != 0) {
            /* Failed encryption usually means the peer holds a bond whose
             * keys we lost (RAM store before NVS_PERSIST, or a flash wipe).
             * Drop OUR copy so the next pairing attempt starts clean instead
             * of deadlocking (phone thinks it is bonded, never re-pairs). */
            struct ble_gap_conn_desc enc_desc;
            if (ble_gap_conn_find(event->enc_change.conn_handle, &enc_desc) == 0) {
                ESP_LOGW(TAG, "encryption failed; deleting our bond for the peer");
                ble_store_util_delete_peer(&enc_desc.peer_id_addr);
            }
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        /* The phone re-paired from an address we still hold a bond for
         * (app reinstall wipes the phone's bond; ours persists in NVS).
         * Returning 0/IGNORE keeps the stale bond whose keys the phone does
         * not have, so encryption can never re-establish and every encrypted
         * Control/Log write fails (hardware-verified 2026-08-23). Delete the
         * conflicting bond and let pairing retry; the passkey overlay fires
         * again via the normal PASSKEY_ACTION path. */
        ESP_LOGW(TAG, "repeat pairing from bonded peer; deleting stale bond and retrying");
        struct ble_gap_conn_desc rp_desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &rp_desc) == 0) {
            ble_store_util_delete_peer(&rp_desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Undirected connectable advertising runs forever; if the controller
         * ever completes it anyway, restart on the driver task. */
        if (s_evq != NULL) {
            ev.type = APP_EV_ADV_RETRY;
            if (xQueueSend(s_evq, &ev, 0) != pdTRUE) {
                ESP_LOGE(TAG, "adv-complete event queue full");
            }
        }
        return 0;
    default:
        return 0;
    }
}

static void app_adv_attempt(uint32_t *retries)
{
    if (!s_want_adv || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        *retries = 0;
        return;
    }
    if (!boost_obd_ble_host_up()) {
        (*retries)++;
        return;
    }
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &s_uuid_svc;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    /* The 128-bit UUID + flags alone fill 21 of the 31 legacy adv bytes; the
     * name goes in the scan response (hardware: EMSGSIZE otherwise). */
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields failed: %d", rc);
        (*retries)++;
        return;
    }
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)APP_BLE_NAME;
    rsp.name_len = (uint8_t)strlen(APP_BLE_NAME);
    rsp.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv rsp failed: %d", rc);
        (*retries)++;
        return;
    }
    uint8_t own_addr_type = 0;
    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGW(TAG, "addr type failed: %d", rc);
        (*retries)++;
        return;
    }
    /* This dashboard's antenna path measured near the Intel Mac's receive
     * floor (-100 dBm). Make the peripheral's advertising link budget
     * explicit rather than relying on the controller's inherited default. */
    rc = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, ESP_PWR_LVL_P9);
    if (rc != ESP_OK) {
        ESP_LOGW(TAG, "advertising TX power set failed: %s", esp_err_to_name(rc));
    }
    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = APP_BLE_ADV_ITVL_MIN;
    params.itvl_max = APP_BLE_ADV_ITVL_MAX;
    rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &params,
                           app_gap_event, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "advertising (%u..%u ms)", 160u, 250u);
        *retries = 0;
    } else {
        ESP_LOGW(TAG, "adv start failed: %d", rc);
        (*retries)++;
    }
}

/* --- driver task: single owner of this module's GAP/notify calls --------- */

static void app_driver_task(void *arg)
{
    (void)arg;
    uint32_t adv_retries = 0;
    app_ble_ev_t ev;
    for (;;) {
        xQueueReceive(s_evq, &ev, portMAX_DELAY);

        switch (ev.type) {
        case APP_EV_START:
            s_want_adv = true;
            adv_retries = 0;
            app_adv_attempt(&adv_retries);
            break;
        case APP_EV_STOP:
            s_want_adv = false;
            if (ble_gap_adv_active()) {
                (void)ble_gap_adv_stop();
            }
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                (void)ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
                s_ctl_subscribed = false;
                app_log_cache_free();
            }
            break;
        case APP_EV_CONNECTED:
            s_conn_handle = ev.conn_handle;
            if (ble_gap_adv_active()) {
                (void)ble_gap_adv_stop();
            }
            break;
        case APP_EV_DISCONNECTED:
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ctl_subscribed = false;
            app_log_cache_free();
            if (s_want_adv) {
                adv_retries = 0;
                app_adv_attempt(&adv_retries);
            }
            break;
        case APP_EV_SUBSCRIBE:
            if (ev.conn_handle != s_conn_handle) {
                break;
            }
            if (ev.attr_handle == s_ctl_val_handle) {
                s_ctl_subscribed = ev.cur_notify != 0;
            }
            break;
        case APP_EV_ADV_RETRY:
            if (s_want_adv) {
                app_adv_attempt(&adv_retries);
            }
            break;
        case APP_EV_TX: {
            char *payload = ev.payload;
            if (payload != NULL &&
                ev.conn_handle == s_conn_handle &&
                s_ctl_val_handle != 0) {
                /* CoreBluetooth can restore a cached notify state without
                 * generating a fresh GAP_SUBSCRIBE event after reconnect.
                 * notify_custom remains the authority (and fails safely when
                 * the peer truly has no CCCD); do not discard a response only
                 * because our advisory mirror missed that event. */
                app_notify_fragmented(ev.conn_handle, s_ctl_val_handle,
                                      payload, strlen(payload));
            } else if (payload != NULL) {
                ESP_LOGW(TAG, "control response dropped (conn %u, sub %d)",
                         (unsigned)ev.conn_handle, s_ctl_subscribed ? 1 : 0);
            }
            free(payload);
            break;
        }
        case APP_EV_CALIBRATE: {
            /* Blocks ~2 s here (driver task), exactly like the HTTP POST does
             * to the httpd task; the NimBLE host loop is left unblocked. */
            const boost_cal_result_t result = boost_sensors_calibrate_atmosphere(NULL);
            char body[APP_BLE_CTRL_MAX + 64];
            int status = 200;
            if (result == BOOST_CAL_OK) {
                if (boost_json_calibration(body, sizeof(body)) <= 0) {
                    snprintf(body, sizeof(body), "{\"error\":\"calibration_failed\"}");
                    status = 500;
                }
            } else {
                status = (result == BOOST_CAL_ERR_PERSIST) ? 500 : 409;
                snprintf(body, sizeof(body), "{\"error\":\"%s\"}",
                         boost_sensors_cal_error_code(result));
            }
            app_ble_enqueue_tx(ev.conn_handle,
                               app_ble_make_response(ev.id, status, body));
            break;
        }
        case APP_EV_LOGS: {
            // Copy+JSON for the bounded window; must not block the host.
            ESP_LOGI(TAG, "logs: ev start (limit=%u, frag cap=%u)",
                     (unsigned)ev.logs_limit, (unsigned)APP_BLE_CTRL_RESP_MAX);
            s_pending_logs_limit = ev.logs_limit;
            char *body = malloc(APP_BLE_CTRL_RESP_MAX);
            if (body == NULL) {
                app_ble_enqueue_tx(ev.conn_handle,
                                   app_ble_make_response(ev.id, 500, "{\"error\":\"no_mem\"}"));
                break;
            }
            const int64_t t0 = esp_timer_get_time();
            int status = route_logs_get(NULL, body, APP_BLE_CTRL_RESP_MAX);
            const int64_t build_us = esp_timer_get_time() - t0;
            app_ble_enqueue_tx(ev.conn_handle,
                               app_ble_make_response(ev.id, status, body));
            ESP_LOGI(TAG, "logs: built %dB in %lldus; fragmenting to client",
                     (unsigned)strlen(body), (long long)build_us);
            free(body);
            break;
        }
        case APP_EV_SCAN: {
            /* Wi-Fi scan blocks for seconds; heap buffers only (a 4 KB stack
             * buffer in this task previously caused a boot-loop stack
             * overflow panic). */
            char *body = malloc(APP_BLE_CTRL_RESP_MAX);
            if (body == NULL) {
                app_ble_enqueue_tx(ev.conn_handle,
                                   app_ble_make_response(ev.id, 500, "{\"error\":\"no_mem\"}"));
                break;
            }
            int status = route_network_scan_get(NULL, body, APP_BLE_CTRL_RESP_MAX);
            app_ble_enqueue_tx(ev.conn_handle,
                               app_ble_make_response(ev.id, status, body));
            free(body);
            break;
        }
        default:
            break;
        }

        /* Retry advertising until the shared host is up (worst case a few
         * hundred ms after boot, or a runtime enable while the host was off).
         * A central scan/connect can transiently make ble_gap_adv_start()
         * fail too, so retries pace at 2 s instead of giving up forever:
         * want_adv stays asserted and the next attempt resumes advertising
         * as soon as the shared host/GAP allows. */
        if (adv_retries > 0 && adv_retries < APP_BLE_ADV_RETRY_MAX && s_want_adv &&
            s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            vTaskDelay(pdMS_TO_TICKS(APP_BLE_ADV_RETRY_MS));
            app_adv_attempt(&adv_retries);
        } else if (adv_retries >= APP_BLE_ADV_RETRY_MAX) {
            if (boost_obd_ble_host_up()) {
                ESP_LOGW(TAG, "advertising retry paced (another GAP procedure active?)");
            }
            adv_retries = 0;
            if (s_want_adv && s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                app_adv_attempt(&adv_retries);
            }
        }
    }
}

/* --- public API ---------------------------------------------------------- */

/* Host-dependent bring-up: preferred MTU, security config, and GATT service
 * registration. Requires the shared host MOUNTED (nimble_port_init done) but
 * NOT started; must run before boost_obd_ble_host_start(). */
static bool s_host_cfg_done;

static void app_ble_register_host_config(void)
{
    if (s_host_cfg_done) {
        return;
    }
    s_host_cfg_done = true;

    const int mtu_rc = ble_att_set_preferred_mtu(APP_BLE_MTU);
    if (mtu_rc != 0) {
        ESP_LOGW(TAG, "preferred MTU %u rejected (%d)", (unsigned)APP_BLE_MTU, mtu_rc);
    }

    /* Security config applies to the shared host before it starts: LE Secure
     * Connections + MITM, display-only passkey, bonding enabled. */
    /* Just Works pairing (user decision 2026-08-23): encrypted + bonded, no
     * passkey. Trades MITM protection for a zero-interaction pairing UX —
     * the panel-passkey overlay cost a full debug cycle and never won the
     * user's attention race against the phone's own dialog. Writes still
     * require an encrypted link; unpaired peers get ATT insufficient-auth. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0; // Intel 8265 on hackintosh wedges on LE Secure Connections; use legacy pairing for NUC8 stability (iPhone falls back cleanly)
    ble_hs_cfg.sm_sc_only = 0;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sync_cb = app_ble_on_sync;
#if CONFIG_BT_NIMBLE_NVS_PERSIST
    ble_store_config_init();
#endif

    /* Standard GAP (device name/appearance) and GATT (service changed)
     * services, then the companion service; all registered before the host
     * task starts. ble_gatts_start() runs from the sync callback. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set(APP_BLE_NAME);
    int rc = ble_gatts_count_cfg(s_svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT count failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_svc_defs);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT add failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "companion GATT service registered");
}

/* Host sync callback. The host finalizes the GATT server itself
 * (ble_hs_start -> ble_gatts_start, ble_hs.c) BEFORE this fires; do not call
 * ble_gatts_start() here — a second call walks already-registered services
 * and faults (hardware-verified 2026-08-23). Advertising retries in the
 * driver task cover the pre-sync window. */
static void app_ble_on_sync(void)
{
    s_host_synced = true;
    ESP_LOGI(TAG, "host synced; GATT server ready");
}

void boost_app_ble_init(void)
{
    if (s_init_done) {
        return;
    }
    s_init_done = true;

    /* Persistence must not depend on another module's NVS init (guard rail);
     * nvs_flash_init() is idempotent. */
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            nerr = nvs_flash_init();
        }
    }
    if (nerr == ESP_OK) {
        s_enabled = app_ble_nvs_read_enabled();
    }

    app_device_info_rebuild();

    /* Host-dependent config only when the shared host is already mounted
     * (tpmsBle boot path). Otherwise start() registers after mounting. */
    if (boost_obd_ble_host_up()) {
        app_ble_register_host_config();
    }

    s_evq = xQueueCreate(APP_BLE_QUEUE_LEN, sizeof(app_ble_ev_t));
    if (s_evq == NULL) {
        ESP_LOGE(TAG, "event queue alloc failed");
        return;
    }
    if (xTaskCreate(app_driver_task, "boost_app_ble", APP_BLE_TASK_STACK,
                    NULL, APP_BLE_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "driver task create failed");
    }
    ESP_LOGI(TAG, "companion BLE init; persisted=%s", s_enabled ? "ON" : "OFF");
}

bool boost_app_ble_enabled(void)
{
    return s_enabled;
}

void boost_app_ble_set_enabled(bool enabled)
{
    if (enabled == s_enabled) {
        return;
    }
    s_enabled = enabled;
    app_ble_nvs_persist(enabled);
    if (enabled) {
        boost_app_ble_start();
    } else {
        boost_app_ble_stop();
    }
}

void boost_app_ble_start(void)
{
    if (!s_init_done) {
        boost_app_ble_init();
    }
    /* Mount the shared host through the RAM-guarded central path (idempotent)
     * so appBle works even while tpmsBle is OFF. */
    boost_obd_ble_init();
    if (!boost_obd_ble_host_up()) {
        ESP_LOGE(TAG, "shared NimBLE host unavailable (RAM guard); BLE stays off");
        return;
    }
    /* Registration must land between mount and host-task start. */
    app_ble_register_host_config();
    boost_obd_ble_host_start();
    app_ble_ev_t ev = { 0 };
    ev.type = APP_EV_START;
    if (s_evq == NULL || xQueueSend(s_evq, &ev, 0) != pdTRUE) {
        ESP_LOGE(TAG, "start event queue full");
    }
}

void boost_app_ble_stop(void)
{
    app_ble_ev_t ev = { 0 };
    ev.type = APP_EV_STOP;
    if (s_evq != NULL) {
        (void)xQueueSend(s_evq, &ev, 0);
    }
}

bool boost_app_ble_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

#else /* APP_BLE_BUILT */

/* BLE-less image (CONFIG_BT_ENABLED=n, NimBLE off, or the OBD central
 * compiled out): keep the public API linkable and inert, exactly like
 * boost_obd_stub.c does for the central. */

static const char *TAG = "boost_app_ble";

void boost_app_ble_init(void)
{
    ESP_LOGI(TAG, "companion BLE not compiled (BLE-less build)");
}

bool boost_app_ble_enabled(void) { return false; }

void boost_app_ble_set_enabled(bool enabled) { (void)enabled; }

void boost_app_ble_start(void) { }

void boost_app_ble_stop(void) { }

bool boost_app_ble_connected(void) { return false; }

void boost_app_ble_set_passkey_display_cb(boost_app_ble_passkey_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
}

void boost_app_ble_set_pair_result_cb(boost_app_ble_pair_result_cb_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
}

#endif /* APP_BLE_BUILT */
