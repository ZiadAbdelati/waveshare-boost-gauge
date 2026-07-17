#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_sim.h"
#include "boost_web.h"

static const char *TAG = "boost_main";

static void sensor_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(20); /* 50 Hz sample / UI push */

    while (true) {
        const boost_sample_t sample = boost_sim_tick();
        boost_model_publish_sample(&sample);
        boost_model_apply_schedule();

        if (bsp_display_lock(50) == ESP_OK) {
            boost_gauge_update(&sample);
            bsp_display_unlock();
        }

        vTaskDelay(period);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Boost gauge starting (demo MAP path)");

    ESP_ERROR_CHECK(boost_model_init());
    boost_config_t cfg;
    boost_model_get_config(&cfg);

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }

    /* Start from persisted config; long-press still toggles max/min. */
    boost_brightness_init(cfg.brightness_high);

    boost_sim_init();

    if (bsp_display_lock(-1) == ESP_OK) {
        boost_gauge_create();
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensor_task,
        "boost_sim",
        4096,
        NULL,
        5,
        NULL,
        0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start sensor task");
    }

    esp_err_t web_err = boost_web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web control plane failed: %s", esp_err_to_name(web_err));
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 2s=brightness toggle · AP password boost1234");
}
