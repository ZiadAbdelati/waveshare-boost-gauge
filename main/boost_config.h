#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOST_FW_VERSION_STR "0.2.4"
typedef enum {
    BOOST_THEME_NIGHT_BLACK = 0,
    BOOST_THEME_GHOST_GRAY  = 1,
    BOOST_THEME_COUNT
} boost_theme_id_t;

typedef enum {
    BOOST_UNITS_PSI = 0,
} boost_units_t;

typedef struct {
    uint8_t brightness;       /**< 0–100 */
    uint8_t theme_id;         /**< boost_theme_id_t */
    uint8_t dim_enable;       /**< schedule auto-dim */
    uint8_t dim_start_hour;   /**< local hour 0–23 */
    uint8_t dim_start_min;
    uint8_t dim_end_hour;
    uint8_t dim_end_min;
    int16_t tz_offset_min;    /**< minutes east of UTC, e.g. -300 for US Central */
    uint8_t units;            /**< boost_units_t */
    char    ap_pass[16];      /**< empty = open AP */
} boost_config_t;

void boost_config_init(void);
void boost_config_get_copy(boost_config_t *out);

/** Replace config, persist to NVS, and apply non-LVGL side effects. */
bool boost_config_set(const boost_config_t *cfg);

bool boost_config_set_brightness(uint8_t percent);
bool boost_config_set_theme(uint8_t theme_id);

/** True if local time is inside the configured dim window. */
bool boost_config_dim_window_active(void);

/** Apply scheduled dim if enabled (call periodically). */
void boost_config_apply_schedule(void);

const char *boost_theme_name(uint8_t theme_id);

#ifdef __cplusplus
}
#endif
