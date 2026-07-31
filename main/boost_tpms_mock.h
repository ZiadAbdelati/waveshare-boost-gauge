#pragma once

#include "boost_tpms.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BOOST_TPMS_MOCK_NORMAL = 0,
    BOOST_TPMS_MOCK_STALE,
    BOOST_TPMS_MOCK_DISCONNECTED,
} boost_tpms_mock_scenario_t;

void boost_tpms_mock_set_scenario(boost_tpms_mock_scenario_t scenario);
boost_tpms_mock_scenario_t boost_tpms_mock_get_scenario(void);
void boost_tpms_mock_tick(uint32_t elapsed_ms);
/* Test/provider hook: publish deterministic raw wheel values through the same
 * conversion and snapshot path as a transport-backed implementation. */
void boost_tpms_mock_publish(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
