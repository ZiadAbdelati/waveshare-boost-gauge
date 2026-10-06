/*
 * Fake NimBLE host/controller + FreeRTOS + NVS + esp_* host harness.
 *
 * Purpose: compile and *drive* the real main/boost_obd_ble.c on the host so
 * the connect/disable/reenable lifecycle can be asserted deterministically.
 * It is not a Bluetooth stack; it reproduces only the ordering facts that
 * make the phantom-connection defect observable:
 *
 *  - ble_gap_connect() admission: EALREADY (procedure active), EBUSY (scan
 *    active), ENOMEM (pool full), EDONE (a connection to that peer exists).
 *  - an accepted connection is a record in a pool with NO cleanup for an
 *    ignored BLE_GAP_EVENT_CONNECT; records are freed only by
 *    ble_gap_terminate() or a real link loss.
 *  - ble_gap_conn_cancel() returns EALREADY when no connect procedure is
 *    active and never tears down an established connection; when a procedure
 *    *is* active it only requests a cancel, and a racing completion still
 *    wins (matching ble_gap_rx_conn_complete() accepting status 0).
 *  - GAP callbacks run on a dedicated controller thread, i.e. asynchronously
 *    relative to the thread that called ble_gap_connect().
 */
#include "nimble_fake.h"

#include "boost_app_ble.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/fake_freertos.h"
#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"

#define FAKE_MAX_CONNECTIONS 3     /* CONFIG_BT_NIMBLE_MAX_CONNECTIONS */
#define FAKE_MAX_DELIVERIES  32
#define FAKE_NVS_MAX         64

/* ------------------------------------------------------------------ time */

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000);
}

int64_t esp_timer_get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + (int64_t)(ts.tv_nsec / 1000);
}

static struct timespec rt_deadline_ms(uint64_t from_now_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + (from_now_ms % 1000ULL) * 1000000ULL;
    ts.tv_sec += (time_t)(from_now_ms / 1000ULL) + (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    return ts;
}

/* --------------------------------------------------------- esp_heap_caps */

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 192 * 1024;   /* well above the driver's 40 KiB DMA floor */
}

size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 224 * 1024;
}

const char *esp_err_to_name(esp_err_t err)
{
    return err == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

/* -------------------------------------------------------------- FreeRTOS */

struct fake_queue {
    pthread_mutex_t m;
    pthread_cond_t  c;
    size_t          len;
    size_t          item_size;
    size_t          count;
    size_t          head;
    size_t          tail;
    unsigned char  *buf;
};

QueueHandle_t xQueueCreate(UBaseType_t uxQueueLength, UBaseType_t uxItemSize)
{
    struct fake_queue *q = (struct fake_queue *)calloc(1, sizeof(*q));
    if (q == NULL) return NULL;
    pthread_mutex_init(&q->m, NULL);
    pthread_cond_init(&q->c, NULL);
    q->len = uxQueueLength;
    q->item_size = uxItemSize;
    q->buf = (unsigned char *)calloc(uxQueueLength ? uxQueueLength : 1, uxItemSize ? uxItemSize : 1);
    if (q->buf == NULL) { free(q); return NULL; }
    return q;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)ticks;
    if (q == NULL || item == NULL) return pdFALSE;
    pthread_mutex_lock(&q->m);
    if (q->count == q->len) { pthread_mutex_unlock(&q->m); return pdFALSE; }
    memcpy(q->buf + q->head * q->item_size, item, q->item_size);
    q->head = (q->head + 1) % q->len;
    q->count++;
    pthread_cond_broadcast(&q->c);
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks)
{
    if (q == NULL || out == NULL) return pdFALSE;
    pthread_mutex_lock(&q->m);
    while (q->count == 0) {
        if (ticks == portMAX_DELAY) {
            pthread_cond_wait(&q->c, &q->m);
        } else {
            struct timespec ts = rt_deadline_ms((uint64_t)ticks);
            if (pthread_cond_timedwait(&q->c, &q->m, &ts) == ETIMEDOUT && q->count == 0) {
                pthread_mutex_unlock(&q->m);
                return pdFALSE;
            }
        }
    }
    memcpy(out, q->buf + q->tail * q->item_size, q->item_size);
    q->tail = (q->tail + 1) % q->len;
    q->count--;
    pthread_mutex_unlock(&q->m);
    return pdTRUE;
}

struct fake_task { pthread_t th; };

static void *task_trampoline(void *p)
{
    void **args = (void **)p;
    TaskFunction_t fn = (TaskFunction_t)args[0];
    void *arg = args[1];
    free(args);
    fn(arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t task, const char *name, uint32_t stack_depth,
                       void *arg, UBaseType_t priority, TaskHandle_t *out)
{
    (void)stack_depth; (void)priority; (void)name;
    void **args = (void **)malloc(sizeof(void *) * 2);
    if (args == NULL) return pdFAIL;
    args[0] = (void *)task;
    args[1] = arg;
    struct fake_task *t = (struct fake_task *)calloc(1, sizeof(*t));
    if (t == NULL) { free(args); return pdFAIL; }
    if (pthread_create(&t->th, NULL, task_trampoline, args) != 0) {
        free(t); free(args);
        return pdFAIL;
    }
    if (out != NULL) *out = t;
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    struct timespec req;
    req.tv_sec = (time_t)(ticks / 1000);
    req.tv_nsec = (long)((ticks % 1000) * 1000000UL);
    while (nanosleep(&req, &req) == -1 && errno == EINTR) { }
}

struct fake_sem { pthread_mutex_t m; };

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    struct fake_sem *s = (struct fake_sem *)calloc(1, sizeof(*s));
    if (s == NULL) return NULL;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s->m, &a);
    pthread_mutexattr_destroy(&a);
    return s;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)ticks;
    if (s == NULL) return pdFALSE;
    pthread_mutex_lock(&s->m);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    if (s == NULL) return pdFALSE;
    pthread_mutex_unlock(&s->m);
    return pdTRUE;
}

/* ------------------------------------------------------------------- NVS */

static pthread_mutex_t g_nvs_lock = PTHREAD_MUTEX_INITIALIZER;
static bool     g_nvs_has;
static uint8_t  g_nvs_blob[FAKE_NVS_MAX];
static size_t   g_nvs_len;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)name; (void)mode;
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    *out = 1;   /* single-slot store */
    return ESP_OK;
}

void nvs_close(nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *value, size_t length)
{
    (void)h; (void)key;
    if (value == NULL || length > FAKE_NVS_MAX) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_nvs_lock);
    memcpy(g_nvs_blob, value, length);
    g_nvs_len = length;
    g_nvs_has = true;
    pthread_mutex_unlock(&g_nvs_lock);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *length)
{
    (void)h; (void)key;
    if (out == NULL || length == NULL) return ESP_ERR_INVALID_ARG;
    pthread_mutex_lock(&g_nvs_lock);
    if (!g_nvs_has || *length < g_nvs_len) {
        pthread_mutex_unlock(&g_nvs_lock);
        return ESP_ERR_NVS_NOT_FOUND;
    }
    memcpy(out, g_nvs_blob, g_nvs_len);
    *length = g_nvs_len;
    pthread_mutex_unlock(&g_nvs_lock);
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{
    (void)h; (void)key;
    pthread_mutex_lock(&g_nvs_lock);
    g_nvs_has = false; g_nvs_len = 0;
    pthread_mutex_unlock(&g_nvs_lock);
    return ESP_OK;
}

esp_err_t nvs_flash_init(void) { return ESP_OK; }
esp_err_t nvs_flash_erase(void) { return ESP_OK; }

/* -------------------------------------------------------------- os_mbuf */

int os_mbuf_copydata(const struct os_mbuf *m, int off, int len, void *dst)
{
    if (m == NULL || dst == NULL || off < 0 || len < 0) return -1;
    if (off + len > (int)sizeof(m->omp_data)) return -1;
    memcpy(dst, m->omp_data + off, (size_t)len);
    return 0;
}

/* --------------------------------------------------------- UUID / adv */

uint16_t ble_uuid_u16(const ble_uuid_t *uuid)
{
    if (uuid == NULL || uuid->type != BLE_UUID_TYPE_16) return 0;
    return ((const ble_uuid16_t *)uuid)->value;
}

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields,
                            const uint8_t *data, uint8_t length)
{
    (void)data; (void)length;
    if (fields != NULL) memset(fields, 0, sizeof(*fields));
    return 0;
}

int ble_hs_util_ensure_addr(int prefer_random) { (void)prefer_random; return 0; }

/* ------------------------------------------------------ fake controller */

typedef struct {
    bool     used;
    uint16_t handle;
    ble_addr_t addr;
} fake_conn_t;

typedef enum {
    DEL_CONNECT = 1,
    DEL_DISCONNECT,
    DEL_DISC_COMPLETE,
} del_type_t;

typedef struct {
    del_type_t         type;
    int                status;
    uint16_t           handle;
    ble_gap_event_fn  *cb;
    void              *arg;
} fake_delivery_t;

static pthread_mutex_t g_lock;
static pthread_cond_t  g_cond;
static pthread_once_t  g_init_once = PTHREAD_ONCE_INIT;
static bool            g_lock_ready;

static fake_conn_t     g_conns[FAKE_MAX_CONNECTIONS];
static uint16_t        g_next_handle = 1;

static bool            g_connect_proc;
static bool            g_connect_cancel_req;
static ble_addr_t      g_connect_addr;
static ble_gap_event_fn *g_connect_cb;
static void           *g_connect_arg;
static uint64_t        g_connect_due_ms;
static int             g_last_connect_rc;
static uint32_t        g_last_disc_reason;

static bool            g_auto_connect = true;
static uint32_t        g_connect_delay_ms = 20;

static bool            g_scan;
static ble_gap_event_fn *g_scan_cb;
static void           *g_scan_arg;
static uint64_t        g_scan_deadline_ms;
static int32_t         g_scan_dur_ms;

/* cb used to deliver asynchronous disconnect / link-loss events */
static ble_gap_event_fn *g_gap_cb;
static void           *g_gap_arg;

static fake_delivery_t g_del[FAKE_MAX_DELIVERIES];
static unsigned        g_del_head, g_del_tail;
static bool            g_ctrl_started;

static void init_once(void)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_lock, &a);
    pthread_mutexattr_destroy(&a);
    pthread_cond_init(&g_cond, NULL);
    g_lock_ready = true;
}

static void ensure_init(void)
{
    pthread_once(&g_init_once, init_once);
}

static void del_push(del_type_t type, int status, uint16_t handle,
                     ble_gap_event_fn *cb, void *arg)
{
    unsigned next = (g_del_head + 1) % FAKE_MAX_DELIVERIES;
    if (next == g_del_tail) return;   /* full: drop (never happens in tests) */
    g_del[g_del_head].type = type;
    g_del[g_del_head].status = status;
    g_del[g_del_head].handle = handle;
    g_del[g_del_head].cb = cb;
    g_del[g_del_head].arg = arg;
    g_del_head = next;
    pthread_cond_broadcast(&g_cond);
}

static int conn_count_locked(void)
{
    int n = 0;
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) if (g_conns[i].used) n++;
    return n;
}

static fake_conn_t *conn_find_locked(uint16_t handle)
{
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) {
        if (g_conns[i].used && g_conns[i].handle == handle) return &g_conns[i];
    }
    return NULL;
}

static fake_conn_t *conn_find_addr_locked(const ble_addr_t *addr)
{
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) {
        if (g_conns[i].used && g_conns[i].addr.type == addr->type &&
            memcmp(g_conns[i].addr.val, addr->val, 6) == 0) {
            return &g_conns[i];
        }
    }
    return NULL;
}

/* Create the connection record and queue the CONNECT(status 0) delivery.
 * Called with g_lock held. */
static void complete_connect_locked(void)
{
    int slot = -1;
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) if (!g_conns[i].used) { slot = i; break; }
    if (slot < 0) { g_connect_proc = false; return; }

    g_conns[slot].used = true;
    g_conns[slot].handle = g_next_handle++;
    g_conns[slot].addr = g_connect_addr;

    ble_gap_event_fn *cb = g_connect_cb;
    void *arg = g_connect_arg;
    uint16_t handle = g_conns[slot].handle;

    g_connect_proc = false;
    g_connect_cancel_req = false;
    g_gap_cb = cb;
    g_gap_arg = arg;

    del_push(DEL_CONNECT, 0, handle, cb, arg);
}

static void deliver(const fake_delivery_t *d)
{
    struct ble_gap_event ev;
    memset(&ev, 0, sizeof(ev));
    switch (d->type) {
    case DEL_CONNECT:
        ev.type = BLE_GAP_EVENT_CONNECT;
        ev.connect.status = d->status;
        ev.connect.conn_handle = d->handle;
        break;
    case DEL_DISCONNECT:
        ev.type = BLE_GAP_EVENT_DISCONNECT;
        ev.disconnect.reason = (uint8_t)d->status;
        ev.disconnect.conn_handle = d->handle;
        break;
    case DEL_DISC_COMPLETE:
        ev.type = BLE_GAP_EVENT_DISC_COMPLETE;
        ev.disc_complete.reason = d->status;
        break;
    default:
        return;
    }
    if (d->cb != NULL) d->cb(&ev, d->arg);
}

static void *ctrl_main(void *unused)
{
    (void)unused;
    for (;;) {
        fake_delivery_t d;
        bool have = false;

        pthread_mutex_lock(&g_lock);
        {
            const uint64_t now = mono_ms();
            if (g_connect_proc && g_auto_connect && now >= g_connect_due_ms) {
                complete_connect_locked();
            } else if (g_scan && g_scan_dur_ms >= 0 && now >= g_scan_deadline_ms) {
                g_scan = false;
                del_push(DEL_DISC_COMPLETE, BLE_HS_ETIMEOUT, BLE_HS_CONN_HANDLE_NONE,
                         g_scan_cb, g_scan_arg);
            }
        }
        if (g_del_head != g_del_tail) {
            d = g_del[g_del_tail];
            g_del_tail = (g_del_tail + 1) % FAKE_MAX_DELIVERIES;
            have = true;
        } else {
            const uint64_t now = mono_ms();
            if (g_connect_proc && g_auto_connect) {
                uint64_t wait = g_connect_due_ms > now ? g_connect_due_ms - now : 0;
                struct timespec ts = rt_deadline_ms(wait);
                pthread_cond_timedwait(&g_cond, &g_lock, &ts);
            } else if (g_scan && g_scan_dur_ms >= 0) {
                uint64_t wait = g_scan_deadline_ms > now ? g_scan_deadline_ms - now : 0;
                struct timespec ts = rt_deadline_ms(wait);
                pthread_cond_timedwait(&g_cond, &g_lock, &ts);
            } else {
                pthread_cond_wait(&g_cond, &g_lock);
            }
        }
        pthread_mutex_unlock(&g_lock);

        if (have) deliver(&d);
    }
    return NULL;
}

/* -------------------------------------------------- nimble_port / host */

static pthread_mutex_t g_port_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_port_cond = PTHREAD_COND_INITIALIZER;
static bool            g_port_running;
static bool            g_port_stop_requested;

esp_err_t nimble_port_init(void)
{
    ensure_init();
    pthread_mutex_lock(&g_lock);
    if (!g_ctrl_started) {
        g_ctrl_started = true;
        pthread_t th;
        (void)pthread_create(&th, NULL, ctrl_main, NULL);
    }
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_port_lock);
    g_port_running = true;
    g_port_stop_requested = false;
    pthread_mutex_unlock(&g_port_lock);
    return ESP_OK;
}

void nimble_port_run(void)
{
    pthread_mutex_lock(&g_port_lock);
    while (!g_port_stop_requested) pthread_cond_wait(&g_port_cond, &g_port_lock);
    pthread_mutex_unlock(&g_port_lock);
}

int nimble_port_stop(void)
{
    pthread_mutex_lock(&g_port_lock);
    g_port_stop_requested = true;
    pthread_cond_broadcast(&g_port_cond);
    pthread_mutex_unlock(&g_port_lock);
    return 0;
}

esp_err_t nimble_port_deinit(void)
{
    pthread_mutex_lock(&g_port_lock);
    g_port_running = false;
    g_port_stop_requested = true;
    pthread_cond_broadcast(&g_port_cond);
    pthread_mutex_unlock(&g_port_lock);
    return ESP_OK;
}

static void *host_trampoline(void *p)
{
    void **args = (void **)p;
    TaskFunction_t fn = (TaskFunction_t)args[0];
    free(args);
    fn(NULL);
    return NULL;
}

void nimble_port_freertos_init(TaskFunction_t host_task_fn)
{
    ensure_init();
    void **args = (void **)malloc(sizeof(void *) * 2);
    if (args == NULL) return;
    args[0] = (void *)host_task_fn;
    pthread_t th;
    if (pthread_create(&th, NULL, host_trampoline, args) != 0) { free(args); return; }
}

void nimble_port_freertos_deinit(void)
{
    nimble_port_stop();
}

/* -------------------------------------------------------------- GATT */

/* The ELM-style profile the driver discovers:
 *   svc 0x18F0 @ 0x0010..0x001F
 *     chr 0xFFF2 def 0x0011 val 0x0012 props WRITE|WRITE_NO_RSP   (TX)
 *     chr 0xFFF1 def 0x0013 val 0x0014 props NOTIFY               (RX)
 *       dsc 0x2902 (CCCD) @ 0x0015
 */
#define FAKE_SVC_START   0x0010
#define FAKE_SVC_END     0x001F
#define FAKE_TX_DEF      0x0011
#define FAKE_TX_VAL      0x0012
#define FAKE_RX_DEF      0x0013
#define FAKE_RX_VAL      0x0014
#define FAKE_CCCD_HANDLE 0x0015

static bool conn_alive(uint16_t handle)
{
    pthread_mutex_lock(&g_lock);
    bool ok = conn_find_locked(handle) != NULL;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_disc_svc_fn *cb, void *cb_arg)
{
    if (!conn_alive(conn_handle)) return BLE_HS_ENOTCONN;
    if (cb == NULL) return 0;

    struct ble_gatt_error err;
    struct ble_gatt_svc svc;
    memset(&err, 0, sizeof(err));
    memset(&svc, 0, sizeof(svc));
    svc.start_handle = FAKE_SVC_START;
    svc.end_handle = FAKE_SVC_END;
    svc.uuid.u16.u.type = BLE_UUID_TYPE_16;
    svc.uuid.u16.value = 0x18F0;
    cb(conn_handle, &err, &svc, cb_arg);

    err.status = BLE_HS_EDONE;
    cb(conn_handle, &err, NULL, cb_arg);
    return 0;
}

int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, ble_gatt_chr_fn *cb, void *cb_arg)
{
    if (!conn_alive(conn_handle)) return BLE_HS_ENOTCONN;
    if (cb == NULL) return 0;

    struct ble_gatt_error err;
    struct ble_gatt_chr chr;
    memset(&err, 0, sizeof(err));

    if (FAKE_TX_DEF >= start_handle && FAKE_TX_DEF <= end_handle) {
        memset(&chr, 0, sizeof(chr));
        chr.def_handle = FAKE_TX_DEF;
        chr.val_handle = FAKE_TX_VAL;
        chr.properties = BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP;
        chr.uuid.u16.u.type = BLE_UUID_TYPE_16;
        chr.uuid.u16.value = 0xFFF2;
        cb(conn_handle, &err, &chr, cb_arg);
    }
    if (FAKE_RX_DEF >= start_handle && FAKE_RX_DEF <= end_handle) {
        memset(&chr, 0, sizeof(chr));
        chr.def_handle = FAKE_RX_DEF;
        chr.val_handle = FAKE_RX_VAL;
        chr.properties = BLE_GATT_CHR_PROP_NOTIFY;
        chr.uuid.u16.u.type = BLE_UUID_TYPE_16;
        chr.uuid.u16.value = 0xFFF1;
        cb(conn_handle, &err, &chr, cb_arg);
    }

    err.status = BLE_HS_EDONE;
    cb(conn_handle, &err, NULL, cb_arg);
    return 0;
}

int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t start_handle,
                            uint16_t end_handle, ble_gatt_dsc_fn *cb, void *cb_arg)
{
    if (!conn_alive(conn_handle)) return BLE_HS_ENOTCONN;
    if (cb == NULL) return 0;

    struct ble_gatt_error err;
    memset(&err, 0, sizeof(err));

    if (FAKE_CCCD_HANDLE >= start_handle && FAKE_CCCD_HANDLE <= end_handle) {
        struct ble_gatt_dsc dsc;
        memset(&dsc, 0, sizeof(dsc));
        dsc.handle = FAKE_CCCD_HANDLE;
        dsc.uuid.u16.u.type = BLE_UUID_TYPE_16;
        dsc.uuid.u16.value = 0x2902;
        cb(conn_handle, &err, FAKE_RX_VAL, &dsc, cb_arg);
    }

    err.status = BLE_HS_EDONE;
    cb(conn_handle, &err, FAKE_RX_VAL, NULL, cb_arg);
    return 0;
}

int ble_gattc_write_flat(uint16_t conn_handle, uint16_t attr_handle,
                         const void *data, uint16_t data_len,
                         ble_gatt_attr_fn *cb, void *cb_arg)
{
    if (!conn_alive(conn_handle)) return BLE_HS_ENOTCONN;
    (void)data; (void)data_len;
    if (cb != NULL) {
        struct ble_gatt_error err;
        struct ble_gatt_attr attr;
        memset(&err, 0, sizeof(err));
        memset(&attr, 0, sizeof(attr));
        attr.handle = attr_handle;
        cb(conn_handle, &err, &attr, cb_arg);
    }
    return 0;
}

/* ---------------------------------------------------------- GAP: query */

int ble_gap_conn_active(void)
{
    pthread_mutex_lock(&g_lock);
    int active = g_connect_proc ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    return active;
}

int ble_gap_conn_find(uint16_t handle, struct ble_gap_conn_desc *out)
{
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_locked(handle);
    if (c == NULL) { pthread_mutex_unlock(&g_lock); return BLE_HS_ENOENT; }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->conn_handle = c->handle;
        out->peer_ota_addr = c->addr;
        out->peer_id_addr = c->addr;
        out->role = 0;   /* central */
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

int ble_gap_conn_find_by_addr(const ble_addr_t *addr, struct ble_gap_conn_desc *out)
{
    if (addr == NULL) return BLE_HS_EINVAL;
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_addr_locked(addr);
    if (c == NULL) { pthread_mutex_unlock(&g_lock); return BLE_HS_ENOENT; }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->conn_handle = c->handle;
        out->peer_ota_addr = c->addr;
        out->peer_id_addr = c->addr;
    }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

struct ble_hs_conn *ble_hs_conn_find_by_addr(const ble_addr_t *addr)
{
    if (addr == NULL) return NULL;
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_addr_locked(addr);
    pthread_mutex_unlock(&g_lock);
    return (struct ble_hs_conn *)c;
}

/* --------------------------------------------------------- GAP: connect */

int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer_addr,
                    int32_t duration_ms,
                    const struct ble_gap_conn_params *conn_params,
                    ble_gap_event_fn *cb, void *cb_arg)
{
    (void)own_addr_type; (void)duration_ms; (void)conn_params;
    if (peer_addr == NULL || cb == NULL) return BLE_HS_EINVAL;

    pthread_mutex_lock(&g_lock);
    int rc;
    if (g_connect_proc) {
        rc = BLE_HS_EALREADY;
    } else if (g_scan) {
        rc = BLE_HS_EBUSY;
    } else if (conn_count_locked() >= FAKE_MAX_CONNECTIONS) {
        rc = BLE_HS_ENOMEM;
    } else if (conn_find_addr_locked(peer_addr) != NULL) {
        rc = BLE_HS_EDONE;
    } else {
        g_connect_proc = true;
        g_connect_cancel_req = false;
        g_connect_addr = *peer_addr;
        g_connect_cb = cb;
        g_connect_arg = cb_arg;
        g_connect_due_ms = mono_ms() + g_connect_delay_ms;
        g_gap_cb = cb;
        g_gap_arg = cb_arg;
        rc = 0;
        pthread_cond_broadcast(&g_cond);
    }
    g_last_connect_rc = rc;
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int ble_gap_conn_cancel(void)
{
    pthread_mutex_lock(&g_lock);
    int rc;
    if (!g_connect_proc) {
        rc = BLE_HS_EALREADY;      /* no procedure: cannot cancel a live link */
    } else {
        g_connect_cancel_req = true;   /* HCI cancel in flight; completion may win */
        pthread_cond_broadcast(&g_cond);
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int ble_gap_terminate(uint16_t conn_handle, uint8_t hci_reason)
{
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_locked(conn_handle);
    if (c == NULL) { pthread_mutex_unlock(&g_lock); return BLE_HS_ENOENT; }
    c->used = false;
    g_last_disc_reason = hci_reason;
    del_push(DEL_DISCONNECT, hci_reason, conn_handle, g_gap_cb, g_gap_arg);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

/* ------------------------------------------------------------ GAP: scan */

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *disc_params,
                 ble_gap_event_fn *cb, void *cb_arg)
{
    (void)own_addr_type; (void)disc_params;
    if (cb == NULL) return BLE_HS_EINVAL;

    pthread_mutex_lock(&g_lock);
    int rc;
    if (g_connect_proc) {
        rc = BLE_HS_EBUSY;                 /* a connect procedure owns the radio */
    } else if (conn_count_locked() > 0) {
        rc = BLE_HS_EBUSY;                 /* a live link owns the radio */
    } else if (g_scan) {
        rc = BLE_HS_EALREADY;
    } else {
        g_scan = true;
        g_scan_cb = cb;
        g_scan_arg = cb_arg;
        g_scan_dur_ms = duration_ms;
        g_scan_deadline_ms = mono_ms() + (duration_ms > 0 ? (uint64_t)duration_ms : 0);
        g_gap_cb = cb;
        g_gap_arg = cb_arg;
        rc = 0;
        pthread_cond_broadcast(&g_cond);
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int ble_gap_disc_cancel(void)
{
    pthread_mutex_lock(&g_lock);
    int rc;
    if (!g_scan) {
        rc = BLE_HS_EALREADY;
    } else {
        g_scan = false;
        del_push(DEL_DISC_COMPLETE, BLE_HS_EAPP, BLE_HS_CONN_HANDLE_NONE, g_scan_cb, g_scan_arg);
        rc = 0;
    }
    pthread_mutex_unlock(&g_lock);
    return rc;
}

/* ------------------------------------------------- test control surface */

void fake_reset(void)
{
    ensure_init();
    pthread_mutex_lock(&g_lock);
    memset(g_conns, 0, sizeof(g_conns));
    g_next_handle = 1;
    g_connect_proc = false;
    g_connect_cancel_req = false;
    memset(&g_connect_addr, 0, sizeof(g_connect_addr));
    g_connect_cb = NULL; g_connect_arg = NULL; g_connect_due_ms = 0;
    g_last_connect_rc = 0;
    g_last_disc_reason = 0;
    g_auto_connect = true;
    g_connect_delay_ms = 20;
    g_scan = false; g_scan_cb = NULL; g_scan_arg = NULL;
    g_scan_deadline_ms = 0; g_scan_dur_ms = 0;
    g_gap_cb = NULL; g_gap_arg = NULL;
    g_del_head = g_del_tail = 0;
    pthread_mutex_unlock(&g_lock);

    pthread_mutex_lock(&g_nvs_lock);
    g_nvs_has = false; g_nvs_len = 0;
    pthread_mutex_unlock(&g_nvs_lock);
}

void fake_nvs_seed_peer(const ble_addr_t *addr)
{
    if (addr == NULL) return;
    pthread_mutex_lock(&g_nvs_lock);
    memcpy(g_nvs_blob, addr, sizeof(*addr));
    g_nvs_len = sizeof(*addr);
    g_nvs_has = true;
    pthread_mutex_unlock(&g_nvs_lock);
}

void fake_set_auto_connect(bool on)
{
    pthread_mutex_lock(&g_lock);
    g_auto_connect = on;
    pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_lock);
}

void fake_set_connect_delay_ms(uint32_t ms)
{
    pthread_mutex_lock(&g_lock);
    g_connect_delay_ms = ms;
    pthread_mutex_unlock(&g_lock);
}

bool fake_conn_pending(void)
{
    pthread_mutex_lock(&g_lock);
    bool p = g_connect_proc;
    pthread_mutex_unlock(&g_lock);
    return p;
}

bool fake_conn_complete_now(void)
{
    pthread_mutex_lock(&g_lock);
    bool had = g_connect_proc;
    if (had) complete_connect_locked();
    pthread_mutex_unlock(&g_lock);
    return had;
}

bool fake_conn_cancel_complete(void)
{
    pthread_mutex_lock(&g_lock);
    bool had = g_connect_proc;
    if (had) {
        g_connect_proc = false;
        g_connect_cancel_req = false;
        del_push(DEL_CONNECT, BLE_HS_EAPP, BLE_HS_CONN_HANDLE_NONE, g_connect_cb, g_connect_arg);
    }
    pthread_mutex_unlock(&g_lock);
    return had;
}

bool fake_drop_link(uint16_t conn_handle)
{
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_locked(conn_handle);
    bool had = c != NULL;
    if (had) {
        c->used = false;
        g_last_disc_reason = 0x08;   /* connection timeout */
        del_push(DEL_DISCONNECT, 0x08, conn_handle, g_gap_cb, g_gap_arg);
    }
    pthread_mutex_unlock(&g_lock);
    return had;
}

int fake_conn_count(void)
{
    pthread_mutex_lock(&g_lock);
    int n = conn_count_locked();
    pthread_mutex_unlock(&g_lock);
    return n;
}

bool fake_conn_addr(int index, ble_addr_t *out_addr, uint16_t *out_handle)
{
    pthread_mutex_lock(&g_lock);
    int seen = 0;
    bool ok = false;
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) {
        if (!g_conns[i].used) continue;
        if (seen == index) {
            if (out_addr) *out_addr = g_conns[i].addr;
            if (out_handle) *out_handle = g_conns[i].handle;
            ok = true;
            break;
        }
        seen++;
    }
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool fake_conn_handle_of(const ble_addr_t *addr, uint16_t *out_handle)
{
    if (addr == NULL) return false;
    pthread_mutex_lock(&g_lock);
    fake_conn_t *c = conn_find_addr_locked(addr);
    if (c != NULL && out_handle) *out_handle = c->handle;
    pthread_mutex_unlock(&g_lock);
    return c != NULL;
}

int fake_last_connect_rc(void)
{
    pthread_mutex_lock(&g_lock);
    int rc = g_last_connect_rc;
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int fake_scan_active(void)
{
    pthread_mutex_lock(&g_lock);
    int s = g_scan ? 1 : 0;
    pthread_mutex_unlock(&g_lock);
    return s;
}

uint32_t fake_last_disconnect_reason(void)
{
    pthread_mutex_lock(&g_lock);
    uint32_t r = g_last_disc_reason;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void format_addr(const ble_addr_t *a, char *buf, size_t len)
{
    snprintf(buf, len, "%02x:%02x:%02x:%02x:%02x:%02x",
             a->val[5], a->val[4], a->val[3], a->val[2], a->val[1], a->val[0]);
}

void fake_conn_table_str(char *buf, size_t len)
{
    if (buf == NULL || len == 0) return;
    size_t off = 0;
    pthread_mutex_lock(&g_lock);
    off += (size_t)snprintf(buf + off, len - off, "[");
    bool first = true;
    for (int i = 0; i < FAKE_MAX_CONNECTIONS; ++i) {
        if (!g_conns[i].used) continue;
        char a[24];
        format_addr(&g_conns[i].addr, a, sizeof(a));
        off += (size_t)snprintf(buf + off, len - off, "%s(h=%u %s)",
                                first ? "" : " ", (unsigned)g_conns[i].handle, a);
        first = false;
    }
    off += (size_t)snprintf(buf + off, len - off, "]");
    pthread_mutex_unlock(&g_lock);
}

const char *fake_rc_str(int rc)
{
    switch (rc) {
    case 0: return "0 (ok)";
    case BLE_HS_EALREADY: return "EALREADY";
    case BLE_HS_EBUSY: return "EBUSY";
    case BLE_HS_ENOMEM: return "ENOMEM";
    case BLE_HS_EDONE: return "EDONE";
    case BLE_HS_ENOENT: return "ENOENT";
    case BLE_HS_EINVAL: return "EINVAL";
    case BLE_HS_ETIMEOUT: return "ETIMEOUT";
    case BLE_HS_ENOTCONN: return "ENOTCONN";
    default: return "?";
    }
}

bool fake_wait_conn_count(int want, uint32_t timeout_ms)
{
    uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (fake_conn_count() == want) return true;
        if (mono_ms() >= deadline) return false;
        usleep(2000);
    }
}

bool fake_wait_conn_pending(uint32_t timeout_ms)
{
    uint64_t deadline = mono_ms() + timeout_ms;
    for (;;) {
        if (fake_conn_pending()) return true;
        if (mono_ms() >= deadline) return false;
        usleep(2000);
    }
}

/* ------------------------------------------- real project symbol */

/* main/boost_app_ble.c is not linked into this harness; the driver only needs
 * to know whether a phone currently holds the companion peripheral link. */
bool boost_app_ble_connected(void) { return false; }
