package com.boostgauge.app.data.transport

import java.io.IOException

/** A single HTTP-style response from the gauge, independent of the wire transport. */
data class Resp(val status: Int, val body: String)

/** Transport-level failure (connect, I/O, timeout, GATT error). */
class TransportException(message: String, cause: Throwable? = null) : IOException(message, cause)

/** Both transports speak the gauge's /api/v1 surface; BLE wraps it in request/response frames. */
interface GaugeTransport {
    suspend fun get(path: String): Resp

    suspend fun send(method: String, path: String, bodyJson: String? = null): Resp

    suspend fun close() {}
}
