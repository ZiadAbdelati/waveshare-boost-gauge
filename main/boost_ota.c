#include "boost_ota.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_format.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

static const char *TAG = "boost_ota";

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no ota partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota = 0;
    bool ota_started = false;
    char buf[1024];
    int remaining = req->content_len;
    int received_total = 0;

    ESP_LOGI(TAG, "OTA begin -> %s size=%d", update_part->label, remaining);

    while (remaining > 0) {
        int to_read = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int r = httpd_req_recv(req, buf, to_read);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            if (ota_started) {
                esp_ota_abort(ota);
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_FAIL;
        }

        /* First chunk may include multipart headers — find binary magic. */
        const char *payload = buf;
        int payload_len = r;
        if (!ota_started) {
            const uint8_t *p = (const uint8_t *)buf;
            int magic_off = -1;
            for (int i = 0; i + 1 < r; i++) {
                if (p[i] == 0xE9) { /* ESP image magic */
                    magic_off = i;
                    break;
                }
            }
            if (magic_off < 0) {
                /* keep reading until magic appears */
                remaining -= r;
                continue;
            }
            payload = buf + magic_off;
            payload_len = r - magic_off;
            esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
                return err;
            }
            ota_started = true;
        }

        esp_err_t err = esp_ota_write(ota, payload, payload_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota write failed");
            return err;
        }
        received_total += payload_len;
        remaining -= r;
    }

    if (!ota_started) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no firmware image found");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota end failed");
        return err;
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_boot: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set boot failed");
        return err;
    }

    ESP_LOGI(TAG, "OTA ok (%d bytes). Rebooting…", received_total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t boost_ota_register(httpd_handle_t server)
{
    httpd_uri_t uri = {
        .uri = "/api/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &uri);
}
