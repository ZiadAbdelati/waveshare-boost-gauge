#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "boost_display.h"

#include "boost_brightness.h"
#include "boost_gauge.h"
#include "boost_page.h"
#include "boost_model.h"
#include "boost_theme.h"
#include "boost_sim.h"
#include "boost_sensors.h"
#include "boost_tpms.h"
#include "boost_tpms_mock.h"
#include "boost_app_ble.h"
#include "boost_obd.h"
#include "boost_obd_ble.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "boost_web.h"

static const char *TAG = "boost_main";

/*
 * The one place the demo/real source is chosen. Demo mode (persisted, default
 * off) runs the synthetic sweep; otherwise we hand back the latest real-sensor
 * snapshot, which the sensor task computes on its own cadence so neither the
 * LVGL timer nor the web task ever blocks on an I2C read.
 */
static boost_sample_t next_sample(void)
{
    if (boost_theme_demo_mode()) {
        return boost_sim_tick();
    }
    return boost_sensors_get_sample();
}

/* LVGL timer owns the physical gauge, preserving its 16 ms render cadence. */
static void gauge_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    const boost_sample_t sample = next_sample();
    boost_page_update(&sample);
    lv_timer_set_period(timer, 16U);
}

/* TPMS runs on its own slow cadence: tire pressures change in seconds, not
 * milliseconds, so a 250 ms tick keeps the page fresh without sharing the
 * 16 ms gauge path. With the OBD2 BLE link enabled the OBD driver publishes
 * real vehicle data and the mock provider stays idle; otherwise the mock
 * stands in so the page stays lively on the bench. */
static void tpms_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    /* The mock stands in ONLY while demo mode drives the gauge: on the bench
     * the page stays lively without hardware, but a real vehicle (demo off)
     * with no adapter must show no data, never simulated pressures. */
    if (boost_theme_demo_mode() && !boost_theme_tpms_ble()) {
        boost_tpms_mock_tick(lv_tick_get());
    } else {
        boost_tpms_age(lv_tick_get());
    }
    boost_tpms_snapshot_t snapshot;
    boost_tpms_get_snapshot(&snapshot);
    boost_page_update_tpms(&snapshot);
    float psi[4];
    bool valid[4];
    for (int i = 0; i < 4; ++i) {
        psi[i] = snapshot.wheel[i].psi;
        valid[i] = snapshot.wheel[i].valid;
    }
    boost_model_publish_tpms(psi, valid, (int)snapshot.status);
}

/* Web/model publication stays independent when a GIF occupies the LVGL worker. */
static void sample_task(void *arg)
{
    (void)arg;
    TickType_t next_wake = xTaskGetTickCount();
    while (true) {
        const boost_sample_t sample = next_sample();
        boost_model_publish_sample(&sample);
        /* Kick the WebSocket push task straight away: the sample is the event,
         * so remote clients no longer wait out a free-running 50 ms timer. */
        boost_web_notify_sample();

        boost_display_metrics_t metrics;
        boost_display_get_metrics(&metrics);
        boost_model_set_display_metrics(&metrics);
        boost_model_refresh_status();
        boost_model_apply_schedule();

        vTaskDelayUntil(&next_wake, pdMS_TO_TICKS(16));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Boost gauge starting");

    /* Before boost_model_init(): the model resolves the active theme by id and
     * must see any persisted colour overrides. Mounts NVS itself, so this does
     * not depend on running after boost_model_init(). */
    boost_theme_init();
    ESP_ERROR_CHECK(boost_model_init());

    /* Real-sensor path: I2C on GPIO18/17 (separate from the BSP touch bus on
     * GPIO14/15). Brings up the bus, probes ADS1115 (0x48), BMP280 (0x76) and
     * the DS3231 RTC (0x68), and starts the reader task. Runs regardless of the
     * demo flag so a runtime flip to real mode has data waiting; init logs which
     * sensors were seen. Deliberately before boot brightness is decided and the
     * panel is started: a valid DS3231 seeds the wall clock, so a night boot
     * comes up dim from the first frame instead of bright until a browser sync. */
    if (!boost_sensors_init()) {
        ESP_LOGW(TAG, "no MAP/ambient sensors detected; real mode will fault until wired");
    }
    if (boost_model_seed_clock_from_rtc() != ESP_OK) {
        ESP_LOGW(TAG, "no valid DS3231 time; clock falls back to NVS/sync");
    }

    boost_config_t cfg;
    boost_model_get_config(&cfg);

    const int boot_pct = boost_model_boot_brightness();
    /* Boot the panel fully dark (0%) and ramp to the schedule brightness only
     * after the first gauge frame is on screen, so the uninitialized white GRAM
     * never flashes - not even at the dim night level. */
    lv_display_t *disp = boost_display_start(0);
    if (disp == NULL) {
        ESP_LOGE(TAG, "boost_display_start failed");
        return;
    }

    /* Apply the persisted tearing-effect preference now that both the theme
     * store and the display exist. Default is off. */
    boost_display_set_te(boost_theme_te_sync());
    boost_display_set_region_dbuf(boost_theme_region_dbuf());
    boost_display_set_te_scanline(boost_theme_te_scanline());

    /* Start from persisted config. Long-press toggles between the configured
     * high/low pair - the same brightnessLow the dim schedule uses - rather
     * than the compile-time fallbacks. Brightness stays 0% until the first
     * frame renders below, then ramps to boot_pct. */
    boost_brightness_set_levels(cfg.brightness_high, cfg.brightness_low);

    boost_sim_init();

    /* TPMS service: mock provider until the BLE adapter profile is verified. */
    boost_tpms_init();
    boost_tpms_start();
    boost_tpms_mock_set_scenario(BOOST_TPMS_MOCK_NORMAL);

    ESP_LOGI(TAG, "sample source at boot: %s", boost_theme_demo_mode() ? "DEMO (sim)" : "real sensors");

    /* The physical gauge receives samples directly in its LVGL timer. */

    if (boost_display_lock(-1) == ESP_OK) {
        boost_page_create();

        const boost_sample_t initial = next_sample();
        boost_page_update(&initial);
        lv_timer_t *gauge_timer = lv_timer_create(gauge_timer_cb, 16, NULL);
        if (gauge_timer == NULL) {
            boost_display_unlock();
            ESP_LOGE(TAG, "failed to create gauge timer");
            return;
        }
        lv_timer_ready(gauge_timer);
        lv_timer_create(tpms_timer_cb, 250, NULL);
        boost_display_unlock();
        ESP_LOGI(TAG, "main stack minimum free after scene build: %u B",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    } else {
        ESP_LOGE(TAG, "display lock failed during UI create");
        return;
    }
    /* The first frame is on screen now (dark gauge over a dark panel): ramp to
     * the schedule brightness. The short delay lets the LVGL worker finish the
     * first render so the white boot GRAM is never shown at any brightness. */
    vTaskDelay(pdMS_TO_TICKS(100));
    boost_brightness_init(boot_pct);
    const BaseType_t sample_task_ok = xTaskCreatePinnedToCore(
        sample_task, "boost_sample", 4096, NULL, 4, NULL, 0);
    if (sample_task_ok != pdPASS) {
        ESP_LOGE(TAG, "failed to start sample task");
    }

    esp_err_t web_err = boost_web_start();
    if (web_err != ESP_OK) {
        ESP_LOGE(TAG, "web control plane failed: %s", esp_err_to_name(web_err));
    }

    /* BLE radio: the OBD2 BLE central and the companion-app GATT peripheral
     * share one NimBLE host. Brought up AFTER the web control plane so a BLE
     * init failure can never precede OTA recovery (the RAM boot-loop class).
     *
     * Ordering is load-bearing (hardware-verified 2026-08-23: a boot-looping
     * LoadProhibited in ble_hs_lock when the host APIs ran unmounted):
     *   1. boost_obd_ble_init() MOUNTS the host (nimble_port_init; RAM-guarded,
     *      idempotent) whenever EITHER persisted toggle may need the radio.
     *   2. boost_app_ble_init() reads its persisted toggle and registers the
     *      companion GATT service — legal only while the host is mounted but
     *      NOT started. Skipped safely when the mount was refused.
     *   3. boost_obd_ble_host_start() starts the host task; ble_gatts_start()
     *      then runs from the host sync callback.
     * appBle gates ADVERTISING, not registration, so flipping it at runtime
     * never needs the host to restart. If the host was never mounted at boot
     * (both toggles off), boost_app_ble_start() performs mount -> register ->
     * start itself on demand. */
    boost_obd_init();
    const bool obd_ble = boost_theme_tpms_ble();
    if (obd_ble) {
        boost_obd_ble_init();
    }
    boost_app_ble_init();
    boost_obd_ble_host_start();
    boost_obd_set_enabled(obd_ble);
    if (boost_app_ble_enabled()) {
        boost_app_ble_start();
    } else {
        boost_app_ble_stop();
    }

    /* OTA rollback gate. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE, a freshly
     * OTA'd image boots in PENDING_VERIFY and must confirm itself healthy or the
     * bootloader reverts to the previous slot on the next reset. "Healthy" here
     * means the control plane came up, i.e. the device can be reached for the
     * *next* OTA - which is exactly the property a rolled-back boot loop lacks.
     * The board runs from 5 V with no serial, so this auto-revert is the safety
     * net that makes a bad flash recoverable without a cable. */
    if (web_err == ESP_OK) {
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_ota_img_states_t state;
        if (running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI(TAG, "OTA image confirmed healthy; rollback cancelled");
            } else {
                ESP_LOGW(TAG, "failed to mark OTA image valid");
            }
        }
    } else {
        ESP_LOGW(TAG, "control plane down; leaving OTA image unconfirmed for rollback");
    }

    ESP_LOGI(TAG, "tap=reset peak · hold 1s=brightness toggle · AP password boost1234");
}
