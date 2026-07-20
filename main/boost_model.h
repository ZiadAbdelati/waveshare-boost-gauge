#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "boost_sim.h"
#include "boost_theme.h"
#include "boost_display.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOST_ZONE_MAX 8
#define BOOST_LOG_CAPACITY 1800

typedef struct {
    bool enabled;
    int start_minutes;
    int end_minutes;
} boost_dim_schedule_t;

typedef struct {
    int brightness_high;
    int brightness_low;
    boost_dim_schedule_t dim_schedule;
    int timezone_offset_minutes;
    char active_theme_id[BOOST_THEME_ID_MAX];
    float psi_min;
    float psi_max;
    float psi_overboost;
} boost_config_t;

typedef struct {
    float psi;
    float peak_psi;
    char zone[BOOST_ZONE_MAX];
    bool demo;
    int brightness;
    const char *firmware_version;
    uint64_t uptime_ms;
    int64_t epoch_ms;
    int timezone_offset_minutes;
    char active_theme_id[BOOST_THEME_ID_MAX];
    boost_display_metrics_t display;
} boost_state_t;

typedef struct {
    uint32_t t_ms;
    float psi;
    float peak_psi;
    char zone[BOOST_ZONE_MAX];
    bool demo;
} boost_log_sample_t;

typedef struct {
    char name[32];
    size_t size;
    uint32_t uploaded_at_ms;
    bool present;
    bool playback_supported;
} boost_media_status_t;

esp_err_t boost_model_init(void);
void boost_model_publish_sample(const boost_sample_t *sample);
void boost_model_get_state(boost_state_t *out);
/** Refresh web-visible clocks/brightness; call outside the LVGL worker. */
void boost_model_refresh_status(void);
void boost_model_set_display_metrics(const boost_display_metrics_t *metrics);
void boost_model_get_config(boost_config_t *out);
esp_err_t boost_model_update_config(const boost_config_t *patch, uint32_t fields);
esp_err_t boost_model_set_active_theme(const char *id);
const boost_theme_t *boost_model_active_theme(void);
esp_err_t boost_model_set_time(int64_t epoch_ms, int timezone_offset_minutes);
void boost_model_apply_schedule(void);
bool boost_model_schedule_wants_low(void);

size_t boost_model_copy_logs(boost_log_sample_t *out, size_t max_count);
void boost_model_clear_logs(void);

void boost_model_set_media_status(const boost_media_status_t *status);
void boost_model_get_media_status(boost_media_status_t *out);

enum {
    BOOST_CONFIG_BRIGHTNESS_HIGH = 1u << 0,
    BOOST_CONFIG_BRIGHTNESS_LOW = 1u << 1,
    BOOST_CONFIG_DIM_ENABLED = 1u << 2,
    BOOST_CONFIG_DIM_START = 1u << 3,
    BOOST_CONFIG_DIM_END = 1u << 4,
    BOOST_CONFIG_TZ_OFFSET = 1u << 5,
    BOOST_CONFIG_THEME = 1u << 6,
    BOOST_CONFIG_PSI_MIN = 1u << 7,
    BOOST_CONFIG_PSI_MAX = 1u << 8,
    BOOST_CONFIG_PSI_OVERBOOST = 1u << 9,
};

#ifdef __cplusplus
}
#endif
