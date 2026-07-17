#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "boost_brightness.h"
#include "boost_config.h"
#include "boost_gauge.h"
#include "boost_http.h"
#include "boost_sim.h"
#include "boost_wifi.h"

static const char *TAG = "boost_main";

static void sensor_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(50); /* 20 Hz */
    uint32_t ticks = 0;
    uint32_t lock_fail = 0;
    uint32_t ok_count = 0;

    while (true) {
        const boost_sample_t sample = boost_sim_tick();
        boost_http_set_sample(&sample);

        if (bsp_display_lock(100) == ESP_OK) {
            boost_gauge_update(&sample);
            bsp_display_unlock();
            lock_fail = 0;
            if ((++ok_count % 40) == 0) {
                ESP_LOGI(TAG, "ui psi=%+.1f peak=%+.1f", sample.psi, sample.peak_psi);
            }
        } else if ((++lock_fail % 20) == 0) {
            ESP_LOGW(TAG, "display lock busy x%lu (psi=%+.1f)", (unsigned long)lock_fail, sample.psi);
        }

        if ((++ticks % 20) == 0) {
            boost_config_apply_schedule();
        }
        vTaskDelay(period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Boost gauge %s starting", BOOST_FW_VERSION_STR);

    boost_config_init();

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }

    boost_brightness_set(boost_config_get()->brightness);
    boost_sim_init();

    if (bsp_display_lock(-1) == ESP_OK) {
        boost_gauge_create();
        boost_gauge_set_theme(boost_config_get()->theme_id);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }

    /* Start network after UI is alive. */
    if (boost_wifi_start_ap() != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP failed");
    } else if (boost_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP failed");
    } else {
        char ssid[32];
        boost_wifi_get_ssid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "Web UI: join '%s' -> http://192.168.4.1/", ssid);
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensor_task, "boost_sim", 6144, NULL, 5, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "sensor task failed");
    }
}
