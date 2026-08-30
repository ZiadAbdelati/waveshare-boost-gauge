#include "boost_tpms_mock.h"

#include "boost_tpms_protocol.h"

static boost_tpms_mock_scenario_t s_scenario = BOOST_TPMS_MOCK_NORMAL;
static bool s_published;

void boost_tpms_mock_set_scenario(boost_tpms_mock_scenario_t scenario)
{
    s_scenario = scenario;
    s_published = false;
}

void boost_tpms_mock_tick(uint32_t now_ms)
{
    switch (s_scenario) {
    case BOOST_TPMS_MOCK_NORMAL: {
        /* Deterministic slow wobble around the placard band so the readouts
         * visibly live: raw ~164-166 -> ~225-228 kPa -> ~32.6-33.1 psi. */
        static const uint16_t base[4] = { 165, 167, 163, 168 };
        uint16_t raw[4];
        const uint32_t phase = now_ms / 1000u;
        for (unsigned i = 0; i < 4; ++i) {
            raw[i] = (uint16_t)(base[i] + (int)((phase + i) % 3u) - 1);
        }
        boost_tpms_publish_raw(now_ms, raw);
        break;
    }
    case BOOST_TPMS_MOCK_STALE:
        /* Publish once, then stop: aging flips the service STALE and the
         * wheels invalid after the staleness window. */
        if (!s_published) {
            boost_tpms_mock_publish(now_ms);
            s_published = true;
        }
        break;
    case BOOST_TPMS_MOCK_DISCONNECTED:
        /* Never publish; aging keeps the service DISCONNECTED. */
        break;
    }
    boost_tpms_age(now_ms);
}
