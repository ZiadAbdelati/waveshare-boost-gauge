#include "boost_sensors.h"

#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "boost_sensors";

/* ---------------------------------------------------------------------------
 * Bus + address map
 * ---------------------------------------------------------------------------
 * Our sensors live on a bus that is deliberately separate from the BSP's
 * touch/IO-expander bus (BSP_I2C_SCL=GPIO14, BSP_I2C_SDA=GPIO15, port 1). We
 * take I2C port 0 on the exposed GPIO18/17 pads, which the BSP pin map never touches, so there
 * is no double-init of a bus the BSP already owns and no pin collision.
 *
 * 100 kHz is deliberate and load-bearing: it is the rate the bus was proven
 * stable at during the hardware investigation. Do not raise it.
 */
#define SENS_I2C_PORT       I2C_NUM_0
#define SENS_SCL_GPIO       18
#define SENS_SDA_GPIO       17
#define SENS_I2C_HZ         100000
#define SENS_IO_TIMEOUT_MS  50

#define ADS1115_ADDR        0x48   /* ADDR pin -> GND */
#define BMP280_ADDR         0x76   /* SDO pin  -> GND */
#define DS3231_ADDR         0x68   /* battery-backed UTC RTC */
/* A single probe on this bus has been observed to ACK a phantom address once;
 * four consecutive ACKs is the filter that made the scanner trustworthy. */
#define SCAN_CONFIRM_ATTEMPTS 4

/* ---------------------------------------------------------------------------
 * ADS1115 (GM 3-bar MAP on A0, single-ended)
 * ---------------------------------------------------------------------------
 * Registers: 0x00 conversion, 0x01 config.
 *
 * Config word we program (continuous conversion so the reader just fetches the
 * latest code without a per-read wait):
 *   OS=1  MUX=100 (AIN0 vs GND)  PGA=000 (+/-6.144 V FSR)  MODE=0 (continuous)
 *   DR=101 (250 SPS)  COMP disabled (COMP_QUE=11)
 *   -> high byte 0xC0, low byte 0xA3
 *
 * PGA note: the GM 3-bar sensor is ratiometric to its supply and swings up to
 * ~4.8 V, so the ADS1115 must be 5 V powered and the +/-6.144 V range is the
 * only one that covers the full span. LSB = 6.144/32768 = 187.5 uV. If the ADS
 * is instead run at 3.3 V, the input cannot exceed VDD; change the divider /
 * range accordingly. These are the two knobs to touch, nothing else.
 */
#define ADS_REG_CONVERSION  0x00
#define ADS_REG_CONFIG      0x01
#define ADS_CONFIG_HI       0xC0
#define ADS_CONFIG_LO       0xA3
#define ADS_FSR_VOLTS       6.144f
#define ADS_VOLTS_PER_LSB   (ADS_FSR_VOLTS / 32768.0f)

/* ---------------------------------------------------------------------------
 * GM 12223861 three-bar MAP transfer function  (absolute pressure)
 * ---------------------------------------------------------------------------
 * The published curve is a straight line through two points, both specified at
 * a 5.00 V supply:
 *
 *     0.619 V -> 40 kPa
 *     4.818 V -> 304 kPa
 *
 * so, in kPa per volt,
 *
 *     slope     = (304 - 40) / (4.818 - 0.619)
 *               = 264 / 4.199
 *               = 62.8721124...          -> MAP_KPA_PER_VOLT
 *     intercept = 40 - slope * 0.619
 *               = 40 - 38.9178375...
 *               =  1.08216242...         -> MAP_KPA_INTERCEPT
 *
 * Check: 62.8721124 * 4.818 + 1.08216242 = 304.000 kPa.
 *
 * The sensor is ratiometric, so a supply that is not 5.00 V scales the whole
 * output. Normalize the measured voltage back onto the 5.00 V curve before
 * applying the line:
 *
 *     normalized_volts = map_volts * 5.00 / supply_volts
 *     nominal_kpa      = MAP_KPA_PER_VOLT * normalized_volts + MAP_KPA_INTERCEPT
 *     corrected_kpa    = nominal_kpa + cal.offset_kpa
 *     gauge_kpa        = corrected_kpa - ambient_kpa
 *
 * This replaces an earlier "volts / 0.0059 V-per-kPa" approximation that read a
 * real 1.5735 V atmosphere sample as 165 kPa. Do not re-derive these constants
 * from a slope-only datasheet blurb; the intercept is not zero.
 *
 * boost_sensors_nominal_kpa() is the only place this arithmetic lives.
 */
#define MAP_CURVE_SUPPLY_V  5.00f          /* supply the two-point fit is defined at */
#define MAP_KPA_PER_VOLT    62.8721124f    /* 264 / 4.199 */
#define MAP_KPA_INTERCEPT   1.08216242f    /* 40 - 62.8721124 * 0.619 */

/* ---------------------------------------------------------------------------
 * BMP280 (ambient reference)
 * ---------------------------------------------------------------------------
 */
#define BMP280_REG_ID        0xD0   /* 0x58 for BMP280 (0x60 = BME280) */
#define BMP280_REG_RESET     0xE0
#define BMP280_REG_CALIB     0x88   /* 24 bytes: dig_T1..T3, dig_P1..P9 */
#define BMP280_REG_CTRL_MEAS 0xF4
#define BMP280_REG_CONFIG    0xF5
#define BMP280_REG_PRESS_MSB 0xF7   /* press[3], temp[3] contiguous from here */
#define BMP280_CHIP_ID       0x58
/* ctrl_meas: osrs_t x1 (001), osrs_p x4 (011), mode normal (11) -> 0x2F */
#define BMP280_CTRL_MEAS     0x2F
/* config: t_sb 0.5ms (000), IIR filter x4 (010), spi3w off -> 0x08 */
#define BMP280_CONFIG        0x08

/* Fallback when the BMP280 is absent: run off a standard atmosphere so the
 * gauge is still usable (degraded), rather than reading nonsense. Whenever this
 * constant is in force the published sample sets ambient_is_fallback, and
 * calibration refuses to use it as a reference. */
#define STANDARD_ATM_KPA     101.325f

#define KPA_TO_PSI           0.145037738f

/* Reader cadence. ADS every loop; BMP once every N loops (ambient drifts
 * slowly, and forced/normal-mode BMP reads are the slow ones we keep off the
 * fast path). The 16 ms loop is the AGENTS.md cadence contract: 62.5 Hz MAP,
 * and 62.5/10 = 6.25 Hz ambient. */
#define SENS_LOOP_MS         16
#define BMP_EVERY_N          10

/* Sustained read failures before we assume the bus is wedged and try to clock
 * it free. 63 loops x 16 ms = 1008 ms, i.e. about one second; the same count
 * also acts as the backoff before the next attempt. */
#define SENS_RECOVER_AFTER   63

/* ---------------------------------------------------------------------------
 * Persistence
 * ---------------------------------------------------------------------------
 * Same "boost" namespace as boost_theme.c, but two dedicated keys rather than a
 * field inside boost_config_t: growing the calibration record must never force
 * a migration of (or risk discarding) the existing gauge settings.
 *
 * map_vsup is the source of truth for the configured supply and is stored on
 * its own so it survives an unreadable/absent calibration record. The record is
 * versioned and only accepted when both the version and the blob size match;
 * anything else is treated as never-calibrated.
 */
#define NVS_NS               "boost"
#define NVS_KEY_SUPPLY       "map_vsup"
#define NVS_KEY_CAL          "map_cal"

/* ---------------------------------------------------------------------------
 * Atmospheric calibration window
 * ---------------------------------------------------------------------------
 * The reader task is the single owner of I2C traffic, so calibration only
 * observes published snapshots. ~2 s at a 50 ms tick = 40 polls, over which a
 * 6.25 Hz BMP delivers roughly 12 fresh values.
 */
#define CAL_POLL_MS          50
#define CAL_POLLS            40
#define CAL_MAX_ADS_AGE_MS   500
#define CAL_MAX_BMP_AGE_MS   1000
#define CAL_MIN_BMP_UPDATES  4
#define CAL_NOMINAL_MIN_KPA  20.0f
#define CAL_NOMINAL_MAX_KPA  200.0f
#define CAL_BMP_MIN_KPA      50.0f
#define CAL_BMP_MAX_KPA      115.0f
#define CAL_MAX_VOLT_SPREAD  0.02f
#define CAL_MAX_KPA_SPREAD   0.30f
/* Anything before 2020-01-01 is an unsynchronised boot clock, not a wall clock;
 * record 0 instead of a misleading 1970 timestamp. */
#define CAL_EPOCH_VALID_SEC  1577836800LL

/* --- BMP280 factory calibration (read from NVM at init) --- */
static uint16_t s_dig_t1;
static int16_t  s_dig_t2, s_dig_t3;
static uint16_t s_dig_p1;
static int16_t  s_dig_p2, s_dig_p3, s_dig_p4, s_dig_p5, s_dig_p6, s_dig_p7, s_dig_p8, s_dig_p9;
static int32_t  s_t_fine;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_ads;
static i2c_master_dev_handle_t s_bmp;
static i2c_master_dev_handle_t s_rtc;
static bool s_ads_present;
static bool s_bmp_present;
static bool s_rtc_present;

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_bus_admin_lock; /* serializes reset against HTTP scan */
static SemaphoreHandle_t s_cal_lock;       /* guards s_supply_volts / s_cal / s_cal_busy */
static boost_sample_t s_latest;   /* guarded by s_lock */
static float s_peak;              /* guarded by s_lock */
static uint32_t s_recoveries;     /* count of bus recoveries (reader task only) */

/* Guarded by s_cal_lock. s_cal.version == 0 means never calibrated. Lock order
 * is always s_cal_lock before s_lock; never the other way round. */
static float s_supply_volts = BOOST_MAP_SUPPLY_DEFAULT;
static boost_map_cal_t s_cal;
static bool s_cal_busy;

/* ------------------------------------------------------------------ helpers */

static esp_err_t reg_write8(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(dev, buf, sizeof(buf), SENS_IO_TIMEOUT_MS);
}

static esp_err_t reg_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, out, n, SENS_IO_TIMEOUT_MS);
}

/* Age of a timestamp in ms. last_us <= 0 means "never read successfully". */
static uint32_t age_ms(int64_t last_us, int64_t now_us)
{
    if (last_us <= 0) {
        return UINT32_MAX;
    }
    int64_t delta = (now_us - last_us) / 1000;
    if (delta < 0) {
        delta = 0;
    }
    if (delta >= (int64_t)UINT32_MAX) {
        return UINT32_MAX - 1u;   /* stale, but distinguishable from "never" */
    }
    return (uint32_t)delta;
}

/* ------------------------------------------------------------- conversion */

/* The transfer function itself, with the supply passed explicitly so the
 * supply-change path can re-evaluate the stored reference without disturbing
 * live state. Everything else goes through boost_sensors_nominal_kpa(). */
static float nominal_kpa_at(float map_volts, float supply_volts)
{
    if (!isfinite(map_volts) || !isfinite(supply_volts) || supply_volts <= 0.0f) {
        return NAN;
    }
    const float normalized = map_volts * (MAP_CURVE_SUPPLY_V / supply_volts);
    return MAP_KPA_PER_VOLT * normalized + MAP_KPA_INTERCEPT;
}

static float cal_supply_volts(void)
{
    float v = BOOST_MAP_SUPPLY_DEFAULT;
    if (s_cal_lock != NULL && xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        v = s_supply_volts;
        xSemaphoreGive(s_cal_lock);
    }
    return v;
}

static float cal_offset_kpa(void)
{
    float off = 0.0f;
    if (s_cal_lock != NULL && xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        if (s_cal.version == BOOST_MAP_CAL_VERSION) {
            off = s_cal.offset_kpa;
        }
        xSemaphoreGive(s_cal_lock);
    }
    return off;
}

float boost_sensors_nominal_kpa(float map_volts)
{
    return nominal_kpa_at(map_volts, cal_supply_volts());
}

/* ------------------------------------------------------------------- ADS1115 */

static bool ads_configure(void)
{
    /* Config register is written big-endian (MSB first). */
    const uint8_t buf[3] = { ADS_REG_CONFIG, ADS_CONFIG_HI, ADS_CONFIG_LO };
    if (i2c_master_transmit(s_ads, buf, sizeof(buf), SENS_IO_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    return true;
}

/* Returns true and fills *volts with the A0 voltage, false on a bus error. */
static bool ads_read_volts(float *volts)
{
    uint8_t raw[2];
    if (reg_read(s_ads, ADS_REG_CONVERSION, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    const int16_t code = (int16_t)((raw[0] << 8) | raw[1]);
    /* Negative codes mean the input is below ground; clamp to 0 V for a
     * ratiometric sensor that should never go negative. */
    float v = (float)code * ADS_VOLTS_PER_LSB;
    if (v < 0.0f) {
        v = 0.0f;
    }
    *volts = v;
    return true;
}

/* -------------------------------------------------------------------- BMP280 */

static bool bmp_load_calibration(void)
{
    uint8_t c[24];
    if (reg_read(s_bmp, BMP280_REG_CALIB, c, sizeof(c)) != ESP_OK) {
        return false;
    }
    s_dig_t1 = (uint16_t)(c[0]  | (c[1]  << 8));
    s_dig_t2 = (int16_t) (c[2]  | (c[3]  << 8));
    s_dig_t3 = (int16_t) (c[4]  | (c[5]  << 8));
    s_dig_p1 = (uint16_t)(c[6]  | (c[7]  << 8));
    s_dig_p2 = (int16_t) (c[8]  | (c[9]  << 8));
    s_dig_p3 = (int16_t) (c[10] | (c[11] << 8));
    s_dig_p4 = (int16_t) (c[12] | (c[13] << 8));
    s_dig_p5 = (int16_t) (c[14] | (c[15] << 8));
    s_dig_p6 = (int16_t) (c[16] | (c[17] << 8));
    s_dig_p7 = (int16_t) (c[18] | (c[19] << 8));
    s_dig_p8 = (int16_t) (c[20] | (c[21] << 8));
    s_dig_p9 = (int16_t) (c[22] | (c[23] << 8));
    return true;
}

/* Bosch datasheet integer compensation. Temperature must be compensated first
 * because it sets s_t_fine, which the pressure formula consumes. */
static int32_t bmp_compensate_temp(int32_t adc_t)
{
    int32_t var1 = ((((adc_t >> 3) - ((int32_t)s_dig_t1 << 1))) * ((int32_t)s_dig_t2)) >> 11;
    int32_t var2 = (((((adc_t >> 4) - ((int32_t)s_dig_t1)) *
                      ((adc_t >> 4) - ((int32_t)s_dig_t1))) >> 12) * ((int32_t)s_dig_t3)) >> 14;
    s_t_fine = var1 + var2;
    return (s_t_fine * 5 + 128) >> 8;   /* 0.01 degC (unused, but keeps t_fine honest) */
}

/* Returns pressure in Pa (Q24.8 -> divide by 256). 0 on the div-by-zero guard. */
static uint32_t bmp_compensate_press(int32_t adc_p)
{
    int64_t var1 = ((int64_t)s_t_fine) - 128000;
    int64_t var2 = var1 * var1 * (int64_t)s_dig_p6;
    var2 = var2 + ((var1 * (int64_t)s_dig_p5) << 17);
    var2 = var2 + (((int64_t)s_dig_p4) << 35);
    var1 = ((var1 * var1 * (int64_t)s_dig_p3) >> 8) + ((var1 * (int64_t)s_dig_p2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)s_dig_p1) >> 33;
    if (var1 == 0) {
        return 0;   /* avoid division by zero */
    }
    int64_t p = 1048576 - adc_p;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)s_dig_p9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)s_dig_p8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)s_dig_p7) << 4);
    return (uint32_t)p;
}

/* Returns true and fills *kpa with ambient pressure, false on a bus error. */
static bool bmp_read_kpa(float *kpa)
{
    uint8_t d[6];
    if (reg_read(s_bmp, BMP280_REG_PRESS_MSB, d, sizeof(d)) != ESP_OK) {
        return false;
    }
    const int32_t adc_p = (int32_t)(((uint32_t)d[0] << 12) | ((uint32_t)d[1] << 4) | (d[2] >> 4));
    const int32_t adc_t = (int32_t)(((uint32_t)d[3] << 12) | ((uint32_t)d[4] << 4) | (d[5] >> 4));
    (void)bmp_compensate_temp(adc_t);              /* sets s_t_fine */
    const uint32_t pa_q248 = bmp_compensate_press(adc_p);
    if (pa_q248 == 0) {
        return false;
    }
    *kpa = ((float)pa_q248 / 256.0f) / 1000.0f;
    return true;
}

/* ------------------------------------------------------------- bus recovery */

/* The two device configs live here so init and recovery add them identically. */
static esp_err_t add_ads(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = SENS_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_ads);
}

static esp_err_t add_bmp(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP280_ADDR,
        .scl_speed_hz = SENS_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_bmp);
}

static esp_err_t add_rtc(void)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = SENS_I2C_HZ,
    };
    return i2c_master_bus_add_device(s_bus, &cfg, &s_rtc);
}

/* Reset the existing controller in place. This uses ESP-IDF's supported bus/FSM
 * clear without invalidating the bus and device handles or exposing the pins. */
static void bus_recover(void)
{
    if (s_bus == NULL || s_bus_admin_lock == NULL ||
        xSemaphoreTake(s_bus_admin_lock, portMAX_DELAY) != pdTRUE) {
        ESP_LOGE(TAG, "I2C bus reset lock failed");
        return;
    }

    const esp_err_t reset_err = i2c_master_bus_reset(s_bus);
    if (reset_err == ESP_OK) {
        if (s_ads_present && s_ads != NULL) {
            ads_configure();
        }
        if (s_bmp_present && s_bmp != NULL) {
            /* calib stays valid across recovery; just re-arm the mode registers. */
            reg_write8(s_bmp, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS);
            reg_write8(s_bmp, BMP280_REG_CONFIG, BMP280_CONFIG);
        }
    }
    xSemaphoreGive(s_bus_admin_lock);

    if (reset_err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus reset failed");
        return;
    }
    ++s_recoveries;
    ESP_LOGW(TAG, "I2C bus recovery attempt #%u", (unsigned)s_recoveries);
}

/* ------------------------------------------------------------------ DS3231 */

/* Battery-backed UTC RTC on the same sensor bus. Registers 0x00..0x06 hold
 * BCD seconds..year; a burst read freezes the copy so it is atomic, and
 * writing the time registers clears the oscillator-stop flag. */
#define DS3231_REG_STATUS   0x0F
#define DS3231_OSF          0x80   /* set until the time has been written */

static uint8_t rtc_bcd2bin(uint8_t b)
{
    return (uint8_t)(((b >> 4) & 0x0F) * 10 + (b & 0x0F));
}

static uint8_t rtc_bin2bcd(uint8_t v)
{
    return (uint8_t)((((v / 10) % 10) << 4) | (v % 10));
}

static bool rtc_bcd_ok(uint8_t b)
{
    return (b & 0x0F) <= 9 && ((b >> 4) & 0x0F) <= 9;
}

bool boost_sensors_rtc_present(void)
{
    return s_rtc_present;
}

/* UTC epoch ms from a civil date (mon 1..12). The toolchain's newlib lacks
 * timegm(), so the conversion is the standard days-from-civil arithmetic. */
static int64_t rtc_epoch_ms(int year, int mon, int day, int hour, int min, int sec)
{
    const int64_t y = year - (mon <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + doe - 719468;   /* days since 1970-01-01 */
    return (days * 86400 + hour * 3600 + min * 60 + sec) * 1000;
}

esp_err_t boost_sensors_rtc_read(int64_t *epoch_ms)
{
    if (epoch_ms == NULL || s_bus == NULL || !s_rtc_present || s_rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t st = 0;
    if (reg_read(s_rtc, DS3231_REG_STATUS, &st, 1) != ESP_OK || (st & DS3231_OSF)) {
        /* Oscillator-stop flag: the time has never been set since power-up. */
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t b[7];   /* 0x00..0x06: sec, min, hr, day, date, month/century, year */
    if (reg_read(s_rtc, 0x00, b, sizeof(b)) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if ((b[0] & 0x80) ||                       /* CH: oscillator stopped */
        !rtc_bcd_ok(b[0] & 0x7F) || !rtc_bcd_ok(b[1] & 0x7F) ||
        !rtc_bcd_ok(b[2] & 0x3F) || !rtc_bcd_ok(b[4] & 0x3F) ||
        !rtc_bcd_ok(b[5] & 0x1F) || !rtc_bcd_ok(b[6])) {
        return ESP_ERR_INVALID_STATE;          /* garbage / non-BCD registers */
    }
    const uint8_t secs = rtc_bcd2bin(b[0] & 0x7F);
    const uint8_t mins = rtc_bcd2bin(b[1] & 0x7F);
    uint8_t hrs = b[2] & 0x3F;
    if (b[2] & 0x40) {                         /* 12-hour mode: normalise */
        const bool pm = (b[2] & 0x20) != 0;
        hrs = rtc_bcd2bin(hrs);
        if (pm) {
            hrs = (hrs == 12) ? 12 : (uint8_t)(hrs + 12);
        } else if (hrs == 12) {
            hrs = 0;
        }
    } else {
        hrs = rtc_bcd2bin(hrs);
    }
    const uint8_t date = rtc_bcd2bin(b[4] & 0x3F);
    const uint8_t mon  = rtc_bcd2bin(b[5] & 0x1F);
    const uint8_t year = rtc_bcd2bin(b[6]);
    if (secs > 59 || mins > 59 || hrs > 23 || date < 1 || date > 31 ||
        mon < 1 || mon > 12 || year > 99) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t ms = rtc_epoch_ms(2000 + year, mon, date, hrs, mins, secs);
    if (ms < 1700000000000LL) {            /* before 2023: implausible */
        return ESP_ERR_INVALID_STATE;
    }
    *epoch_ms = ms;
    return ESP_OK;
}

esp_err_t boost_sensors_rtc_write(int64_t epoch_ms)
{
    if (epoch_ms <= 0 || s_bus == NULL || !s_rtc_present || s_rtc == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const time_t secs = (time_t)(epoch_ms / 1000);
    struct tm t;
    gmtime_r(&secs, &t);
    if (t.tm_year + 1900 < 2000 || t.tm_year + 1900 > 2099) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Burst write: pointer to the seconds register, then sec..year. CH (sec
     * bit 7) is 0 so the oscillator keeps/restarts; hour bit 6 is 0 for
     * 24-hour mode; the century bit in the month register stays 0 (2000s). */
    const uint8_t buf[8] = {
        0x00,
        rtc_bin2bcd((uint8_t)t.tm_sec),
        rtc_bin2bcd((uint8_t)t.tm_min),
        rtc_bin2bcd((uint8_t)t.tm_hour),
        rtc_bin2bcd((uint8_t)(t.tm_wday ? t.tm_wday : 7)),  /* 1..7, arbitrary */
        rtc_bin2bcd((uint8_t)t.tm_mday),
        rtc_bin2bcd((uint8_t)(t.tm_mon + 1)),
        rtc_bin2bcd((uint8_t)(t.tm_year - 100)),            /* year - 2000 */
    };
    return i2c_master_transmit(s_rtc, buf, sizeof(buf), SENS_IO_TIMEOUT_MS);
}

/* ---------------------------------------------------------------- reader task */

static void publish(const boost_sample_t *s, float peak)
{
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_latest = *s;
        s_latest.peak_psi = peak;
        s_peak = peak;
        xSemaphoreGive(s_lock);
    }
}

static void sensors_task(void *arg)
{
    (void)arg;

    /* Seed from whatever init found; hold these across read failures. */
    float last_volts = 0.0f;
    float ambient_kpa = STANDARD_ATM_KPA;
    bool have_map = false;
    bool have_ambient = false;
    uint32_t loop = 0;
    int consecutive_fail = 0;

    /* Freshness bookkeeping. Touched only by this task; copied into the sample
     * so consumers never have to guess whether a presence flag is still true. */
    int64_t ads_last_us = 0;
    int64_t bmp_last_us = 0;
    uint32_t bmp_updates = 0;

    TickType_t next = xTaskGetTickCount();
    while (true) {
        bool fault = false;

        /* MAP: fast path, every loop. */
        if (s_ads_present && s_ads != NULL) {
            float v;
            if (ads_read_volts(&v)) {
                last_volts = v;
                have_map = true;
                ads_last_us = esp_timer_get_time();
                consecutive_fail = 0;
            } else {
                fault = true;   /* hold last good */
                ++consecutive_fail;
            }
        } else {
            fault = true;       /* no MAP source at all */
        }

        /* A run of failures means the bus is likely wedged; reset the hardware
         * bus/FSM in place rather than fault forever.
         * Only worth it if something was ever detected to recover to. */
        if (consecutive_fail >= SENS_RECOVER_AFTER && (s_ads_present || s_bmp_present)) {
            bus_recover();
            consecutive_fail = 0;   /* backoff: another full second before retry */
        }

        /* Ambient: slow path, every BMP_EVERY_N loops. */
        if (s_bmp_present && s_bmp != NULL && (loop % BMP_EVERY_N) == 0) {
            float k;
            if (bmp_read_kpa(&k)) {
                ambient_kpa = k;
                have_ambient = true;
                bmp_last_us = esp_timer_get_time();
                ++bmp_updates;
            } else {
                /* Not a hard fault: a stale-but-recent ambient is fine, and if
                 * we have never had one, fall back to standard atmosphere. The
                 * age keeps growing so calibration still refuses it. */
                if (!have_ambient) {
                    ambient_kpa = STANDARD_ATM_KPA;
                }
            }
        }

        /* One conversion, one place: boost_sensors_nominal_kpa() owns the
         * transfer function and the supply normalization. */
        const float nominal_kpa = have_map ? boost_sensors_nominal_kpa(last_volts) : 0.0f;
        const float map_kpa = have_map ? nominal_kpa + cal_offset_kpa() : 0.0f;
        const float gauge_kpa = map_kpa - ambient_kpa;
        const float psi = have_map ? gauge_kpa * KPA_TO_PSI : 0.0f;

        float peak;
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            peak = s_peak;
            xSemaphoreGive(s_lock);
        } else {
            peak = 0.0f;
        }
        if (psi > peak) {
            peak = psi;
        }

        const int64_t now_us = esp_timer_get_time();
        boost_sample_t s = {
            .psi = psi,
            .peak_psi = peak,
            .demo = false,
            .map_volts = last_volts,
            .map_abs_kpa = map_kpa,
            .map_nominal_kpa = nominal_kpa,
            .ambient_kpa = ambient_kpa,
            .ads_present = s_ads_present,
            .bmp_present = s_bmp_present,
            .sensor_fault = fault,
            .ads_age_ms = age_ms(ads_last_us, now_us),
            .bmp_age_ms = age_ms(bmp_last_us, now_us),
            .bmp_updates = bmp_updates,
            .ambient_is_fallback = !have_ambient,
        };
        publish(&s, peak);

        ++loop;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SENS_LOOP_MS));
    }
}

/* ------------------------------------------------------------- persistence */

/* Mount NVS here rather than relying on being called after boost_theme_init().
 * Getting that order wrong is silent: nvs_open() just fails, the boot-time read
 * is skipped, and writes appear to work right up until the next reboot drops
 * them. nvs_flash_init() is idempotent, so calling it again is free. */
static bool nvs_ready(void)
{
    static bool mounted;
    if (mounted) {
        return true;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    mounted = (err == ESP_OK);
    if (!mounted) {
        ESP_LOGE(TAG, "NVS mount failed (%s)", esp_err_to_name(err));
    }
    return mounted;
}

/* Write either or both records in one transaction. NULL means "leave alone".
 * Returns false if anything failed, in which case the caller must not activate
 * whatever it was trying to save. */
static bool persist_settings(const float *supply, const boost_map_cal_t *cal)
{
    if (!nvs_ready()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    esp_err_t err = ESP_OK;
    if (supply != NULL) {
        err = nvs_set_blob(h, NVS_KEY_SUPPLY, supply, sizeof(*supply));
    }
    if (err == ESP_OK && cal != NULL) {
        err = nvs_set_blob(h, NVS_KEY_CAL, cal, sizeof(*cal));
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "calibration persist failed (%s)", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Restore the configured supply and the calibration record. Must run before the
 * reader task starts, so the very first published sample already uses them. */
static void load_settings(void)
{
    if (!nvs_ready()) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;   /* namespace not created yet: defaults stand */
    }

    float supply = 0.0f;
    size_t len = sizeof(supply);
    if (nvs_get_blob(h, NVS_KEY_SUPPLY, &supply, &len) == ESP_OK &&
        len == sizeof(supply) && isfinite(supply) &&
        supply >= BOOST_MAP_SUPPLY_MIN && supply <= BOOST_MAP_SUPPLY_MAX) {
        s_supply_volts = supply;
    }

    /* Accepted only when both the version and the exact blob size match; a
     * record written by another schema is treated as never-calibrated rather
     * than reinterpreted field by field. */
    boost_map_cal_t rec;
    size_t rec_len = sizeof(rec);
    if (nvs_get_blob(h, NVS_KEY_CAL, &rec, &rec_len) == ESP_OK &&
        rec_len == sizeof(rec) && rec.version == BOOST_MAP_CAL_VERSION &&
        isfinite(rec.offset_kpa) && fabsf(rec.offset_kpa) <= BOOST_MAP_CAL_MAX_KPA) {
        s_cal = rec;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "MAP supply %.2f V, calibration %s (offset %.3f kPa)",
             (double)s_supply_volts,
             s_cal.version == BOOST_MAP_CAL_VERSION ? "restored" : "none",
             (double)s_cal.offset_kpa);
}

/* ---------------------------------------------------------------------- init */

/* Probe, add, and configure both sensors on the current bus, setting the
 * present flags. Factored out so the boot path can call it a second time after
 * an in-place bus reset. */
static void detect_sensors(void)
{
    if (i2c_master_probe(s_bus, ADS1115_ADDR, SENS_IO_TIMEOUT_MS) == ESP_OK &&
        add_ads() == ESP_OK && ads_configure()) {
        s_ads_present = true;
        ESP_LOGI(TAG, "ADS1115 detected at 0x%02X (MAP on A0, +/-%.3f V FSR, continuous)",
                 ADS1115_ADDR, (double)ADS_FSR_VOLTS);
    } else {
        if (s_ads) { i2c_master_bus_rm_device(s_ads); s_ads = NULL; }
        s_ads_present = false;
        ESP_LOGW(TAG, "ADS1115 NOT detected at 0x%02X - MAP unavailable, gauge will fault",
                 ADS1115_ADDR);
    }

    if (i2c_master_probe(s_bus, BMP280_ADDR, SENS_IO_TIMEOUT_MS) == ESP_OK &&
        add_bmp() == ESP_OK) {
        uint8_t id = 0;
        const bool id_ok = reg_read(s_bmp, BMP280_REG_ID, &id, 1) == ESP_OK;
        if (id_ok && id == BMP280_CHIP_ID && bmp_load_calibration() &&
            reg_write8(s_bmp, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS) == ESP_OK &&
            reg_write8(s_bmp, BMP280_REG_CONFIG, BMP280_CONFIG) == ESP_OK) {
            s_bmp_present = true;
            ESP_LOGI(TAG, "BMP280 detected at 0x%02X (chip id 0x%02X, normal mode, calib loaded)",
                     BMP280_ADDR, id);
        } else {
            if (s_bmp) { i2c_master_bus_rm_device(s_bmp); s_bmp = NULL; }
            s_bmp_present = false;
            ESP_LOGW(TAG, "BMP280 at 0x%02X did not init (id 0x%02X) - using %.1f kPa ambient",
                     BMP280_ADDR, id, (double)STANDARD_ATM_KPA);
        }
    } else {
        if (s_bmp) { i2c_master_bus_rm_device(s_bmp); s_bmp = NULL; }
        s_bmp_present = false;
        ESP_LOGW(TAG, "BMP280 NOT detected at 0x%02X - using %.1f kPa standard atmosphere",
                 BMP280_ADDR, (double)STANDARD_ATM_KPA);
    }

    /* The RTC can be present while the MAP sensors are not (and vice versa),
     * so the handle must be torn down first to make re-detect idempotent. */
    if (s_rtc) { i2c_master_bus_rm_device(s_rtc); s_rtc = NULL; }
    s_rtc_present = false;
    if (i2c_master_probe(s_bus, DS3231_ADDR, SENS_IO_TIMEOUT_MS) == ESP_OK &&
        add_rtc() == ESP_OK) {
        uint8_t st = 0;
        if (reg_read(s_rtc, DS3231_REG_STATUS, &st, 1) == ESP_OK) {
            s_rtc_present = true;
            ESP_LOGI(TAG, "DS3231 detected at 0x%02X%s", DS3231_ADDR,
                     (st & DS3231_OSF) ? " (time never set - clock not trusted)" : "");
        } else {
            i2c_master_bus_rm_device(s_rtc);
            s_rtc = NULL;
        }
    }
    if (!s_rtc_present) {
        ESP_LOGW(TAG, "DS3231 NOT detected at 0x%02X - clock falls back to NVS/sync",
                 DS3231_ADDR);
    }
}

bool boost_sensors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }
    s_bus_admin_lock = xSemaphoreCreateMutex();
    if (s_bus_admin_lock == NULL) {
        ESP_LOGE(TAG, "I2C admin mutex alloc failed");
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return false;
    }
    s_cal_lock = xSemaphoreCreateMutex();
    if (s_cal_lock == NULL) {
        ESP_LOGE(TAG, "calibration mutex alloc failed");
        vSemaphoreDelete(s_bus_admin_lock);
        s_bus_admin_lock = NULL;
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
        return false;
    }
    /* A sane zero snapshot until the first read lands. */
    s_latest = (boost_sample_t){
        .demo = false,
        .ambient_kpa = STANDARD_ATM_KPA,
        .ads_age_ms = UINT32_MAX,
        .bmp_age_ms = UINT32_MAX,
        .ambient_is_fallback = true,
    };

    /* Supply voltage and calibration must be live before the reader task can
     * publish its first sample, otherwise one frame goes out uncorrected. */
    load_settings();

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = SENS_I2C_PORT,
        .sda_io_num = SENS_SDA_GPIO,
        .scl_io_num = SENS_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = { .enable_internal_pullup = true },
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed (SCL=GPIO%d SDA=GPIO%d port %d)",
                 SENS_SCL_GPIO, SENS_SDA_GPIO, SENS_I2C_PORT);
        return false;
    }
    ESP_LOGI(TAG, "sensor I2C up: port %d, SCL=GPIO%d, SDA=GPIO%d, %d kHz",
             SENS_I2C_PORT, SENS_SCL_GPIO, SENS_SDA_GPIO, SENS_I2C_HZ / 1000);

    detect_sensors();

    /* Retry once after the driver's supported in-place hardware/FSM reset,
     * preserving the original bus and device-handle lifecycle. */
    if (!s_ads_present && !s_bmp_present) {
        ESP_LOGW(TAG, "no sensors answered; resetting the bus in place and retrying");
        if (i2c_master_bus_reset(s_bus) == ESP_OK) {
            detect_sensors();
        }
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensors_task, "boost_sensors", 4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "sensor task create failed");
        return false;
    }

    ESP_LOGI(TAG, "sensor path ready: ADS1115 %s, BMP280 %s, DS3231 %s",
             s_ads_present ? "present" : "absent",
             s_bmp_present ? "present" : "absent",
             s_rtc_present ? "present" : "absent");
    return s_ads_present || s_bmp_present;
}

boost_sample_t boost_sensors_get_sample(void)
{
    boost_sample_t out;
    /* Short, non-blocking take: the reader holds the lock only for a struct
     * copy, so this practically never misses, but if it does we hand back the
     * last value we saw rather than stall the display. */
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        out = s_latest;
        xSemaphoreGive(s_lock);
    } else {
        out = s_latest;   /* aligned struct read; worst case one frame stale */
    }
    return out;
}

uint32_t boost_sensors_recoveries(void)
{
    return s_recoveries;
}

int boost_sensors_i2c_scan(uint8_t *out, int max)
{
    if (s_bus == NULL || s_bus_admin_lock == NULL) {
        return -1;   /* bus never initialised */
    }
    if (xSemaphoreTake(s_bus_admin_lock, portMAX_DELAY) != pdTRUE) {
        return -1;
    }
    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77 && n < max; ++addr) {
        bool stable_ack = true;
        for (int attempt = 0; attempt < SCAN_CONFIRM_ATTEMPTS; ++attempt) {
            if (i2c_master_probe(s_bus, addr, SENS_IO_TIMEOUT_MS) != ESP_OK) {
                stable_ack = false;
                break;
            }
        }
        if (stable_ack) {
            out[n++] = addr;
        }
    }
    xSemaphoreGive(s_bus_admin_lock);
    return n;
}

void boost_sensors_reset_peak(void)
{
    if (s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_peak = s_latest.psi > 0.0f ? s_latest.psi : 0.0f;
        s_latest.peak_psi = s_peak;
        xSemaphoreGive(s_lock);
    }
}

/* ------------------------------------------------------- calibration access */

boost_map_cal_t boost_sensors_get_calibration(void)
{
    boost_map_cal_t out = (boost_map_cal_t){0};
    if (s_cal_lock != NULL && xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        out = s_cal;
        xSemaphoreGive(s_cal_lock);
    }
    return out;
}

float boost_sensors_get_supply_volts(void)
{
    return cal_supply_volts();
}

esp_err_t boost_sensors_set_supply_volts(float volts)
{
    if (!isfinite(volts) || volts < BOOST_MAP_SUPPLY_MIN || volts > BOOST_MAP_SUPPLY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_cal_lock == NULL) {
        return ESP_FAIL;
    }

    boost_map_cal_t rec;
    if (xSemaphoreTake(s_cal_lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    rec = s_cal;
    xSemaphoreGive(s_cal_lock);

    /* Recompute from the stored reference rather than rescaling the old offset:
     * the offset is an additive kPa correction derived under one normalization,
     * so it has no meaning under another. ref_map_volts / ref_bmp_kpa are the
     * raw observation and survive any number of supply changes. */
    const bool have_cal = (rec.version == BOOST_MAP_CAL_VERSION);
    if (have_cal) {
        const float nominal = nominal_kpa_at(rec.ref_map_volts, volts);
        if (!isfinite(nominal)) {
            return ESP_FAIL;
        }
        rec.supply_volts = volts;
        rec.ref_nominal_kpa = nominal;
        rec.offset_kpa = rec.ref_bmp_kpa - nominal;
    }

    /* Persist before going live: a supply that survived the reboot but a
     * calibration that did not would silently apply the wrong correction. */
    if (!persist_settings(&volts, have_cal ? &rec : NULL)) {
        return ESP_FAIL;
    }

    if (xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        s_supply_volts = volts;
        if (have_cal) {
            s_cal = rec;
        }
        xSemaphoreGive(s_cal_lock);
    }
    ESP_LOGI(TAG, "MAP supply set to %.2f V%s", (double)volts,
             have_cal ? " (calibration offset recomputed)" : "");
    return ESP_OK;
}

/* These strings are the wire contract for the JSON `error` field and are
 * mirrored by the explanation table in web/app.js. Changing one without the
 * other degrades a specific diagnosis into a generic failure message. */
const char *boost_sensors_cal_error_code(boost_cal_result_t result)
{
    switch (result) {
        case BOOST_CAL_OK:                 return "ok";
        case BOOST_CAL_ERR_NO_ADS:         return "no_ads";
        case BOOST_CAL_ERR_NO_BMP:         return "no_bmp";
        case BOOST_CAL_ERR_STALE:          return "stale_reading";
        case BOOST_CAL_ERR_UNSTABLE:       return "unstable_reading";
        case BOOST_CAL_ERR_IMPLAUSIBLE:    return "implausible_pressure";
        case BOOST_CAL_ERR_OUT_OF_RANGE:   return "correction_out_of_range";
        case BOOST_CAL_ERR_PERSIST:        return "persist_failed";
        case BOOST_CAL_ERR_BUSY:           return "busy";
        default:                           return "calibration_failed";
    }
}

/* --------------------------------------------------------- calibration run */

static int64_t wall_clock_ms(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < CAL_EPOCH_VALID_SEC) {
        return 0;
    }
    return (int64_t)tv.tv_sec * 1000 + (int64_t)(tv.tv_usec / 1000);
}

/* Observe published snapshots only. Issuing I2C from here would give the bus a
 * second owner and race the reader task's transactions. */
static boost_cal_result_t calibrate_observe(boost_map_cal_t *out)
{
    const float supply = cal_supply_volts();

    int ads_fresh = 0;
    int bmp_fresh = 0;
    int used = 0;
    int bmp_increments = 0;
    bool ads_seen = false;
    bool bmp_seen = false;
    bool bmp_measured = false;
    bool have_prev_updates = false;
    bool nonfinite = false;
    uint32_t prev_updates = 0;

    double sum_volts = 0.0;
    double sum_nominal = 0.0;
    double sum_bmp = 0.0;
    float volts_min = 0.0f, volts_max = 0.0f;
    float nominal_min = 0.0f, nominal_max = 0.0f;
    float bmp_min = 0.0f, bmp_max = 0.0f;

    for (int i = 0; i < CAL_POLLS; ++i) {
        vTaskDelay(pdMS_TO_TICKS(CAL_POLL_MS));
        const boost_sample_t s = boost_sensors_get_sample();

        if (s.ads_present) {
            ads_seen = true;
        }
        if (s.bmp_present) {
            bmp_seen = true;
        }
        if (!s.ambient_is_fallback) {
            bmp_measured = true;
        }

        const bool a_ok = s.ads_present && s.ads_age_ms <= CAL_MAX_ADS_AGE_MS;
        const bool b_ok = s.bmp_present && !s.ambient_is_fallback &&
                          s.bmp_age_ms <= CAL_MAX_BMP_AGE_MS;
        if (a_ok) {
            ++ads_fresh;
        }
        if (b_ok) {
            ++bmp_fresh;
            if (have_prev_updates && s.bmp_updates != prev_updates) {
                ++bmp_increments;
            }
            prev_updates = s.bmp_updates;
            have_prev_updates = true;
        }
        if (!a_ok || !b_ok) {
            continue;
        }

        /* Always recomputed from the raw voltage. Averaging the already
         * corrected pressure would fold the previous offset into the new one
         * and make every recalibration compound. */
        const float nominal = nominal_kpa_at(s.map_volts, supply);
        if (!isfinite(s.map_volts) || !isfinite(nominal) || !isfinite(s.ambient_kpa)) {
            nonfinite = true;
            continue;
        }

        if (used == 0) {
            volts_min = volts_max = s.map_volts;
            nominal_min = nominal_max = nominal;
            bmp_min = bmp_max = s.ambient_kpa;
        } else {
            if (s.map_volts < volts_min) { volts_min = s.map_volts; }
            if (s.map_volts > volts_max) { volts_max = s.map_volts; }
            if (nominal < nominal_min)   { nominal_min = nominal; }
            if (nominal > nominal_max)   { nominal_max = nominal; }
            if (s.ambient_kpa < bmp_min) { bmp_min = s.ambient_kpa; }
            if (s.ambient_kpa > bmp_max) { bmp_max = s.ambient_kpa; }
        }
        sum_volts += (double)s.map_volts;
        sum_nominal += (double)nominal;
        sum_bmp += (double)s.ambient_kpa;
        ++used;
    }

    /* Order matters: report the most fundamental missing precondition first, so
     * the operator is told "no BMP" rather than "unstable" when the BMP is out. */
    if (!ads_seen || ads_fresh == 0) {
        return BOOST_CAL_ERR_NO_ADS;
    }
    if (!bmp_seen || !bmp_measured || bmp_fresh == 0) {
        return BOOST_CAL_ERR_NO_BMP;
    }
    if (bmp_increments < CAL_MIN_BMP_UPDATES || used == 0) {
        return BOOST_CAL_ERR_STALE;
    }
    if (nonfinite ||
        nominal_min < CAL_NOMINAL_MIN_KPA || nominal_max > CAL_NOMINAL_MAX_KPA ||
        bmp_min < CAL_BMP_MIN_KPA || bmp_max > CAL_BMP_MAX_KPA) {
        return BOOST_CAL_ERR_IMPLAUSIBLE;
    }
    if ((volts_max - volts_min) > CAL_MAX_VOLT_SPREAD ||
        (bmp_max - bmp_min) > CAL_MAX_KPA_SPREAD) {
        return BOOST_CAL_ERR_UNSTABLE;
    }

    const float mean_volts = (float)(sum_volts / (double)used);
    const float mean_nominal = (float)(sum_nominal / (double)used);
    const float mean_bmp = (float)(sum_bmp / (double)used);
    const float offset = mean_bmp - mean_nominal;
    if (!isfinite(offset) || fabsf(offset) > BOOST_MAP_CAL_MAX_KPA) {
        return BOOST_CAL_ERR_OUT_OF_RANGE;
    }

    const boost_map_cal_t rec = {
        .version = BOOST_MAP_CAL_VERSION,
        .samples = (uint16_t)used,
        .offset_kpa = offset,
        .supply_volts = supply,
        .ref_map_volts = mean_volts,
        .ref_nominal_kpa = mean_nominal,
        .ref_bmp_kpa = mean_bmp,
        .epoch_ms = wall_clock_ms(),
    };

    /* Persist first. If the write fails the live conversion must keep using the
     * previous offset, otherwise a reboot would silently revert the gauge. */
    if (!persist_settings(NULL, &rec)) {
        return BOOST_CAL_ERR_PERSIST;
    }

    if (xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        s_cal = rec;
        xSemaphoreGive(s_cal_lock);
    }
    /* A peak captured under the previous conversion is not comparable. */
    boost_sensors_reset_peak();

    ESP_LOGI(TAG,
             "atmosphere calibration: %d samples, %.4f V -> %.3f kPa nominal, "
             "BMP %.3f kPa, offset %+.3f kPa at %.2f V supply",
             used, (double)mean_volts, (double)mean_nominal, (double)mean_bmp,
             (double)offset, (double)supply);

    if (out != NULL) {
        *out = rec;
    }
    return BOOST_CAL_OK;
}

boost_cal_result_t boost_sensors_calibrate_atmosphere(boost_map_cal_t *out)
{
    if (s_lock == NULL || s_cal_lock == NULL) {
        return BOOST_CAL_ERR_NO_ADS;   /* sensors never came up at all */
    }
    if (xSemaphoreTake(s_cal_lock, portMAX_DELAY) != pdTRUE) {
        return BOOST_CAL_ERR_BUSY;
    }
    if (s_cal_busy) {
        xSemaphoreGive(s_cal_lock);
        return BOOST_CAL_ERR_BUSY;
    }
    s_cal_busy = true;
    xSemaphoreGive(s_cal_lock);

    const boost_cal_result_t result = calibrate_observe(out);

    if (xSemaphoreTake(s_cal_lock, portMAX_DELAY) == pdTRUE) {
        s_cal_busy = false;
        xSemaphoreGive(s_cal_lock);
    }
    if (result != BOOST_CAL_OK) {
        ESP_LOGW(TAG, "atmosphere calibration rejected: %s",
                 boost_sensors_cal_error_code(result));
    }
    return result;
}
