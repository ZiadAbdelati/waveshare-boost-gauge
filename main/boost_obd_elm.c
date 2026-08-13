#include "boost_obd_elm.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "boost_obd_ble.h"

static const char *TAG = "boost_obd_elm";

static void on_rx(const uint8_t *data, size_t len, void *ctx);

void boost_obd_elm_init(void)
{
    boost_obd_ble_set_rx_cb(on_rx, NULL);
}

static SemaphoreHandle_t s_mutex;    /* guards buffer + flags */
static SemaphoreHandle_t s_reply;    /* given when '>' completes a reply */

static uint8_t s_rx_buf[BOOST_OBD_ELM_RX_MAX];
static size_t s_rx_len;
static bool s_active;                /* a request is waiting for '>' */
static bool s_had_traffic;           /* any byte seen since request start */

static void on_rx(const uint8_t *data, size_t len, void *ctx)
{
    (void)ctx;
    if (data == NULL || len == 0 || s_reply == NULL || s_mutex == NULL) return;
    if (xSemaphoreTake(s_mutex, 0) != pdTRUE) return; /* never block the host task */
    if (!s_active) {
        xSemaphoreGive(s_mutex);
        return;
    }
    s_had_traffic = true;
    size_t i = 0;
    for (; i < len; ++i) {
        if (s_rx_len < sizeof(s_rx_buf) - 1) {
            s_rx_buf[s_rx_len++] = data[i];
        }
        if (data[i] == '>') {
            xSemaphoreGive(s_mutex);
            xSemaphoreGive(s_reply);
            return;
        }
    }
    xSemaphoreGive(s_mutex);
}

/* Trim leading/trailing CR, LF, spaces and NULs. */
static void trim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
    char *p = s;
    while (*p == '\r' || *p == '\n' || *p == ' ') ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

bool boost_obd_elm_request(const char *cmd, char *reply, size_t reply_size,
                           uint32_t timeout_ms)
{
    if (cmd == NULL || reply == NULL || reply_size == 0) return false;
    if (s_reply == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        s_reply = xSemaphoreCreateBinary();
        if (s_reply == NULL) return false;
    }

    /* Discard any leftover completion token and arm a fresh capture. */
    xSemaphoreTake(s_reply, 0);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rx_len = 0;
    s_active = true;
    s_had_traffic = false;
    xSemaphoreGive(s_mutex);

    char wire[BOOST_OBD_ELM_RX_MAX];
    if (strlen(cmd) + 1 >= sizeof(wire)) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_active = false;
        xSemaphoreGive(s_mutex);
        return false;
    }
    strcpy(wire, cmd);
    strcat(wire, "\r");

    if (!boost_obd_ble_send((const uint8_t *)wire, strlen(wire))) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_active = false;
        xSemaphoreGive(s_mutex);
        return false;
    }

    const bool ok = xSemaphoreTake(s_reply, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_active = false;
    size_t n = s_rx_len;
    if (n > 0 && s_rx_buf[n - 1] == '>') --n; /* terminator, if captured */
    const size_t copy = n < reply_size - 1 ? n : reply_size - 1;
    if (copy > 0) memcpy(reply, s_rx_buf, copy);
    reply[copy] = '\0';
    s_rx_len = 0;
    xSemaphoreGive(s_mutex);

    trim(reply);
    if (!ok && !s_had_traffic) {
        ESP_LOGW(TAG, "timeout on '%s' (no reply)", cmd);
    }
    return ok;
}

void boost_obd_elm_reset(void)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_rx_len = 0;
    s_active = false;
    s_had_traffic = false;
    xSemaphoreGive(s_mutex);
    xSemaphoreTake(s_reply, 0);
}
