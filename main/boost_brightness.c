#include "boost_brightness.h"

#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "bsp/display.h"
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
    bsp_display_brightness_set(s_percent);
    if (s_percent > 0) {
        bsp_display_backlight_on();
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
