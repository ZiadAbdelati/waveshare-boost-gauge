#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ELM327-over-BLE request/reply framing. The adapter presents a transparent
 * serial-UART GATT pipe; this layer owns byte accumulation, '>' completion
 * detection and the synchronous request primitive used by the application poll
 * loop. It knows nothing about OBD semantics or vehicles.
 */

/* Maximum accumulated reply length (before '>'). ELM PID/DID replies are a few
 * dozen bytes; this comfortably holds multi-line "SEARCHING...\r\r41 0C ...". */
#define BOOST_OBD_ELM_RX_MAX 256

/* Register the BLE transport receive callback. Call once at startup. */
void boost_obd_elm_init(void);

/* Send a command (CR appended by the layer) and wait for the adapter's '>'
 * prompt. `reply` receives the full response text (terminator excluded),
 * null-terminated and CR/LF trimmed at both ends. Returns true when '>' arrived
 * within timeout_ms; false on timeout, non-READY link or concurrent request. */
bool boost_obd_elm_request(const char *cmd, char *reply, size_t reply_size,
                           uint32_t timeout_ms);

/* Drop any in-flight accumulation and the active-request flag. Call on link
 * reset/re-init; an outstanding request will simply time out. */
void boost_obd_elm_reset(void);

/* Uptime (ms) of the last complete ELM reply ("prompt '>' seen"), 0 if none.
 * This tracks link/ELM liveness independently of whether a query decoded a
 * value - "NO DATA" is still a valid reply. */
uint32_t boost_obd_elm_last_reply_ms(void);

#ifdef __cplusplus
}
#endif
