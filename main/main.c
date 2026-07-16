#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "boost_gauge.h"
#include "boost_sim.h"

static const char *TAG = "boost_main";

static void sensor_task(void *arg)
{
    (void)arg;

    const TickType_t period = pdMS_TO_TICKS(20); /* 50 Hz sample / UI push */

    while (true) {
        const boost_sample_t sample = boost_sim_tick();

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

    lv_display_t *disp = bsp_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return;
    }

    /* AMOLED brightness via panel — 70% is cabin-friendly for first flash. */
    bsp_display_brightness_set(70);
    bsp_display_backlight_on();

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

    ESP_LOGI(TAG, "running — tap screen to reset peak");
}
