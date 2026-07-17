#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_sim.h"
#include "boost_web.h"

static const char *TAG = "boost_main";
static QueueHandle_t s_sample_queue;

/* LVGL timer: only simulation and LVGL mutation; never waits on model/NVS. */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    const boost_sample_t sample = boost_sim_tick();
    boost_gauge_update(&sample);
    (void)xQueueOverwrite(s_sample_queue, &sample);
}

/* Publish web/log state outside LVGL so HTTP/model contention cannot freeze it. */
static void control_task(void *arg)
{
    (void)arg;
    boost_sample_t sample;
    uint32_t ticks = 0;
    while (true) {
        if (xQueueReceive(s_sample_queue, &sample, pdMS_TO_TICKS(1000)) == pdTRUE) {
            boost_model_publish_sample(&sample);
        }
        boost_model_refresh_status();
        if ((++ticks % 20) == 0) {
            boost_model_apply_schedule();
        }
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
    s_sample_queue = xQueueCreate(1, sizeof(boost_sample_t));
    if (s_sample_queue == NULL) {
        ESP_LOGE(TAG, "failed to create sample queue");
        return;
    }

    if (bsp_display_lock(-1) == ESP_OK) {
        boost_gauge_create();

        /* Seed the first frame, then let the BSP LVGL worker flush it. */
        const boost_sample_t initial = boost_sim_tick();
        boost_gauge_update(&initial);
        (void)xQueueOverwrite(s_sample_queue, &initial);
        lv_timer_t *gauge_timer = lv_timer_create(gauge_timer_cb, 50, NULL);
        if (gauge_timer == NULL) {
            bsp_display_unlock();
            ESP_LOGE(TAG, "failed to create gauge timer");
            return;
        }
        lv_timer_ready(gauge_timer);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }
    const BaseType_t task_ok = xTaskCreatePinnedToCore(
        control_task, "boost_ctl", 4096, NULL, 4, NULL, 0);
    if (task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start control task");
    }

    esp_err_t web_err = boost_web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web control plane failed: %s", esp_err_to_name(web_err));
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 2s=brightness toggle · AP password boost1234");
}
