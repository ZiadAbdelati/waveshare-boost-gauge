#include "boost_tpms_protocol.h"

#include <math.h>
#include <string.h>

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

float boost_tpms_protocol_raw_to_kpa(uint16_t raw, float replacement_offset_kpa)
{
    if (!isfinite(replacement_offset_kpa)) {
        return NAN;
    }
    return (float)raw * BOOST_TPMS_RAW_TO_KPA + replacement_offset_kpa;
}

float boost_tpms_protocol_kpa_to_psi(float kpa)
{
    return isfinite(kpa) ? kpa * BOOST_TPMS_KPA_TO_PSI : NAN;
}

float boost_tpms_protocol_raw_to_psi(uint16_t raw, float replacement_offset_kpa)
{
    return boost_tpms_protocol_kpa_to_psi(
        boost_tpms_protocol_raw_to_kpa(raw, replacement_offset_kpa));
}

boost_tpms_protocol_wheel_t boost_tpms_protocol_wheel_for_did(uint16_t did)
{
    switch (did) {
        case BOOST_TPMS_DID_FL: return BOOST_TPMS_WHEEL_FL;
        case BOOST_TPMS_DID_FR: return BOOST_TPMS_WHEEL_FR;
        case BOOST_TPMS_DID_RL: return BOOST_TPMS_WHEEL_RL;
        case BOOST_TPMS_DID_RR: return BOOST_TPMS_WHEEL_RR;
        default: return BOOST_TPMS_WHEEL_INVALID;
    }
}

uint16_t boost_tpms_protocol_did_for_wheel(boost_tpms_protocol_wheel_t wheel)
{
    switch (wheel) {
        case BOOST_TPMS_WHEEL_FL: return BOOST_TPMS_DID_FL;
        case BOOST_TPMS_WHEEL_FR: return BOOST_TPMS_DID_FR;
        case BOOST_TPMS_WHEEL_RL: return BOOST_TPMS_DID_RL;
        case BOOST_TPMS_WHEEL_RR: return BOOST_TPMS_DID_RR;
        default: return 0;
    }
}

bool boost_tpms_protocol_make_read_did(uint16_t did, uint8_t out_request[3])
{
    if (out_request == NULL || boost_tpms_protocol_wheel_for_did(did) == BOOST_TPMS_WHEEL_INVALID) {
        return false;
    }
    out_request[0] = 0x22;
    out_request[1] = (uint8_t)(did >> 8);
    out_request[2] = (uint8_t)did;
    return true;
}

bool boost_tpms_protocol_parse_uds_response(const uint8_t *payload, size_t length,
                                            uint16_t *out_did, uint16_t *out_raw)
{
    if (payload == NULL || (length != 4 && length != 5) || payload[0] != 0x62 ||
        boost_tpms_protocol_wheel_for_did(read_be16(&payload[1])) == BOOST_TPMS_WHEEL_INVALID) {
        return false;
    }
    if (out_did != NULL) *out_did = read_be16(&payload[1]);
    /* The MX-5 ND TPMS module answers these DIDs with a single data byte
     * (0x62 DID-hi DID-lo value), i.e. a 4-byte response. Accept a two-byte
     * value as well in case another module pads it. */
    if (out_raw != NULL) {
        *out_raw = (length == 5) ? read_be16(&payload[3]) : (uint16_t)payload[3];
    }
    return true;
}
