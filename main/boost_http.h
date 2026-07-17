#pragma once

#include "esp_err.h"
#include "boost_sim.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start SoftAP-facing HTTP control server (REST + SSE + static UI). */
esp_err_t boost_http_start(void);

/** Update live sample for /api/status and SSE (call from sensor task). */
void boost_http_set_sample(const boost_sample_t *s);

#ifdef __cplusplus
}
#endif
