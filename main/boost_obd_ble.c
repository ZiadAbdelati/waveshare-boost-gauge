#include "boost_obd_ble.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"

/*
 * NimBLE host task -> driver task mailbox. The driver task owns every
 * ble_gap/ble_gattc call, so writes and the connect/discover/subscribe
 * sequence are serialised on one thread; the host callbacks only post events.
 *
 * Note: this NimBLE version removed ble_gattc_subscribe(); subscribing to the
 * adapter's RX characteristic is done by writing 0x0001 (notify) to its CCCD
 * descriptor.
 */
#define OBD_EV_QUEUE_LEN   12
#define OBD_MAX_WRITE      64
#define OBD_MAX_RX         256
#define OBD_MAX_CHR_CAND   8
#define OBD_ADV_SCAN_MS    3000
#define OBD_RECONNECT_MS   10000
/* When the adapter is consistently unreachable (bench, adapter unplugged) the
 * old loop scanned every ~13 s forever, saturating the shared 2.4 GHz radio
 * and spiking web latency. Back off the scan interval exponentially after each
 * failed cycle, capped, so an absent peer stops hammering the radio while a
 * present one is still found within one scan period. Reset on any connect. */
#define OBD_RECONNECT_MAX_MS 120000
#define OBD_CCCD_UUID      0x2902
/* The BT controller allocates its buffer pool from DMA-capable internal RAM,
 * and a failed allocation inside esp_bt_controller_enable() panics the board
 * (LoadProhibited) rather than returning an error. Refuse to init below this
 * largest-block floor so a RAM-starved board fails gracefully instead of
 * boot-looping. The controller needs more than the ~26.6 kB observed to fail. */
#define OBD_BLE_MIN_DMA_BLOCK 40960u

static const char *TAG = "boost_obd_ble";

#define NVS_NS          "boost"
#define NVS_KEY_OBD_PEER "obd_peer"

typedef enum {
    OBD_EV_START = 1,       /* (re)enter the connect/scan loop */
    OBD_EV_SCAN_DISC,       /* disc.addr filled */
    OBD_EV_SCAN_DONE,       /* scan ended without a match */
    OBD_EV_CONNECTED,       /* conn_handle valid */
    OBD_EV_CONNECT_FAIL,    /* connect attempt failed (status in a) */
    OBD_EV_DISCONNECTED,    /* link dropped (reason in a) */
    OBD_EV_SVC_DONE,        /* service discovery complete; a=start_handle b=end_handle */
    OBD_EV_DISC_DONE,       /* char discovery complete; chr[] + chr_count */
    OBD_EV_DISC_FAIL,
    OBD_EV_READY,           /* subscription + MTU done; link usable */
    OBD_EV_WRITE,           /* outbound bytes (len + data) */
    OBD_EV_STOP,
} obd_ev_t;

typedef struct {
    uint16_t def_handle;
    uint16_t val_handle;
    uint16_t properties;
    uint16_t uuid16;        /* 0 when the uuid is not 16-bit */
} obd_chr_cand_t;

typedef struct {
    uint32_t type;
    uint16_t conn_handle;
    uint16_t a;             /* status/reason, or svc start handle */
    uint16_t b;             /* svc end handle */
    ble_addr_t addr;        /* SCAN_DISC */
    uint8_t len;
    uint8_t data[OBD_MAX_WRITE];
    obd_chr_cand_t chr[OBD_MAX_CHR_CAND];
    uint8_t chr_count;
} obd_ble_event_t;

static QueueHandle_t s_evq;
static TaskHandle_t s_task;
static bool s_init_done;
static volatile bool s_enabled;

static volatile boost_obd_ble_state_t s_state = BOOST_OBD_BLE_DOWN;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static volatile uint16_t s_rx_handle;   /* notification source */
static volatile uint16_t s_tx_handle;   /* write target */

static boost_obd_ble_rx_cb_t s_rx_cb;
static void *s_rx_ctx;

static ble_addr_t s_peer;               /* last successful peer (for reconnect) */
static char s_peer_name[24];
static char s_peer_addr[32];
static uint32_t s_ready_start_ms;
static volatile uint16_t s_last_err;    /* last connect/discovery failure status */
static uint32_t s_backoff_ms = OBD_RECONNECT_MS; /* grows while peer is unreachable */

static uint8_t s_rx_tmp[OBD_MAX_RX];

static void publish_state(boost_obd_ble_state_t st) { s_state = st; }

static void format_addr(const ble_addr_t *addr, char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr->val[5], addr->val[4], addr->val[3], addr->val[2], addr->val[1], addr->val[0]);
}

static bool uuid16_matches(uint16_t u, const ble_uuid16_t *list, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (ble_uuid_u16(&list[i].u) == u) return true;
    }
    return false;
}

static void save_peer(const ble_addr_t *addr)
{
    s_peer = *addr;
    format_addr(addr, s_peer_addr, sizeof(s_peer_addr));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, NVS_KEY_OBD_PEER, addr, sizeof(*addr));
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool load_peer(ble_addr_t *addr)
{
    nvs_handle_t h;
    size_t len = sizeof(*addr);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    const esp_err_t err = nvs_get_blob(h, NVS_KEY_OBD_PEER, addr, &len);
    nvs_close(h);
    return err == ESP_OK && len == sizeof(*addr) && addr->val[0] != 0;
}

static void load_stored_peer(void)
{
    if (s_peer_addr[0] != '\0') return;
    ble_addr_t stored;
    if (load_peer(&stored)) {
        s_peer = stored;
        format_addr(&stored, s_peer_addr, sizeof(s_peer_addr));
        ESP_LOGI(TAG, "stored peer %s", s_peer_addr);
    }
}

static const ble_uuid16_t s_tx_uuids[] = {
    BLE_UUID16_INIT(0x2AF1),
    BLE_UUID16_INIT(0xFFF2),
    BLE_UUID16_INIT(0xFFE1),
};

static const ble_uuid16_t s_rx_uuids[] = {
    BLE_UUID16_INIT(0x2AF0),
    BLE_UUID16_INIT(0xFFF1),
};

static obd_chr_cand_t s_cands[OBD_MAX_CHR_CAND];
static uint8_t s_cand_count;
static bool s_cccd_found;   /* set when the RX CCCD descriptor is seen */

/* Service discovery: track the best ELM candidate service (known 16-bit UUIDs
 * preferred, then the first non-SIG service) and its bounded handle range. */
static uint16_t s_best_svc_start;
static uint16_t s_best_svc_end;

static int gatt_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;
    if (error->status == 0 && service != NULL) {
        const uint16_t u = (service->uuid.u.type == BLE_UUID_TYPE_16)
                               ? ble_uuid_u16(&service->uuid.u) : 0;
        if (u == 0x18F0 || u == 0xFFF0 || u == 0xFFE0) {
            s_best_svc_start = service->start_handle;
            s_best_svc_end = service->end_handle;
        } else if (s_best_svc_start == 0 && s_best_svc_end == 0 &&
                   !(service->uuid.u.type == BLE_UUID_TYPE_16 && u <= 0x180F)) {
            /* First non-SIG service as a fallback. */
            s_best_svc_start = service->start_handle;
            s_best_svc_end = service->end_handle;
        }
    } else if (error->status == BLE_HS_EDONE) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_SVC_DONE;
        ev.conn_handle = conn_handle;
        ev.a = s_best_svc_start;
        ev.b = s_best_svc_end;
        xQueueSend(s_evq, &ev, 0);
    } else if (error->status != 0) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_DISC_FAIL;
        ev.conn_handle = conn_handle;
        ev.a = (uint16_t)error->status;
        xQueueSend(s_evq, &ev, 0);
    }
    return 0;
}

static void start_svc_discovery(uint16_t conn_handle)
{
    s_best_svc_start = 0;
    s_best_svc_end = 0;
    const int rc = ble_gattc_disc_all_svcs(conn_handle, gatt_svc_cb, NULL);
    if (rc != 0) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_DISC_FAIL;
        ev.conn_handle = conn_handle;
        ev.a = (uint16_t)rc;
        xQueueSend(s_evq, &ev, 0);
    }
}

static int gatt_all_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;
    if (error->status == 0 && chr != NULL && s_cand_count < OBD_MAX_CHR_CAND) {
        obd_chr_cand_t *c = &s_cands[s_cand_count];
        c->def_handle = chr->def_handle;
        c->val_handle = chr->val_handle;
        c->properties = chr->properties;
        c->uuid16 = (chr->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&chr->uuid.u) : 0;
        if (c->uuid16 != 0) s_cand_count++;
    } else if (error->status == BLE_HS_EDONE) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_DISC_DONE;
        ev.conn_handle = conn_handle;
        ev.chr_count = s_cand_count;
        memcpy(ev.chr, s_cands, sizeof(s_cands));
        s_cand_count = 0;
        xQueueSend(s_evq, &ev, 0);
    } else if (error->status != 0) {
        s_cand_count = 0;
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_DISC_FAIL;
        ev.conn_handle = conn_handle;
        ev.a = (uint16_t)error->status;
        xQueueSend(s_evq, &ev, 0);
    }
    return 0;
}

static void start_char_discovery(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle)
{
    s_cand_count = 0;
    /* Bounded to the chosen service's handle range: walking the whole 1..0xFFFF
     * space makes the FD+ fail to terminate the "Read By Type" walk, and the
     * resume then overflows prev_handle+1 to 0 -> BLE_HS_EINVAL. */
    const int rc = ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle,
                                           gatt_all_chr_cb, NULL);
    if (rc != 0) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_DISC_FAIL;
        ev.conn_handle = conn_handle;
        ev.a = (uint16_t)rc;
        xQueueSend(s_evq, &ev, 0);
    }
}

static int gatt_cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                              struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    (void)arg;
    obd_ble_event_t ev = { 0 };
    ev.conn_handle = conn_handle;
    if (error->status == 0) {
        ev.type = OBD_EV_READY;
    } else {
        ev.type = OBD_EV_DISC_FAIL;
        ev.a = (uint16_t)error->status;
    }
    xQueueSend(s_evq, &ev, 0);
    return 0;
}

static int gatt_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)arg;
    (void)chr_val_handle;
    if (error->status == 0 && dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&dsc->uuid.u) == OBD_CCCD_UUID) {
            s_cccd_found = true;
            static const uint8_t notify_on[] = { 0x01, 0x00 };
            const int rc = ble_gattc_write_flat(conn_handle, dsc->handle, notify_on,
                                                sizeof(notify_on), gatt_cccd_write_cb, NULL);
            if (rc != 0) {
                obd_ble_event_t ev = { 0 };
                ev.type = OBD_EV_DISC_FAIL;
                ev.conn_handle = conn_handle;
                ev.a = (uint16_t)rc;
                xQueueSend(s_evq, &ev, 0);
            }
            return 0;
        }
    } else if (error->status == BLE_HS_EDONE) {
        /* Descriptor walk complete. If the CCCD was never seen, the adapter
         * cannot push notifications and the link is unusable. */
        if (!s_cccd_found) {
            ESP_LOGE(TAG, "no CCCD descriptor for RX characteristic");
            obd_ble_event_t ev = { 0 };
            ev.type = OBD_EV_DISC_FAIL;
            ev.conn_handle = conn_handle;
            ev.a = BLE_HS_ENOENT;
            xQueueSend(s_evq, &ev, 0);
        }
    }
    return 0;
}

static bool adv_matches(const struct ble_hs_adv_fields *f)
{
    if (f->uuids16 != NULL) {
        for (int i = 0; i < f->num_uuids16; ++i) {
            const uint16_t u = ble_uuid_u16(&f->uuids16[i].u);
            if (u == 0x18F0 || u == 0xFFF0 || u == 0xFFE0 || u == 0xFFF5) return true;
        }
    }
    if (f->name != NULL) {
        char lower[32];
        const size_t n = f->name_len < sizeof(lower) - 1 ? f->name_len : sizeof(lower) - 1;
        for (size_t i = 0; i < n; ++i) {
            lower[i] = (char)(f->name[i] >= 'A' && f->name[i] <= 'Z'
                                  ? f->name[i] - 'A' + 'a' : f->name[i]);
        }
        lower[n] = '\0';
        if (strstr(lower, "vlink") != NULL || strstr(lower, "obd") != NULL ||
            strstr(lower, "elm") != NULL) {
            strlcpy(s_peer_name, lower, sizeof(s_peer_name));
            return true;
        }
    }
    return false;
}

static int gap_event_fn(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    obd_ble_event_t ev = { 0 };
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (!adv_matches(&fields)) return 0;
        ev.type = OBD_EV_SCAN_DISC;
        ev.addr = event->disc.addr;
        ble_gap_disc_cancel(); /* best effort; ignore errors */
        xQueueSend(s_evq, &ev, 0);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE: {
        ev.type = OBD_EV_SCAN_DONE;
        xQueueSend(s_evq, &ev, 0);
        return 0;
    }
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
                save_peer(&desc.peer_ota_addr);
            }
            ev.type = OBD_EV_CONNECTED;
            ev.conn_handle = event->connect.conn_handle;
        } else {
            ev.type = OBD_EV_CONNECT_FAIL;
            ev.a = (uint16_t)event->connect.status;
        }
        xQueueSend(s_evq, &ev, 0);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        ev.type = OBD_EV_DISCONNECTED;
        ev.a = (uint16_t)event->disconnect.reason;
        xQueueSend(s_evq, &ev, 0);
        return 0;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (event->notify_rx.attr_handle == s_rx_handle && s_rx_cb != NULL &&
            event->notify_rx.om != NULL) {
            uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
            if (len > sizeof(s_rx_tmp)) len = (uint16_t)sizeof(s_rx_tmp);
            os_mbuf_copydata(event->notify_rx.om, 0, len, s_rx_tmp);
            s_rx_cb(s_rx_tmp, len, s_rx_ctx);
        }
        return 0;
    }
    default:
        return 0;
    }
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 0x0080,             /* 80 ms interval (was 10 ms continuous) */
        .window = 0x0020,           /* 20 ms window (25% duty cycle, frees 75% airtime for Wi-Fi) */
        .filter_duplicates = 1,  /* active scan so we get advertised names */
        .passive = 0,
        .limited = 0,
        .filter_policy = 0,
    };
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, OBD_ADV_SCAN_MS, &params,
                                gap_event_fn, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "scan start failed: %d", rc);
        publish_state(BOOST_OBD_BLE_DISCONNECTED);
    } else {
        publish_state(BOOST_OBD_BLE_SCANNING);
    }
}

static void try_connect(const ble_addr_t *addr)
{
    struct ble_gap_conn_params params = {
        .scan_itvl = 0x0080,
        .scan_window = 0x0020,
        .itvl_min = 16,             /* 20 ms */
        .itvl_max = 32,             /* 40 ms */
        .latency = 0,
        .supervision_timeout = 400, /* 4 s */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };
    publish_state(BOOST_OBD_BLE_CONNECTING);
    const int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, addr, 5000, &params,
                                   gap_event_fn, NULL);
    if (rc != 0) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_CONNECT_FAIL;
        ev.a = (uint16_t)rc;
        xQueueSend(s_evq, &ev, 0);
    }
}

static void driver_task(void *arg)
{
    (void)arg;
    bool first_pass = true;
    for (;;) {
        obd_ble_event_t ev;
        if (!xQueueReceive(s_evq, &ev, portMAX_DELAY)) continue;

        switch (ev.type) {
        case OBD_EV_START: {
            first_pass = true;
            s_backoff_ms = OBD_RECONNECT_MS;
            if (s_enabled) {
                load_stored_peer();
                if (s_peer_addr[0] != '\0') {
                    try_connect(&s_peer);
                } else {
                    start_scan();
                }
            }
            break;
        }
        case OBD_EV_STOP: {
            if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ble_gap_disc_cancel();
                ble_gap_conn_cancel();
            }
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ready_start_ms = 0;
            publish_state(BOOST_OBD_BLE_DOWN);
            break;
        }
        case OBD_EV_SCAN_DISC: {
            if (!s_enabled) break;
            try_connect(&ev.addr);
            break;
        }
        case OBD_EV_SCAN_DONE:
        case OBD_EV_CONNECT_FAIL: {
            if (!s_enabled) break;
            if (ev.type == OBD_EV_CONNECT_FAIL) {
                s_last_err = ev.a;
                ESP_LOGW(TAG, "connect failed: status=0x%04x", (unsigned)ev.a);
            }
            vTaskDelay(pdMS_TO_TICKS(first_pass ? 0 : s_backoff_ms));
            first_pass = false;
            if (s_backoff_ms < OBD_RECONNECT_MAX_MS) {
                s_backoff_ms = s_backoff_ms * 2 > OBD_RECONNECT_MAX_MS ? OBD_RECONNECT_MAX_MS : s_backoff_ms * 2;
            }
            if (s_enabled) start_scan();
            break;
        }
        case OBD_EV_CONNECTED: {
            if (!s_enabled) break;
            s_conn_handle = ev.conn_handle;
            s_last_err = 0;   /* new link: reset the failure marker */
            s_backoff_ms = OBD_RECONNECT_MS;   /* peer found: restore the fast retry cadence */
            publish_state(BOOST_OBD_BLE_DISCOVERING);
            start_svc_discovery(ev.conn_handle);
            break;
        }
        case OBD_EV_SVC_DONE: {
            if (!s_enabled) break;
            if (ev.a == 0 || ev.b == 0 || ev.b < ev.a) {
                ESP_LOGE(TAG, "no usable GATT service found; retrying link");
                s_last_err = 0xE002;   /* no candidate service */
                ble_gap_terminate(ev.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            ESP_LOGI(TAG, "service range 0x%04x..0x%04x", ev.a, ev.b);
            start_char_discovery(ev.conn_handle, ev.a, ev.b);
            break;
        }
        case OBD_EV_DISC_DONE: {
            /* Prefer a writable/notifiable char whose UUID is in the known ELM
             * tables; otherwise take the first writable / first notifiable. */
            uint16_t tx = 0, rx = 0, rx_def = 0;
            for (int i = 0; i < ev.chr_count; ++i) {
                const obd_chr_cand_t *c = &ev.chr[i];
                if ((c->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP)) &&
                    uuid16_matches(c->uuid16, s_tx_uuids, sizeof(s_tx_uuids) / sizeof(s_tx_uuids[0]))) {
                    tx = c->val_handle;
                }
                if ((c->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) &&
                    uuid16_matches(c->uuid16, s_rx_uuids, sizeof(s_rx_uuids) / sizeof(s_rx_uuids[0]))) {
                    rx = c->val_handle;
                    rx_def = c->def_handle;
                }
            }
            for (int i = 0; (tx == 0 || rx == 0) && i < ev.chr_count; ++i) {
                const obd_chr_cand_t *c = &ev.chr[i];
                if (tx == 0 && (c->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
                    tx = c->val_handle;
                }
                if (rx == 0 && (c->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
                    rx = c->val_handle;
                    rx_def = c->def_handle;
                }
            }
            if (tx == 0 || rx == 0) {
                ESP_LOGE(TAG, "no usable TX/RX characteristic (%u candidates)", (unsigned)ev.chr_count);
                s_last_err = 0xE001;   /* no usable TX/RX char */
                ble_gap_terminate(ev.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            s_tx_handle = tx;
            s_rx_handle = rx;
            ESP_LOGI(TAG, "adapter chars: write=0x%04x notify=0x%04x", tx, rx);
            /* The CCCD descriptor lives inside this characteristic's handle
             * range, i.e. after its value and before the next declaration.
             * ble_gattc_disc_all_dscs() starts at (start_handle + 1), so pass
             * the VALUE handle here, not the descriptor handle. */
            uint16_t dsc_hi = 0xFFFF;
            for (int i = 0; i < ev.chr_count; ++i) {
                if (ev.chr[i].def_handle > rx_def) {
                    dsc_hi = ev.chr[i].def_handle - 1;
                    break;
                }
            }
            s_cccd_found = false;
            const int rc = ble_gattc_disc_all_dscs(ev.conn_handle, rx, dsc_hi,
                                                   gatt_dsc_cb, NULL);
            if (rc != 0) {
                obd_ble_event_t ev2 = { 0 };
                ev2.type = OBD_EV_DISC_FAIL;
                ev2.conn_handle = ev.conn_handle;
                ev2.a = (uint16_t)rc;
                xQueueSend(s_evq, &ev2, 0);
            }
            break;
        }
        case OBD_EV_DISC_FAIL: {
            ESP_LOGW(TAG, "discovery failed (%u); retrying link", (unsigned)ev.a);
            s_last_err = ev.a;
            ble_gap_terminate(ev.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        case OBD_EV_READY: {
            if (!s_enabled) break;
            s_ready_start_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);
            publish_state(BOOST_OBD_BLE_READY);
            ESP_LOGI(TAG, "OBD link READY (%s %s)", s_peer_name, s_peer_addr);
            break;
        }
        case OBD_EV_DISCONNECTED: {
            ESP_LOGW(TAG, "link dropped (reason %u)", (unsigned)ev.a);
            if (s_last_err == 0) s_last_err = ev.a;   /* record first failure */
            s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            s_ready_start_ms = 0;
            publish_state(BOOST_OBD_BLE_DISCONNECTED);
            vTaskDelay(pdMS_TO_TICKS(OBD_RECONNECT_MS));
            if (s_enabled) {
                if (s_peer_addr[0] != '\0') {
                    try_connect(&s_peer);
                } else {
                    start_scan();
                }
            }
            break;
        }
        case OBD_EV_WRITE: {
            if (s_enabled && s_state == BOOST_OBD_BLE_READY &&
                ev.conn_handle == s_conn_handle && s_tx_handle != 0 && ev.len > 0) {
                const int rc = ble_gattc_write_flat(s_conn_handle, s_tx_handle,
                                                    ev.data, ev.len, NULL, NULL);
                if (rc != 0) ESP_LOGW(TAG, "write failed: %d", rc);
            }
            break;
        }
        default:
            break;
        }
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void boost_obd_ble_init(void)
{
    if (s_init_done) return;
    const uint32_t largest =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "heap before BLE: internal_free=%u largest_block=%u dma_free=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
    if (largest < OBD_BLE_MIN_DMA_BLOCK) {
        ESP_LOGE(TAG, "insufficient internal DMA RAM for BLE controller "
                      "(largest block %u, need %u); BLE disabled",
                 (unsigned)largest, (unsigned)OBD_BLE_MIN_DMA_BLOCK);
        return;
    }
    s_evq = xQueueCreate(OBD_EV_QUEUE_LEN, sizeof(obd_ble_event_t));
    if (s_evq == NULL) {
        ESP_LOGE(TAG, "event queue alloc failed");
        return;
    }
    const esp_err_t rc = nimble_port_init();
    if (rc != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(rc));
        ESP_LOGE(TAG, "heap at failure: internal_free=%u largest_block=%u dma_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
        return;
    }
    s_init_done = true;
    ESP_LOGI(TAG, "NimBLE host mounted (task start deferred)");
}

void boost_obd_ble_host_start(void)
{
    static bool s_host_started;
    if (!s_init_done || s_host_started) return;
    s_host_started = true;
    /* Starts the host task. GATT service registration must complete BEFORE
     * this call (boost_app_ble_init registers between mount and start). */
    nimble_port_freertos_init(host_task);
}

bool boost_obd_ble_host_up(void)
{
    return s_init_done;
}

void boost_obd_ble_start(void)
{
    if (!s_init_done) boost_obd_ble_init();
    if (!s_init_done) return;   /* host failed to come up (RAM guard); stay idle */
    boost_obd_ble_host_start();
    if (s_enabled) return;
    if (s_task == NULL) {
        if (xTaskCreate(driver_task, "boost_obd_ble", 4096, NULL, 6, &s_task) != pdPASS) {
            ESP_LOGE(TAG, "driver task create failed");
            return;
        }
    }
    s_enabled = true;
    obd_ble_event_t ev = { 0 };
    ev.type = OBD_EV_START;
    xQueueSend(s_evq, &ev, 0);
}

void boost_obd_ble_stop(void)
{
    if (!s_enabled) return;
    s_enabled = false;
    if (s_task != NULL) {
        obd_ble_event_t ev = { 0 };
        ev.type = OBD_EV_STOP;
        xQueueSend(s_evq, &ev, 0);
    }
}

bool boost_obd_ble_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0 || len > OBD_MAX_WRITE) return false;
    if (s_state != BOOST_OBD_BLE_READY || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) return false;
    obd_ble_event_t ev = { 0 };
    ev.type = OBD_EV_WRITE;
    ev.conn_handle = s_conn_handle;
    ev.len = (uint8_t)len;
    memcpy(ev.data, data, len);
    return xQueueSend(s_evq, &ev, 0) == pdTRUE;
}

void boost_obd_ble_set_rx_cb(boost_obd_ble_rx_cb_t cb, void *ctx)
{
    s_rx_cb = cb;
    s_rx_ctx = ctx;
}

boost_obd_ble_state_t boost_obd_ble_state(void) { return s_state; }

/* Last link-failure status (NimBLE error code); 0 while healthy. Exposed so the
 * dashboard can distinguish "adapter not in range" from "connect refused". */
uint16_t boost_obd_ble_last_error(void) { return s_last_err; }

const char *boost_obd_ble_peer_name(void) { return s_peer_name; }
const char *boost_obd_ble_peer_addr(void) { return s_peer_addr; }

uint32_t boost_obd_ble_uptime_ms(void)
{
    if (s_state != BOOST_OBD_BLE_READY || s_ready_start_ms == 0) return 0;
    return (uint32_t)(esp_timer_get_time() / 1000ULL) - s_ready_start_ms;
}
