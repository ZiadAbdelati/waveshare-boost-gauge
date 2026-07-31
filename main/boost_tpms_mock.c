#include "boost_tpms_mock.h"

#include <string.h>

#include "boost_tpms_protocol.h"

static boost_tpms_mock_scenario_t s_scenario = BOOST_TPMS_MOCK_NORMAL;
static uint32_t s_elapsed;

void boost_tpms_mock_set_scenario(boost_tpms_mock_scenario_t scenario)
{
    s_scenario = scenario;
    s_elapsed = 0;
}

boost_tpms_mock_scenario_t boost_tpms_mock_get_scenario(void)
{
    return s_scenario;
}

void boost_tpms_mock_tick(uint32_t elapsed_ms)
{
    s_elapsed += elapsed_ms;
    boost_tpms_snapshot_t snapshot;
    boost_tpms_get_snapshot(&snapshot);
    snapshot.updated_at_ms += elapsed_ms;
    if (s_scenario == BOOST_TPMS_MOCK_DISCONNECTED) {
        snapshot.status = BOOST_TPMS_STATUS_DISCONNECTED;
        for (size_t i = 0; i < 4; ++i) {
            snapshot.wheel[i].valid = false;
            snapshot.wheel[i].age_ms = UINT32_MAX;
        }
    } else if (s_scenario == BOOST_TPMS_MOCK_STALE) {
        snapshot.status = BOOST_TPMS_STATUS_STALE;
        for (size_t i = 0; i < 4; ++i) {
            snapshot.wheel[i].age_ms += elapsed_ms;
        }
    } else {
        snapshot.status = BOOST_TPMS_STATUS_NORMAL;
        for (size_t i = 0; i < 4; ++i) {
            snapshot.wheel[i].age_ms = 0;
        }
    }
    (void)s_elapsed;
    /* Mock is intentionally a deterministic input provider; the service owns
     * publication in the default implementation, so this hook is a no-op
     * unless a host test supplies its own service snapshot. */
}
