#include "boost_sensors.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "boost_sensors";

/* ---------------------------------------------------------------------------
 * Bus + address map
 * ---------------------------------------------------------------------------
 * Our sensors live on a bus that is deliberately separate from the BSP's
 * touch/IO-expander bus (BSP_I2C_SCL=GPIO14, BSP_I2C_SDA=GPIO15, port 1). We
 * take I2C port 0 on GPIO18/17, which the BSP pin map never touches, so there
 * is no double-init of a bus the BSP already owns and no pin collision.
 */
#define SENS_I2C_PORT       I2C_NUM_0
#define SENS_SCL_GPIO       18
#define SENS_SDA_GPIO       17
#define SENS_I2C_HZ         400000
#define SENS_IO_TIMEOUT_MS  50

#define ADS1115_ADDR        0x48   /* ADDR pin -> GND */
#define BMP280_ADDR         0x76   /* SDO pin  -> GND */

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
 * PGA note: the GM 3-bar sensor is ratiometric to a 5 V supply and swings up to
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
 * GM 3-bar MAP transfer function  (absolute pressure)
 * ---------------------------------------------------------------------------
 * kPa_abs = (volts - MAP_VOLT_OFFSET) / MAP_VOLTS_PER_KPA
 *
 * Default constants are the common GM 3-bar approximation the user supplied
 * (also seen in MegaSquirt / DIYAutoTune GM-3bar references). BOTH are named
 * and meant to be calibrated on the bench:
 *   - MAP_VOLT_OFFSET   : sensor volts at 0 kPa absolute (sets the zero)
 *   - MAP_VOLTS_PER_KPA : slope in volts per kPa (sets the span)
 * Calibrate: with the engine off, MAP == ambient, so read map_volts and adjust
 * MAP_VOLT_OFFSET until map_abs_kpa matches the BMP280 ambient (~101 kPa). Then
 * apply a known second pressure (e.g. a hand vacuum/pressure pump) to trim the
 * slope. Exposed raw over /api/v1/state precisely so this can be done.
 */
#define MAP_VOLT_OFFSET     0.6f      /* V at 0 kPa absolute */
#define MAP_VOLTS_PER_KPA   0.0059f   /* V per kPa */

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
 * gauge is still usable (degraded), rather than reading nonsense. */
#define STANDARD_ATM_KPA     101.325f

#define KPA_TO_PSI           0.145037738f

/* Reader cadence. ADS every loop; BMP once every N loops (ambient drifts
 * slowly, and forced/normal-mode BMP reads are the slow ones we keep off the
 * fast path). 20 ms loop -> 50 Hz MAP, 5 Hz ambient. */
#define SENS_LOOP_MS         20
#define BMP_EVERY_N          10

/* --- BMP280 factory calibration (read from NVM at init) --- */
static uint16_t s_dig_t1;
static int16_t  s_dig_t2, s_dig_t3;
static uint16_t s_dig_p1;
static int16_t  s_dig_p2, s_dig_p3, s_dig_p4, s_dig_p5, s_dig_p6, s_dig_p7, s_dig_p8, s_dig_p9;
static int32_t  s_t_fine;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_ads;
static i2c_master_dev_handle_t s_bmp;
static bool s_ads_present;
static bool s_bmp_present;

static SemaphoreHandle_t s_lock;
static boost_sample_t s_latest;   /* guarded by s_lock */
static float s_peak;              /* guarded by s_lock */

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
    float last_map_kpa = 0.0f;
    float ambient_kpa = s_bmp_present ? 0.0f : STANDARD_ATM_KPA;
    bool have_map = false;
    bool have_ambient = s_bmp_present ? false : true;
    uint32_t loop = 0;

    TickType_t next = xTaskGetTickCount();
    while (true) {
        bool fault = false;

        /* MAP: fast path, every loop. */
        if (s_ads_present) {
            float v;
            if (ads_read_volts(&v)) {
                last_volts = v;
                last_map_kpa = (v - MAP_VOLT_OFFSET) / MAP_VOLTS_PER_KPA;
                have_map = true;
            } else {
                fault = true;   /* hold last good */
            }
        } else {
            fault = true;       /* no MAP source at all */
        }

        /* Ambient: slow path, every BMP_EVERY_N loops. */
        if (s_bmp_present && (loop % BMP_EVERY_N) == 0) {
            float k;
            if (bmp_read_kpa(&k)) {
                ambient_kpa = k;
                have_ambient = true;
            } else {
                /* Not a hard fault: a stale-but-recent ambient is fine, and if
                 * we have never had one, fall back to standard atmosphere. */
                if (!have_ambient) {
                    ambient_kpa = STANDARD_ATM_KPA;
                }
            }
        }

        const float map_kpa = have_map ? last_map_kpa : 0.0f;
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

        boost_sample_t s = {
            .psi = psi,
            .peak_psi = peak,
            .demo = false,
            .map_volts = last_volts,
            .map_abs_kpa = map_kpa,
            .ambient_kpa = ambient_kpa,
            .ads_present = s_ads_present,
            .bmp_present = s_bmp_present,
            .sensor_fault = fault,
        };
        publish(&s, peak);

        ++loop;
        vTaskDelayUntil(&next, pdMS_TO_TICKS(SENS_LOOP_MS));
    }
}

/* ---------------------------------------------------------------------- init */

bool boost_sensors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return false;
    }
    /* A sane zero snapshot until the first read lands. */
    s_latest = (boost_sample_t){ .demo = false };

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

    /* --- ADS1115 --- */
    const i2c_device_config_t ads_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS1115_ADDR,
        .scl_speed_hz = SENS_I2C_HZ,
    };
    if (i2c_master_probe(s_bus, ADS1115_ADDR, SENS_IO_TIMEOUT_MS) == ESP_OK &&
        i2c_master_bus_add_device(s_bus, &ads_cfg, &s_ads) == ESP_OK &&
        ads_configure()) {
        s_ads_present = true;
        ESP_LOGI(TAG, "ADS1115 detected at 0x%02X (MAP on A0, +/-%.3f V FSR, continuous)",
                 ADS1115_ADDR, (double)ADS_FSR_VOLTS);
    } else {
        s_ads_present = false;
        ESP_LOGW(TAG, "ADS1115 NOT detected at 0x%02X - MAP unavailable, gauge will fault",
                 ADS1115_ADDR);
    }

    /* --- BMP280 --- */
    const i2c_device_config_t bmp_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP280_ADDR,
        .scl_speed_hz = SENS_I2C_HZ,
    };
    if (i2c_master_probe(s_bus, BMP280_ADDR, SENS_IO_TIMEOUT_MS) == ESP_OK &&
        i2c_master_bus_add_device(s_bus, &bmp_cfg, &s_bmp) == ESP_OK) {
        uint8_t id = 0;
        const bool id_ok = reg_read(s_bmp, BMP280_REG_ID, &id, 1) == ESP_OK;
        if (id_ok && id == BMP280_CHIP_ID && bmp_load_calibration() &&
            reg_write8(s_bmp, BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS) == ESP_OK &&
            reg_write8(s_bmp, BMP280_REG_CONFIG, BMP280_CONFIG) == ESP_OK) {
            s_bmp_present = true;
            ESP_LOGI(TAG, "BMP280 detected at 0x%02X (chip id 0x%02X, normal mode, calib loaded)",
                     BMP280_ADDR, id);
        } else {
            s_bmp_present = false;
            ESP_LOGW(TAG, "BMP280 at 0x%02X did not init (id 0x%02X) - using %.1f kPa ambient",
                     BMP280_ADDR, id, (double)STANDARD_ATM_KPA);
        }
    } else {
        s_bmp_present = false;
        ESP_LOGW(TAG, "BMP280 NOT detected at 0x%02X - using %.1f kPa standard atmosphere",
                 BMP280_ADDR, (double)STANDARD_ATM_KPA);
    }

    const BaseType_t ok = xTaskCreatePinnedToCore(
        sensors_task, "boost_sensors", 4096, NULL, 5, NULL, 0);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "sensor task create failed");
        return false;
    }

    ESP_LOGI(TAG, "sensor path ready: ADS1115 %s, BMP280 %s",
             s_ads_present ? "present" : "absent",
             s_bmp_present ? "present" : "absent");
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
