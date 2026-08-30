#include "boost_tpms.h"

#include <math.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "nvs.h"
#endif

#include "boost_tpms_protocol.h"
#include "boost_tpms_mock.h"

#ifndef BOOST_TPMS_BLE_ENABLED
#define BOOST_TPMS_BLE_ENABLED 0
#endif

#define TPMS_NVS_NS     "boost_tpms"
#define TPMS_NVS_LOW    "low_kpa"    /* u16, kPa */
#define TPMS_NVS_STALE  "stale_ms"   /* u32, ms  */

static boost_tpms_snapshot_t s_snapshot;
static boost_tpms_config_t s_config = {
    .stale_after_ms = BOOST_TPMS_STALE_AFTER_MS,
    .replacement_offset_kpa = 0.0f,
    .low_kpa = BOOST_TPMS_LOW_PRESSURE_KPA,
    .enabled = true,
};

static void publish_raw(uint32_t now_ms, const uint16_t raw[4])
{
    s_snapshot.sequence++;
    s_snapshot.updated_at_ms = now_ms;
    s_snapshot.status = BOOST_TPMS_STATUS_NORMAL;
    for (unsigned i = 0; i < 4; ++i) {
        s_snapshot.wheel[i].raw = raw[i];
        s_snapshot.wheel[i].did = boost_tpms_protocol_did_for_wheel((boost_tpms_protocol_wheel_t)i);
        s_snapshot.wheel[i].kpa = boost_tpms_protocol_raw_to_kpa(raw[i], s_config.replacement_offset_kpa);
        s_snapshot.wheel[i].psi = boost_tpms_protocol_kpa_to_psi(s_snapshot.wheel[i].kpa);
        s_snapshot.wheel[i].age_ms = 0;
        s_snapshot.wheel[i].valid = true;
    }
}

void boost_tpms_publish_raw(uint32_t now_ms, const uint16_t raw[4])
{
    if (raw != NULL) publish_raw(now_ms, raw);
}

void boost_tpms_age(uint32_t now_ms)
{
    if (s_snapshot.sequence == 0) {
        /* Never received a sample: the link is down, not merely late. */
        s_snapshot.status = BOOST_TPMS_STATUS_DISCONNECTED;
        return;
    }
    if (now_ms < s_snapshot.updated_at_ms) return;
    const uint32_t age = now_ms - s_snapshot.updated_at_ms;
    for (unsigned i = 0; i < 4; ++i) {
        s_snapshot.wheel[i].age_ms = age;
        s_snapshot.wheel[i].valid = age <= s_config.stale_after_ms;
    }
    s_snapshot.status = (age <= s_config.stale_after_ms) ? BOOST_TPMS_STATUS_NORMAL : BOOST_TPMS_STATUS_STALE;
}

void boost_tpms_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.status = BOOST_TPMS_STATUS_DISCONNECTED;
    for (unsigned i = 0; i < 4; ++i) {
        s_snapshot.wheel[i].did = boost_tpms_protocol_did_for_wheel((boost_tpms_protocol_wheel_t)i);
        s_snapshot.wheel[i].age_ms = UINT32_MAX;
    }
    /* Own-namespace NVS restore (nvs_flash_init already ran in boost_model_init,
     * and is idempotent anyway). Absent keys keep the compiled defaults. */
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(TPMS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint16_t low = 0;
        uint32_t stale = 0;
        if (nvs_get_u16(h, TPMS_NVS_LOW, &low) == ESP_OK && low >= 100 && low <= 400) {
            s_config.low_kpa = (float)low;
        }
        if (nvs_get_u32(h, TPMS_NVS_STALE, &stale) == ESP_OK && stale >= 2000 && stale <= 120000) {
            s_config.stale_after_ms = stale;
        }
        nvs_close(h);
    }
#endif
}

void boost_tpms_start(void)
{
    /* Default is intentionally UI-independent and transport-neutral. */
}

void boost_tpms_get_snapshot(boost_tpms_snapshot_t *out)
{
    if (out != NULL) *out = s_snapshot;
}

void boost_tpms_get_config(boost_tpms_config_t *out)
{
    if (out != NULL) *out = s_config;
}

bool boost_tpms_set_config(const boost_tpms_config_t *config)
{
    if (config == NULL || config->stale_after_ms == 0 ||
        !isfinite(config->replacement_offset_kpa) ||
        !isfinite(config->low_kpa) ||
        config->low_kpa < 100.0f || config->low_kpa > 400.0f) return false;
    s_config = *config;
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open(TPMS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, TPMS_NVS_LOW, (uint16_t)(s_config.low_kpa + 0.5f));
        nvs_set_u32(h, TPMS_NVS_STALE, s_config.stale_after_ms);
        nvs_commit(h);
        nvs_close(h);
    }
#endif
    return true;
}

void boost_tpms_mock_publish(uint32_t now_ms)
{
    /* Deterministic raw values near the ND placard band (~33 psi). */
    static const uint16_t raw[4] = { 165, 167, 163, 168 };
    publish_raw(now_ms, raw);
}
