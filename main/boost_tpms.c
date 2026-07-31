#include "boost_tpms.h"

#include <math.h>
#include <string.h>

#include "boost_tpms_protocol.h"
#include "boost_tpms_mock.h"

#ifndef BOOST_TPMS_BLE_ENABLED
#define BOOST_TPMS_BLE_ENABLED 0
#endif

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
    if (now_ms < s_snapshot.updated_at_ms) return;
    const uint32_t age = now_ms - s_snapshot.updated_at_ms;
    for (unsigned i = 0; i < 4; ++i) {
        s_snapshot.wheel[i].age_ms = age;
        s_snapshot.wheel[i].valid = age <= s_config.stale_after_ms;
    }
    s_snapshot.status = (age <= s_config.stale_after_ms) ? BOOST_TPMS_STATUS_NORMAL : BOOST_TPMS_STATUS_STALE;
}

#ifndef BOOST_TPMS_MOCK_DISABLED
static uint32_t s_mock_now_ms;
#endif


static boost_tpms_snapshot_t s_snapshot;
static boost_tpms_config_t s_config = {
    .stale_after_ms = 5000,
    .replacement_offset_kpa = 0.0f,
    .enabled = true,
};

void boost_tpms_init(void)
{
    memset(&s_snapshot, 0, sizeof(s_snapshot));
    s_snapshot.status = BOOST_TPMS_STATUS_DISCONNECTED;
    for (unsigned i = 0; i < 4; ++i) {
        s_snapshot.wheel[i].did = boost_tpms_protocol_did_for_wheel((boost_tpms_protocol_wheel_t)i);
        s_snapshot.wheel[i].age_ms = UINT32_MAX;
    }
}

void boost_tpms_start(void)
{
    /* Default is intentionally UI-independent and transport-neutral. A BLE
     * central or vehicle transport may call boost_tpms_poll() later. */
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
        !isfinite(config->replacement_offset_kpa)) return false;
    s_config = *config;
    return true;
}

void boost_tpms_poll(void)
{
    /* No hardware dependency in the default service. */
}

void boost_tpms_mock_publish(uint32_t now_ms)
{
    static const uint16_t raw[4] = { 73, 74, 72, 75 };
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
