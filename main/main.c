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

    /* UI at ~20 Hz is enough; lower lock pressure vs httpd/SSE. */
    const TickType_t period = pdMS_TO_TICKS(50);
    uint32_t ticks = 0;
    uint32_t lock_fail = 0;

    while (true) {
        const boost_sample_t sample = boost_sim_tick();
        boost_http_set_sample(&sample);

        if (bsp_display_lock(200) == ESP_OK) {
            boost_gauge_service();
            boost_gauge_update(&sample);
            bsp_display_unlock();
            lock_fail = 0;
        } else if ((++lock_fail % 20) == 0) {
            ESP_LOGW(TAG, "display lock busy (%lu fails)", (unsigned long)lock_fail);
        }

        /* Auto-dim schedule check ~1 Hz */
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

    /* Config already applied brightness; ensure panel on. */
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

    if (boost_wifi_start_ap() != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP failed — web UI unavailable");
    } else if (boost_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed");
    } else {
        char ssid[32];
        boost_wifi_get_ssid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "Web UI: join Wi-Fi '%s' → http://192.168.4.1/", ssid);
    }

    /* Pin gauge UI to core 1; leave core 0 for Wi-Fi/httpd. */
    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensor_task,
        "boost_sim",
        6144,
        NULL,
        6,
        NULL,
        1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start sensor task");
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 2s=brightness · web=control/OTA");
}
