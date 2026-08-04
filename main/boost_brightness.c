#include "boost_brightness.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/display.h"
#include "boost_display.h"
#else
#define ESP_LOGI(tag, fmt, ...) printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#endif

static const char *TAG = "boost_bright";
static int s_percent = BOOST_BRIGHTNESS_MAX;

/* Toggle targets. Seeded with the compile-time fallbacks so a long-press before
 * the config is loaded still does something sane; boost_brightness_set_levels()
 * replaces them with the user's configured pair. */
static int s_level_high = BOOST_BRIGHTNESS_MAX;
static int s_level_low = BOOST_BRIGHTNESS_MIN;

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

static bool apply_percent(int target, bool lock_held)
{
    target = clamp_percent(target);
#ifdef ESP_PLATFORM
    static uint32_t request_id;
    const uint32_t id = ++request_id;
    const int64_t request_us = esp_timer_get_time();
    ESP_LOGI(TAG, "brightness request id=%lu target=%d%% at=%lldus lock_held=%d",
             (unsigned long)id, target, (long long)request_us, lock_held);

    if (!lock_held) {
        const int64_t lock_start_us = esp_timer_get_time();
        if (boost_display_lock(1000) != ESP_OK) {
            ESP_LOGW(TAG, "brightness lock timeout id=%lu waited=%lldus target=%d%%",
                     (unsigned long)id,
                     (long long)(esp_timer_get_time() - lock_start_us), target);
            return false;
        }
        ESP_LOGI(TAG, "brightness lock acquired id=%lu waited=%lldus",
                 (unsigned long)id,
                 (long long)(esp_timer_get_time() - lock_start_us));
    }

    const int64_t tx_start_us = esp_timer_get_time();
    const esp_err_t err = boost_display_set_brightness(target);
    const int64_t tx_done_us = esp_timer_get_time();
    if (!lock_held) boost_display_unlock();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "brightness tx failed id=%lu target=%d%% duration=%lldus: %s",
                 (unsigned long)id, target,
                 (long long)(tx_done_us - tx_start_us), esp_err_to_name(err));
        return false;
    }

    s_percent = target;
    ESP_LOGI(TAG, "brightness applied id=%lu target=%d%% request_to_applied=%lldus tx=%lldus",
             (unsigned long)id, target,
             (long long)(tx_done_us - request_us),
             (long long)(tx_done_us - tx_start_us));
#else
    (void)lock_held;
    s_percent = target;
    ESP_LOGI(TAG, "brightness %d%%", s_percent);
#endif
    return true;
}

void boost_brightness_set(int percent)
{
    (void)apply_percent(percent, false);
}

int boost_brightness_get(void)
{
    return s_percent;
}

void boost_brightness_set_levels(int high, int low)
{
    high = clamp_percent(high);
    low = clamp_percent(low);
    /* A collapsed pair would make the toggle a no-op and strand the panel at
     * whichever level it landed on, so keep at least a one-point separation. */
    if (low >= high) {
        if (high > 0) low = high - 1;
        else high = 1;
    }
    s_level_high = high;
    s_level_low = low;
    ESP_LOGI(TAG, "toggle levels %d%% / %d%%", s_level_high, s_level_low);
}

bool boost_brightness_is_max(void)
{
    /* Classify by the nearest configured endpoint. Comparing doubled distances
     * avoids midpoint truncation trapping high=1/low=0 at the low endpoint. */
    return abs(s_percent - s_level_high) <= abs(s_percent - s_level_low);
}

void boost_brightness_toggle_max_min(void)
{
    boost_brightness_set(boost_brightness_is_max() ? s_level_low : s_level_high);
}

void boost_brightness_toggle_max_min_locked(void)
{
    (void)apply_percent(boost_brightness_is_max() ? s_level_low : s_level_high, true);
}
