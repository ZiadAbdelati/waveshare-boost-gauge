#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "boost_display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_sim.h"
#include "boost_web.h"

static const char *TAG = "boost_main";
/* LVGL timer owns the physical gauge, preserving its 16 ms render cadence. */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    const boost_sample_t sample = boost_sim_tick();
    boost_gauge_update(&sample);
    lv_timer_set_period(timer, 16U);
}

/* Web/model publication stays independent when a GIF occupies the LVGL worker. */
static void sample_task(void *arg)
{
    (void)arg;
    TickType_t next_wake = xTaskGetTickCount();
    while (true) {
        const boost_sample_t sample = boost_sim_tick();
        boost_model_publish_sample(&sample);

        boost_display_metrics_t metrics;
        boost_display_get_metrics(&metrics);
        boost_model_set_display_metrics(&metrics);
        boost_model_refresh_status();
        boost_model_apply_schedule();

        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(16));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Boost gauge starting (demo MAP path)");

    ESP_ERROR_CHECK(boost_model_init());
    boost_config_t cfg;
    boost_model_get_config(&cfg);

    lv_display_t *disp = boost_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "boost_display_start failed");
        return;
    }

    /* Start from persisted config; long-press still toggles max/min. */
    boost_brightness_init(cfg.brightness_high);

    boost_sim_init();

    /* The physical gauge receives samples directly in its LVGL timer. */

    if (boost_display_lock(-1) == ESP_OK) {
        boost_gauge_create();

        const boost_sample_t initial = boost_sim_tick();
        boost_gauge_update(&initial);
        lv_timer_t *gauge_timer = lv_timer_create(gauge_timer_cb, 16, NULL);
        if (gauge_timer == NULL) {
            boost_display_unlock();
            ESP_LOGE(TAG, "failed to create gauge timer");
            return;
        }
        lv_timer_ready(gauge_timer);
        boost_display_unlock();
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }
    const BaseType_t sample_task_ok = xTaskCreatePinnedToCore(
        sample_task, "boost_sample", 4096, NULL, 4, NULL, 0);
    if (sample_task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start sample task");
    }

    esp_err_t web_err = boost_web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web control plane failed: %s", esp_err_to_name(web_err));
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 2s=brightness toggle · AP password boost1234");
}
