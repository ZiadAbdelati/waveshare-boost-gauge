#include "boost_model.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "boost_brightness.h"

#define NVS_NS "boost"
#define NVS_KEY_CONFIG "config"
#define NVS_KEY_EPOCH_MS "epoch_ms"
#define NVS_KEY_MONO_MS "mono_ms"

static const char *TAG = "boost_model";
static SemaphoreHandle_t s_lock;
static boost_config_t s_config;
static boost_state_t s_state;
static boost_log_sample_t s_logs[BOOST_LOG_CAPACITY];
static size_t s_log_head;
static size_t s_log_count;
static uint32_t s_log_divider;
static boost_media_status_t s_media;
static int s_last_schedule_minute = -1;
static int s_pending_brightness = -1;
static int clamp_percent(int v)
{
    if (v < 0) {
        return 0;
    }
    if (v > 100) {
        return 100;
    }
    return v;
}

static int normalize_minutes(int minutes)
{
    const int day = 24 * 60;
    minutes %= day;
    if (minutes < 0) {
        minutes += day;
    }
    return minutes;
}

static int clamp_tz_offset(int minutes)
{
    if (minutes < -14 * 60) {
        return -14 * 60;
    }
    if (minutes > 14 * 60) {
        return 14 * 60;
    }
    return minutes;
}

static const char *zone_for_psi(float psi)
{
    if (psi >= 18.0f) {
        return "OVER";
    }
    if (psi >= 0.35f) {
        return "BOOST";
    }
    if (psi > -0.35f) {
        return "ATMO";
    }
    return "VAC";
}

static int64_t epoch_ms_now(void)
{
    struct timeval tv = {0};
    gettimeofday(&tv, NULL);
    if (tv.tv_sec < 1700000000) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)tv.tv_usec / 1000LL;
}

static void defaults(boost_config_t *cfg)
{
    const boost_theme_t *theme = boost_theme_default();
    memset(cfg, 0, sizeof(*cfg));
    cfg->brightness_high = theme->brightness_high;
    cfg->brightness_low = theme->brightness_low;
    cfg->dim_schedule.enabled = false;
    cfg->dim_schedule.start_minutes = 21 * 60;
    cfg->dim_schedule.end_minutes = 7 * 60;
    cfg->timezone_offset_minutes = 0;
    strlcpy(cfg->active_theme_id, theme->id, sizeof(cfg->active_theme_id));
}

static esp_err_t save_config_locked(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_CONFIG, &s_config, sizeof(s_config));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void load_config(void)
{
    defaults(&s_config);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_config);
    boost_config_t loaded;
    if (nvs_get_blob(h, NVS_KEY_CONFIG, &loaded, &len) == ESP_OK && len == sizeof(loaded) &&
        boost_theme_find(loaded.active_theme_id) != NULL) {
        s_config = loaded;
        s_config.brightness_high = clamp_percent(s_config.brightness_high);
        s_config.brightness_low = clamp_percent(s_config.brightness_low);
        s_config.dim_schedule.start_minutes = normalize_minutes(s_config.dim_schedule.start_minutes);
        s_config.dim_schedule.end_minutes = normalize_minutes(s_config.dim_schedule.end_minutes);
        s_config.timezone_offset_minutes = clamp_tz_offset(s_config.timezone_offset_minutes);
    }
    int64_t epoch_ms = 0;
    int64_t saved_mono_ms = 0;
    if (nvs_get_i64(h, NVS_KEY_EPOCH_MS, &epoch_ms) == ESP_OK && epoch_ms > 0) {
        if (nvs_get_i64(h, NVS_KEY_MONO_MS, &saved_mono_ms) == ESP_OK) {
            const int64_t now_mono_ms = esp_timer_get_time() / 1000LL;
            if (now_mono_ms >= saved_mono_ms) {
                epoch_ms += now_mono_ms - saved_mono_ms;
            }
        }
        struct timeval tv = {
            .tv_sec = (time_t)(epoch_ms / 1000),
            .tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000),
        };
        settimeofday(&tv, NULL);
    }
    nvs_close(h);
}

esp_err_t boost_model_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }

    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    load_config();
    memset(&s_state, 0, sizeof(s_state));
    s_state.firmware_version = "0.3.0-web";
    s_state.brightness = s_config.brightness_high;
    s_state.timezone_offset_minutes = s_config.timezone_offset_minutes;
    strlcpy(s_state.active_theme_id, s_config.active_theme_id, sizeof(s_state.active_theme_id));
    memset(&s_media, 0, sizeof(s_media));
    s_media.playback_supported = false;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "config ready: theme=%s high=%d low=%d",
             s_config.active_theme_id, s_config.brightness_high, s_config.brightness_low);
    return ESP_OK;
}

void boost_model_publish_sample(const boost_sample_t *sample)
{
    if (sample == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.psi = sample->psi;
    s_state.peak_psi = sample->peak_psi > 0.0f ? sample->peak_psi : 0.0f;
    strlcpy(s_state.zone, zone_for_psi(sample->psi), sizeof(s_state.zone));
    s_state.demo = sample->demo;
    s_state.brightness = boost_brightness_get();
    s_state.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_state.epoch_ms = epoch_ms_now();
    s_state.timezone_offset_minutes = s_config.timezone_offset_minutes;
    strlcpy(s_state.active_theme_id, s_config.active_theme_id, sizeof(s_state.active_theme_id));

    if (++s_log_divider >= 5) {
        s_log_divider = 0;
        boost_log_sample_t *dst = &s_logs[s_log_head];
        dst->t_ms = (uint32_t)s_state.uptime_ms;
        dst->psi = s_state.psi;
        dst->peak_psi = s_state.peak_psi;
        dst->demo = s_state.demo;
        strlcpy(dst->zone, s_state.zone, sizeof(dst->zone));
        s_log_head = (s_log_head + 1) % BOOST_LOG_CAPACITY;
        if (s_log_count < BOOST_LOG_CAPACITY) {
            ++s_log_count;
        }
    }
    xSemaphoreGive(s_lock);
}

void boost_model_set_display_metrics(const boost_display_metrics_t *metrics)
{
    if (metrics == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.display = *metrics;
    xSemaphoreGive(s_lock);
}

void boost_model_refresh_status(void)
{
    if (s_lock == NULL) {
        return;
    }
    int pending = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pending = s_pending_brightness;
    /* Keep -2 through apply_schedule so it can force re-eval, then clear. */
    if (pending != -2) {
        s_pending_brightness = -1;
    }
    s_state.brightness = boost_brightness_get();
    s_state.uptime_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
    s_state.epoch_ms = epoch_ms_now();
    s_state.timezone_offset_minutes = s_config.timezone_offset_minutes;
    strlcpy(s_state.active_theme_id, s_config.active_theme_id, sizeof(s_state.active_theme_id));
    xSemaphoreGive(s_lock);

    if (pending == -2) {
        boost_model_apply_schedule();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pending_brightness == -2) {
            s_pending_brightness = -1;
        }
        xSemaphoreGive(s_lock);
    } else if (pending >= 0) {
        const int before = boost_brightness_get();
        boost_brightness_set(pending);
        if (boost_brightness_get() != pending && before == boost_brightness_get()) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (s_pending_brightness < 0) {
                s_pending_brightness = pending;
            }
            xSemaphoreGive(s_lock);
        }
    }
}

void boost_model_get_state(boost_state_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_lock);
}

void boost_model_get_config(boost_config_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_config;
    xSemaphoreGive(s_lock);
}

esp_err_t boost_model_update_config(const boost_config_t *patch, uint32_t fields)
{
    if (patch == NULL || s_lock == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (fields & BOOST_CONFIG_BRIGHTNESS_HIGH) {
        s_config.brightness_high = clamp_percent(patch->brightness_high);
    }
    if (fields & BOOST_CONFIG_BRIGHTNESS_LOW) {
        s_config.brightness_low = clamp_percent(patch->brightness_low);
    }
    if (fields & BOOST_CONFIG_DIM_ENABLED) {
        s_config.dim_schedule.enabled = patch->dim_schedule.enabled;
    }
    if (fields & BOOST_CONFIG_DIM_START) {
        s_config.dim_schedule.start_minutes = normalize_minutes(patch->dim_schedule.start_minutes);
    }
    if (fields & BOOST_CONFIG_DIM_END) {
        s_config.dim_schedule.end_minutes = normalize_minutes(patch->dim_schedule.end_minutes);
    }
    if (fields & BOOST_CONFIG_TZ_OFFSET) {
        s_config.timezone_offset_minutes = clamp_tz_offset(patch->timezone_offset_minutes);
    }
    if (fields & BOOST_CONFIG_THEME) {
        const boost_theme_t *theme = boost_theme_find(patch->active_theme_id);
        if (theme == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;
        }
        strlcpy(s_config.active_theme_id, theme->id, sizeof(s_config.active_theme_id));
    }
    esp_err_t err = save_config_locked();
    const bool schedule_enabled = s_config.dim_schedule.enabled;
    const int high = s_config.brightness_high;
    if (err == ESP_OK) {
        if (schedule_enabled) {
            s_last_schedule_minute = -1;
            /* Defer SPI brightness to control task — never block httpd. */
            s_pending_brightness = -2; /* re-evaluate schedule */
        } else if (fields & (BOOST_CONFIG_DIM_ENABLED | BOOST_CONFIG_BRIGHTNESS_HIGH |
                             BOOST_CONFIG_THEME)) {
            /* Leaving schedule (or explicit high/theme) restores daytime level. */
            s_pending_brightness = high;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t boost_model_set_active_theme(const char *id)
{
    boost_config_t patch;
    boost_model_get_config(&patch);
    strlcpy(patch.active_theme_id, id ? id : "", sizeof(patch.active_theme_id));
    const boost_theme_t *theme = boost_theme_find(patch.active_theme_id);
    if (theme == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    patch.brightness_high = theme->brightness_high;
    patch.brightness_low = theme->brightness_low;
    return boost_model_update_config(
        &patch,
        BOOST_CONFIG_THEME | BOOST_CONFIG_BRIGHTNESS_HIGH | BOOST_CONFIG_BRIGHTNESS_LOW);
}

const boost_theme_t *boost_model_active_theme(void)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    const boost_theme_t *theme = boost_theme_find(cfg.active_theme_id);
    if (theme == NULL) {
        theme = boost_theme_default();
    }
    return theme;
}

esp_err_t boost_model_set_time(int64_t epoch_ms, int timezone_offset_minutes)
{
    if (epoch_ms <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    struct timeval tv = {
        .tv_sec = (time_t)(epoch_ms / 1000),
        .tv_usec = (suseconds_t)((epoch_ms % 1000) * 1000),
    };
    if (settimeofday(&tv, NULL) != 0) {
        return ESP_FAIL;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_config.timezone_offset_minutes = clamp_tz_offset(timezone_offset_minutes);
    s_state.timezone_offset_minutes = s_config.timezone_offset_minutes;
    s_state.epoch_ms = epoch_ms_now();
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_i64(h, NVS_KEY_EPOCH_MS, epoch_ms);
        if (err == ESP_OK) {
            err = nvs_set_i64(h, NVS_KEY_MONO_MS, esp_timer_get_time() / 1000LL);
        }
        if (err == ESP_OK) {
            err = save_config_locked();
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);
    return err;
}

bool boost_model_schedule_wants_low(void)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    if (!cfg.dim_schedule.enabled) {
        return false;
    }
    const int64_t now_ms = epoch_ms_now();
    if (now_ms <= 0) {
        return false;
    }
    const int minutes_day = 24 * 60;
    int local_min = (int)((now_ms / 60000LL + cfg.timezone_offset_minutes) % minutes_day);
    if (local_min < 0) {
        local_min += minutes_day;
    }
    const int start = cfg.dim_schedule.start_minutes;
    const int end = cfg.dim_schedule.end_minutes;
    if (start == end) {
        return true;
    }
    if (start < end) {
        return local_min >= start && local_min < end;
    }
    return local_min >= start || local_min < end;
}

void boost_model_apply_schedule(void)
{
    boost_config_t cfg;
    boost_model_get_config(&cfg);
    if (!cfg.dim_schedule.enabled) {
        return;
    }

    const int64_t now_ms = epoch_ms_now();
    if (now_ms <= 0) {
        /* No wall clock yet — cannot evaluate local schedule. */
        return;
    }

    const int minutes_day = 24 * 60;
    int local_min = (int)((now_ms / 60000LL + cfg.timezone_offset_minutes) % minutes_day);
    if (local_min < 0) {
        local_min += minutes_day;
    }

    /* Re-apply at most once per local minute, or when forced via pending=-2. */
    if (local_min == s_last_schedule_minute && s_pending_brightness != -2) {
        return;
    }
    s_last_schedule_minute = local_min;

    const int target = boost_model_schedule_wants_low() ? cfg.brightness_low : cfg.brightness_high;
    if (boost_brightness_get() != target) {
        boost_brightness_set(target);
    }
}

size_t boost_model_copy_logs(boost_log_sample_t *out, size_t max_count)
{
    if (out == NULL || max_count == 0 || s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_log_count < max_count ? s_log_count : max_count;
    size_t start = (s_log_head + BOOST_LOG_CAPACITY - n) % BOOST_LOG_CAPACITY;
    for (size_t i = 0; i < n; ++i) {
        out[i] = s_logs[(start + i) % BOOST_LOG_CAPACITY];
    }
    xSemaphoreGive(s_lock);
    return n;
}

void boost_model_clear_logs(void)
{
    if (s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log_head = 0;
    s_log_count = 0;
    s_log_divider = 0;
    xSemaphoreGive(s_lock);
}

void boost_model_set_media_status(const boost_media_status_t *status)
{
    if (status == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_media = *status;
    xSemaphoreGive(s_lock);
}

void boost_model_get_media_status(boost_media_status_t *out)
{
    if (out == NULL || s_lock == NULL) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_media;
    xSemaphoreGive(s_lock);
}
