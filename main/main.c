#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "boost_display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_theme.h"
#include "boost_sim.h"
#include "boost_sensors.h"
#include "boost_web.h"

static const char *TAG = "boost_main";

/*
 * The one place the demo/real source is chosen. Demo mode (persisted, default
 * off) runs the synthetic sweep; otherwise we hand back the latest real-sensor
 * snapshot, which the sensor task computes on its own cadence so neither the
 * LVGL timer nor the web task ever blocks on an I2C read.
 */
static boost_sample_t next_sample(void)
{
    if (boost_theme_demo_mode()) {
        return boost_sim_tick();
    }
    return boost_sensors_get_sample();
}

/* LVGL timer owns the physical gauge, preserving its 16 ms render cadence. */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    const boost_sample_t sample = next_sample();
    boost_gauge_update(&sample);
    lv_timer_set_period(timer, 16U);
}

/* Web/model publication stays independent when a GIF occupies the LVGL worker. */
static void sample_task(void *arg)
{
    (void)arg;
    TickType_t next_wake = xTaskGetTickCount();
    while (true) {
        const boost_sample_t sample = next_sample();
        boost_model_publish_sample(&sample);
        /* Kick the WebSocket push task straight away: the sample is the event,
         * so remote clients no longer wait out a free-running 50 ms timer. */
        boost_web_notify_sample();

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
    ESP_LOGI(TAG, "Boost gauge starting");

    /* Before boost_model_init(): the model resolves the active theme by id and
     * must see any persisted colour overrides. Mounts NVS itself, so this does
     * not depend on running after boost_model_init(). */
    boost_theme_init();
    ESP_ERROR_CHECK(boost_model_init());
    boost_config_t cfg;
    boost_model_get_config(&cfg);

    lv_display_t *disp = boost_display_start();
    if (disp == NULL) {
        ESP_LOGE(TAG, "boost_display_start failed");
        return;
    }

    /* Apply the persisted tearing-effect preference now that both the theme
     * store and the display exist. Default is off. */
    boost_display_set_te(boost_theme_te_sync());

    /* Start from persisted config; long-press still toggles max/min. */
    boost_brightness_init(cfg.brightness_high);

    boost_sim_init();

    /* Real-sensor path: I2C on GPIO18/17 (separate from the BSP touch bus on
     * GPIO14/15). Brings up the bus, probes ADS1115 (0x48) and BMP280 (0x76),
     * and starts the reader task. Runs regardless of the demo flag so a runtime
     * flip to real mode has data waiting; init logs which sensors were seen. */
    if (!boost_sensors_init()) {
        ESP_LOGW(TAG, "no MAP/ambient sensors detected; real mode will fault until wired");
    }
    ESP_LOGI(TAG, "sample source at boot: %s", boost_theme_demo_mode() ? "DEMO (sim)" : "real sensors");

    /* The physical gauge receives samples directly in its LVGL timer. */

    if (boost_display_lock(-1) == ESP_OK) {
        boost_gauge_create();

        const boost_sample_t initial = next_sample();
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
