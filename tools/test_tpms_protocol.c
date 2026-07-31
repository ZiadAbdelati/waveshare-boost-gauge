#include "boost_tpms_protocol.h"

#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int approx(float actual, float expected, float tolerance)
{
    return isfinite(actual) && fabsf(actual - expected) <= tolerance;
}

int main(void)
{
    assert(boost_tpms_protocol_raw_to_kpa(0u, 0.0f) == 0.0f);
    assert(approx(boost_tpms_protocol_raw_to_kpa(165u, 0.0f), 165.0f * 1.373f, 1e-3f));
    assert(approx(boost_tpms_protocol_kpa_to_psi(226.545f),
                  226.545f * 0.145037738f, 1e-3f));
    assert(approx(boost_tpms_protocol_raw_to_psi(165u, 2.0f),
                  boost_tpms_protocol_kpa_to_psi(boost_tpms_protocol_raw_to_kpa(165u, 2.0f)),
                  1e-6f));
    assert(approx(boost_tpms_protocol_raw_to_psi(100u, -3.5f),
                  boost_tpms_protocol_kpa_to_psi(boost_tpms_protocol_raw_to_kpa(100u, -3.5f)),
                  1e-6f));
    assert(approx(boost_tpms_protocol_raw_to_kpa(100u, 10.0f), 100.0f * 1.373f + 10.0f,
                  1e-3f));
    assert(!isfinite(boost_tpms_protocol_raw_to_kpa(100u, INFINITY)));
    assert(isnan(boost_tpms_protocol_raw_to_kpa(100u, NAN)));

    const boost_tpms_protocol_wheel_t wheels[] = {
        BOOST_TPMS_WHEEL_FL, BOOST_TPMS_WHEEL_FR,
        BOOST_TPMS_WHEEL_RL, BOOST_TPMS_WHEEL_RR,
    };
    const uint16_t dids[] = {0x2A05u, 0x2A07u, 0x2A06u, 0x2A08u};
    for (size_t i = 0; i < 4u; ++i) {
        assert(boost_tpms_protocol_wheel_for_did(dids[i]) == wheels[i]);
        assert(boost_tpms_protocol_did_for_wheel(wheels[i]) == dids[i]);
    }
    assert(boost_tpms_protocol_wheel_for_did(0x1234u) == BOOST_TPMS_WHEEL_INVALID);

    uint8_t request[3] = {0u, 0u, 0u};
    assert(boost_tpms_protocol_make_read_did(0x2A05u, request));
    assert(request[0] == 0x22u && request[1] == 0x2Au && request[2] == 0x05u);

    const uint8_t valid[] = {0x62u, 0x2Au, 0x05u, 0x00u, 0xA5u};
    uint16_t out_did = 0u;
    uint16_t out_raw = 0u;
    assert(boost_tpms_protocol_parse_uds_response(valid, sizeof(valid), &out_did, &out_raw));
    assert(out_did == 0x2A05u && out_raw == 0x00A5u);
    const uint8_t wrong_sid[] = {0x7Fu, 0x2Au, 0x05u, 0x00u, 0xA5u};
    const uint8_t wrong_len[] = {0x62u, 0x2Au, 0x05u, 0x00u};
    const uint8_t wrong_did[] = {0x62u, 0x2Au, 0x09u, 0x00u, 0xA5u};
    assert(!boost_tpms_protocol_parse_uds_response(wrong_sid, sizeof(wrong_sid), NULL, NULL));
    assert(!boost_tpms_protocol_parse_uds_response(wrong_len, sizeof(wrong_len), NULL, NULL));
    assert(!boost_tpms_protocol_parse_uds_response(wrong_did, sizeof(wrong_did), NULL, NULL));

    boost_tpms_isotp_parser_t parser;
    boost_tpms_isotp_init(&parser);
    const uint8_t frame[8] = {0x05u, 0x62u, 0x2Au, 0x05u, 0x00u, 0xA5u, 0x00u, 0x00u};
    const uint8_t *payload = NULL;
    size_t payload_length = 0u;
    assert(boost_tpms_isotp_feed(&parser, frame, sizeof(frame), &payload, &payload_length) ==
           BOOST_TPMS_ISOTP_COMPLETE);
    assert(payload != NULL && payload_length == 5u);
    assert(memcmp(payload, &frame[1], 5u) == 0);

    puts("tpms protocol tests: PASS");
    return 0;
}
