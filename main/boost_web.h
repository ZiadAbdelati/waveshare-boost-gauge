#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t boost_web_start(void);

/* Tell the live-telemetry WebSocket task that a new sample has been published,
 * so it can push immediately instead of waiting on a free-running timer. Safe
 * to call before boost_web_start(); it is a no-op until the task exists. */
void boost_web_notify_sample(void);

#ifdef __cplusplus
}
#endif
