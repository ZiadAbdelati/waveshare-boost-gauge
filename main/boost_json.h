#pragma once

#include <stddef.h>
#include "boost_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Single owner of every JSON shape the HTTP API (boost_web.c) and the BLE
 * Control route (boost_app_ble.c) serve. AGENTS.md requires the two
 * transports to answer byte-for-byte identically; sharing the builders makes
 * that parity structural instead of a copy to keep in sync. */

/* Escape a string into `out` as JSON string contents (quotes/backslashes
 * escaped, control bytes dropped). */
void boost_json_escape(const char *in, char *out, size_t out_len);

/* Upper bound on one boost_json_theme_item() object, incl. the comma. */
#define BOOST_JSON_THEME_MAX 385

/* Each returns the snprintf-style written length; callers check
 * n > 0 && n < capacity to catch truncation. */
int boost_json_state(char *json, size_t len);
int boost_json_config(char *json, size_t len);
int boost_json_tpms_config(char *json, size_t len);
int boost_json_calibration(char *json, size_t len);
int boost_json_network_status(char *json, size_t len);
/* One theme entry of the /themes "themes" array (no leading comma). */
int boost_json_theme_item(char *json, size_t len, const boost_theme_t *theme);
/* Complete /themes payload (config block + array). Returns -1 if the payload
 * would not fit, so no caller ever serves truncated JSON. */
int boost_json_themes(char *json, size_t len);

#ifdef __cplusplus
}
#endif
