#include "freertos/FreeRTOS.h"

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
static uint32_t s_ui_ticks;

/**
 * Runs inside LVGL's own worker while its display lock is already held.
 * One sample drives both the physical face and web status, so they cannot desync.
 */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    const boost_sample_t sample = boost_sim_tick();
    boost_gauge_update(&sample);
    boost_http_set_sample(&sample);

    if ((++s_ui_ticks % 20) == 0) {
        boost_config_apply_schedule();
    }
    if ((s_ui_ticks % 40) == 0) {
        ESP_LOGI(TAG, "physical+web psi=%+.1f peak=%+.1f",
                 (double)sample.psi, (double)sample.peak_psi);
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

    boost_config_t cfg;
    boost_config_get_copy(&cfg);
    boost_brightness_set(cfg.brightness);
    boost_sim_init();

    if (bsp_display_lock(-1) == ESP_OK) {
        boost_gauge_create();
        boost_gauge_set_theme(cfg.theme_id);

        /* Seed immediately, then run at 20 Hz on the LVGL worker. */
        const boost_sample_t initial = boost_sim_tick();
        boost_gauge_update(&initial);
        boost_http_set_sample(&initial);
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

    if (boost_wifi_start_ap() != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP failed");
    } else if (boost_http_start() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP failed");
    } else {
        char ssid[32];
        boost_wifi_get_ssid(ssid, sizeof(ssid));
        ESP_LOGI(TAG, "Web UI: join '%s' -> http://192.168.4.1/", ssid);
    }

    ESP_LOGI(TAG, "LVGL timer owns physical + web demo samples");
}
