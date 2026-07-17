#include "freertos/FreeRTOS.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_model.h"
#include "boost_sim.h"
#include "boost_web.h"

static const char *TAG = "boost_main";
static uint32_t s_ui_ticks;

/*
 * Run gauge mutation on LVGL's worker. Cross-core LVGL calls can starve or
 * corrupt the partial RGB565 flush path on the physical CO5300 panel.
 */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    const boost_sample_t sample = boost_sim_tick();
    boost_gauge_update(&sample);
    boost_model_publish_sample(&sample);

    if ((++s_ui_ticks % 20) == 0) {
        boost_model_apply_schedule();
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

        /* Seed a complete frame before networking starts. */
        const boost_sample_t initial = boost_sim_tick();
        boost_gauge_update(&initial);
        boost_model_publish_sample(&initial);
        lv_timer_t *gauge_timer = lv_timer_create(gauge_timer_cb, 50, NULL);
        if (gauge_timer == NULL) {
            bsp_display_unlock();
            ESP_LOGE(TAG, "failed to create gauge timer");
            return;
        }
        lv_timer_ready(gauge_timer);
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(disp);
        bsp_display_unlock();
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }
    esp_err_t web_err = boost_web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web control plane failed: %s", esp_err_to_name(web_err));
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 2s=brightness toggle · AP password boost1234");
}
