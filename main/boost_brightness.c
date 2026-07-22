#include "boost_brightness.h"

#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "bsp/display.h"
#include "boost_display.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char *TAG = "boost_bright";
static int s_percent = BOOST_BRIGHTNESS_MAX;

static int clamp_percent(int percent)
{
    if (percent < 0) {
        return 0;
    }
    if (percent > 100) {
        return 100;
    }
    return percent;
}

void boost_brightness_init(int initial_percent)
{
    boost_brightness_set(initial_percent);
}

void boost_brightness_set(int percent)
{
    s_percent = clamp_percent(percent);
#ifdef ESP_PLATFORM
    /*
     * Brightness is a CO5300 SPI command on the same bus as LVGL flushes.
     * Never touch the panel without the LVGL adapter lock — concurrent access
     * panics (LoadProhibited) under theme/schedule changes.
     */
    if (boost_display_lock(1000) != ESP_OK) {
        ESP_LOGW(TAG, "brightness %d%% deferred (display busy)", s_percent);
        return;
    }
    esp_err_t err = boost_display_set_brightness(s_percent);
    boost_display_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "boost_display_set_brightness(%d) failed: %s",
                 s_percent, esp_err_to_name(err));
    }
#endif
    ESP_LOGI(TAG, "brightness %d%%", s_percent);
}
int boost_brightness_get(void)
{
    return s_percent;
}

bool boost_brightness_is_max(void)
{
    return s_percent >= (BOOST_BRIGHTNESS_MAX + BOOST_BRIGHTNESS_MIN) / 2;
}

void boost_brightness_toggle_max_min(void)
{
    if (boost_brightness_is_max()) {
        boost_brightness_set(BOOST_BRIGHTNESS_MIN);
    } else {
        boost_brightness_set(BOOST_BRIGHTNESS_MAX);
    }
}
