#include "boost_config.h"

#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "boost_brightness.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "boost_gauge.h"

static const char *TAG = "boost_cfg";
static const char *NVS_NS = "boost";

static boost_config_t s_cfg;
static bool s_ready;
static SemaphoreHandle_t s_mutex;
static StaticSemaphore_t s_mutex_storage;

static void config_lock(void)
{
    if (s_mutex) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void config_unlock(void)
{
    if (s_mutex) {
        xSemaphoreGive(s_mutex);
    }
}

static void set_defaults(boost_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->brightness = BOOST_BRIGHTNESS_MAX;
    c->theme_id = BOOST_THEME_NIGHT_BLACK;
    c->dim_enable = 0;
    c->dim_start_hour = 21;
    c->dim_start_min = 0;
    c->dim_end_hour = 7;
    c->dim_end_min = 0;
    c->tz_offset_min = 0;
    c->units = BOOST_UNITS_PSI;
    c->ap_pass[0] = '\0';
}

static void apply_side_effects(const boost_config_t *c)
{
    boost_brightness_set(c->brightness);
    /* Theme LVGL objects may not exist yet during early boot. */
}

static bool load_from_nvs(boost_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t len = sizeof(*c);
    esp_err_t err = nvs_get_blob(h, "cfg", c, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(*c)) {
        return false;
    }
    if (c->theme_id >= BOOST_THEME_COUNT) {
        c->theme_id = BOOST_THEME_NIGHT_BLACK;
    }
    if (c->brightness > 100) {
        c->brightness = 100;
    }
    c->ap_pass[sizeof(c->ap_pass) - 1] = '\0';
    return true;
}

static bool save_to_nvs(const boost_config_t *c)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_set_blob(h, "cfg", c, sizeof(*c));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
}

void boost_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    s_mutex = xSemaphoreCreateMutexStatic(&s_mutex_storage);

    set_defaults(&s_cfg);
    if (!load_from_nvs(&s_cfg)) {
        ESP_LOGI(TAG, "using defaults");
        save_to_nvs(&s_cfg);
    } else {
        ESP_LOGI(TAG, "loaded from NVS (theme=%u brightness=%u)",
                 s_cfg.theme_id, s_cfg.brightness);
    }
    apply_side_effects(&s_cfg);
    s_ready = true;
}


void boost_config_get_copy(boost_config_t *out)
{
    if (!out) {
        return;
    }
    config_lock();
    *out = s_cfg;
    config_unlock();
}

bool boost_config_set(const boost_config_t *cfg)
{
    if (!cfg) {
        return false;
    }
    boost_config_t tmp = *cfg;
    if (tmp.theme_id >= BOOST_THEME_COUNT) {
        tmp.theme_id = BOOST_THEME_NIGHT_BLACK;
    }
    if (tmp.brightness > 100) {
        tmp.brightness = 100;
    }
    tmp.dim_start_hour %= 24;
    tmp.dim_end_hour %= 24;
    if (tmp.dim_start_min > 59) {
        tmp.dim_start_min = 59;
    }
    if (tmp.dim_end_min > 59) {
        tmp.dim_end_min = 59;
    }
    tmp.ap_pass[sizeof(tmp.ap_pass) - 1] = '\0';

    config_lock();
    s_cfg = tmp;
    config_unlock();
    apply_side_effects(&tmp);
    return save_to_nvs(&tmp);
}

bool boost_config_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    config_lock();
    s_cfg.brightness = percent;
    config_unlock();
    boost_brightness_set(percent);
    boost_config_t snapshot;
    boost_config_get_copy(&snapshot);
    return save_to_nvs(&snapshot);
}

bool boost_config_set_theme(uint8_t theme_id)
{
    if (theme_id >= BOOST_THEME_COUNT) {
        return false;
    }
    /* Persist only. Caller applies LVGL theme while holding display lock. */
    config_lock();
    s_cfg.theme_id = theme_id;
    config_unlock();
    boost_config_t snapshot;
    boost_config_get_copy(&snapshot);
    return save_to_nvs(&snapshot);
}

bool boost_config_dim_window_active(void)
{
    boost_config_t cfg;
    boost_config_get_copy(&cfg);
    if (!cfg.dim_enable) {
        return false;
    }

    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);

    const int now_m = tm_local.tm_hour * 60 + tm_local.tm_min;
    const int start_m = cfg.dim_start_hour * 60 + cfg.dim_start_min;
    const int end_m = cfg.dim_end_hour * 60 + cfg.dim_end_min;

    if (start_m == end_m) {
        return false;
    }
    if (start_m < end_m) {
        return now_m >= start_m && now_m < end_m;
    }
    return now_m >= start_m || now_m < end_m;
}

void boost_config_apply_schedule(void)
{
    boost_config_t cfg;
    boost_config_get_copy(&cfg);
    if (!s_ready || !cfg.dim_enable) {
        return;
    }
    if (boost_config_dim_window_active()) {
        if (boost_brightness_get() != BOOST_BRIGHTNESS_MIN) {
            boost_brightness_set(BOOST_BRIGHTNESS_MIN);
        }
    } else if (boost_brightness_get() == BOOST_BRIGHTNESS_MIN &&
               cfg.brightness != BOOST_BRIGHTNESS_MIN) {
        boost_brightness_set(cfg.brightness);
    }
}

const char *boost_theme_name(uint8_t theme_id)
{
    switch (theme_id) {
    case BOOST_THEME_NIGHT_BLACK:
        return "Night Black";
    case BOOST_THEME_GHOST_GRAY:
        return "Ghost Gray";
    default:
        return "Unknown";
    }
}
