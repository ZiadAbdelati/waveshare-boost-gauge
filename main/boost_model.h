#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "boost_sim.h"
#include "boost_theme.h"
#include "boost_display.h"
#include "boost_obd.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BOOST_ZONE_MAX 8
/* Background log ring: 5 Hz for one hour. Capacity / interval = 18000 / 5 = 3600 s. */
#define BOOST_LOG_INTERVAL_MS 200
#define BOOST_LOG_CAPACITY 18000

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
    float zero_angle;
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
    int active_page;
    boost_display_metrics_t display;
    /* Real-sensor diagnostics, mirrored from the latest sample. Let the parent
     * sanity-check readings on hardware (engine off: ambient ~101 kPa, gauge
     * ~0 psi). Zero/false on the demo path. */
    float map_volts;
    float map_abs_kpa;
    float ambient_kpa;
    bool ads_present;
    bool bmp_present;
    bool sensor_fault;
    /* TPMS snapshot mirrored from the mock/BLE provider. Four wheels in
     * FL/FR/RL/RR order; psi and valid per wheel; status is the service-level
     * enum (0=normal, 1=stale, 2=disconnected). */
    float tpms_psi[4];
    bool tpms_valid[4];
    int tpms_status;
    /* OBD2 BLE link (generic mode-01 PIDs + TPMS source). state: 0 disabled,
     * 1 scanning, 2 connecting, 3 ready, 4 error. valid=false until the first
     * successful reading ages within the staleness window. */
    int obd_state;
    uint16_t obd_last_error;
    char obd_peer[24];
    char obd_peer_addr[32];
    uint32_t obd_uptime_ms;
    uint32_t obd_age_ms;
    bool obd_valid;
    float obd_rpm;
    float obd_speed_kph;
    float obd_coolant_c;
    float obd_map_kpa;
    float obd_iat_c;
    float obd_throttle_pct;
    float obd_maf_gps;
    float obd_fuel_pct;
    float obd_battery_v;
    char obd_last_reply[48];
    char obd_protocol[24];
} boost_state_t;

typedef struct {
    uint32_t t_ms;
    float psi;
    float peak_psi;
    char zone[BOOST_ZONE_MAX];
    bool demo;
} boost_log_sample_t;

esp_err_t boost_model_init(void);
void boost_model_publish_sample(const boost_sample_t *sample);
void boost_model_publish_tpms(const float psi[4], const bool valid[4], int status);
void boost_model_publish_obd(const boost_obd_state_t *obd);
void boost_model_get_state(boost_state_t *out);
/** Refresh web-visible clocks/brightness; call outside the LVGL worker. */
void boost_model_refresh_status(void);
void boost_model_set_display_metrics(const boost_display_metrics_t *metrics);
void boost_model_get_config(boost_config_t *out);
esp_err_t boost_model_update_config(const boost_config_t *patch, uint32_t fields);
esp_err_t boost_model_set_active_theme(const char *id);
esp_err_t boost_model_set_active_page(int page);
const boost_theme_t *boost_model_active_theme(void);
esp_err_t boost_model_set_time(int64_t epoch_ms, int timezone_offset_minutes);
void boost_model_apply_schedule(void);
bool boost_model_schedule_wants_low(void);
/** Brightness the panel should boot at (dim-schedule night level if the
 * schedule is enabled and the wall clock is valid, else the daytime level).
 * No SPI/display access - safe to call before boost_display_start(). */
int boost_model_boot_brightness(void);

size_t boost_model_copy_logs(boost_log_sample_t *out, size_t max_count);
void boost_model_clear_logs(void);

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
    BOOST_CONFIG_ZERO_ANGLE = 1u << 10,
};

#ifdef __cplusplus
}
#endif
