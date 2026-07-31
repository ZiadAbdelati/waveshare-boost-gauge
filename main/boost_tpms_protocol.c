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
    if (payload == NULL || length != 5 || payload[0] != 0x62 ||
        boost_tpms_protocol_wheel_for_did(read_be16(&payload[1])) == BOOST_TPMS_WHEEL_INVALID) {
        return false;
    }
    if (out_did != NULL) *out_did = read_be16(&payload[1]);
    if (out_raw != NULL) *out_raw = read_be16(&payload[3]);
    return true;
}

void boost_tpms_isotp_init(boost_tpms_isotp_parser_t *parser)
{
    if (parser != NULL) memset(parser, 0, sizeof(*parser));
}

boost_tpms_isotp_result_t boost_tpms_isotp_feed(boost_tpms_isotp_parser_t *parser,
                                                 const uint8_t *frame, size_t length,
                                                 const uint8_t **out_payload,
                                                 size_t *out_length)
{
    if (out_payload != NULL) *out_payload = NULL;
    if (out_length != NULL) *out_length = 0;
    if (parser == NULL || frame == NULL || length == 0 || length > 8) return BOOST_TPMS_ISOTP_ERROR;

    const uint8_t pci = frame[0] >> 4;
    if (pci == 0) {
        const size_t n = frame[0] & 0x0f;
        if (n == 0 || n > length - 1 || n > BOOST_TPMS_ISOTP_MAX_PAYLOAD) return BOOST_TPMS_ISOTP_ERROR;
        memcpy(parser->payload, &frame[1], n);
        parser->expected_length = n;
        parser->received_length = n;
        parser->active = false;
        if (out_payload != NULL) *out_payload = parser->payload;
        if (out_length != NULL) *out_length = n;
        return BOOST_TPMS_ISOTP_COMPLETE;
    }
    if (pci == 1) {
        if (length < 2) return BOOST_TPMS_ISOTP_ERROR;
        const size_t n = ((size_t)(frame[0] & 0x0f) << 8) | frame[1];
        if (n == 0 || n > BOOST_TPMS_ISOTP_MAX_PAYLOAD || length < 3) return BOOST_TPMS_ISOTP_ERROR;
        parser->expected_length = n;
        parser->received_length = length - 2;
        if (parser->received_length > n) return BOOST_TPMS_ISOTP_ERROR;
        memcpy(parser->payload, &frame[2], parser->received_length);
        parser->next_sequence = 1;
        parser->active = parser->received_length < n;
        if (!parser->active) {
            if (out_payload != NULL) *out_payload = parser->payload;
            if (out_length != NULL) *out_length = n;
            return BOOST_TPMS_ISOTP_COMPLETE;
        }
        return BOOST_TPMS_ISOTP_INCOMPLETE;
    }
    if (pci == 2 && parser->active) {
        if ((frame[0] & 0x0f) != parser->next_sequence || length < 2) return BOOST_TPMS_ISOTP_ERROR;
        const size_t remaining = parser->expected_length - parser->received_length;
        const size_t n = (length - 1 < remaining) ? length - 1 : remaining;
        memcpy(&parser->payload[parser->received_length], &frame[1], n);
        parser->received_length += n;
        parser->next_sequence = (uint8_t)((parser->next_sequence + 1) & 0x0f);
        if (parser->received_length == parser->expected_length) {
            parser->active = false;
            if (out_payload != NULL) *out_payload = parser->payload;
            if (out_length != NULL) *out_length = parser->expected_length;
            return BOOST_TPMS_ISOTP_COMPLETE;
        }
        return BOOST_TPMS_ISOTP_INCOMPLETE;
    }
    return BOOST_TPMS_ISOTP_ERROR;
}
