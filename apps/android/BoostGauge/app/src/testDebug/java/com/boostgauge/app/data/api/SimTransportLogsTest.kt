package com.boostgauge.app.data.api

import com.boostgauge.app.data.transport.SimBleTransport
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Debug-only: the emulator sim transport serves the full 5-minute log window
 * over BLE (it is a BleGaugeTransport that owns the ring). Compiled only for
 * the debug variant alongside SimBleTransport itself.
 */
class SimTransportLogsTest {

    @Test
    fun logsOverBleSimServesFullFiveMinuteWindow() = runBlocking {
        val transport = SimBleTransport()
        try {
            val api = GaugeApi { transport }
            val logs = api.getLogs(1500)
            assertEquals(1500, logs.samples.size)
        } finally {
            transport.close()
        }
    }
}