package com.boostgauge.app.data.transport

import kotlinx.coroutines.flow.StateFlow

/**
 * The live, push-based transport surface the repository and API consume from
 * BLE. The real [BleTransport] and the in-process emulator [SimBleTransport]
 * both expose it, so the repository's statusLine loop and the BGL1 log read
 * behave identically for both.
 */
internal interface BleGaugeTransport : GaugeTransport {
    val transportKind: String
    val statusLine: StateFlow<String?>
    /**
     * True while the GATT link is established. Flips false on link loss, on a
     * failed connect, and on close — the reconnect loop keys off this rather
     * than a status-frame timeout, so a stalled /state stream can never demote
     * a live link to "Reconnecting" (the Live·BLE ↔ Attempting ping-pong).
     */
    val linkUp: StateFlow<Boolean>
    suspend fun connect() {}
    suspend fun readLog(): String
    suspend fun readStatus(): String

    /** Raw device-info JSON; overrides return the firmware shape. */
    suspend fun readDeviceInfo(): String = """{"name":"BoostGauge","api":1}"""
}