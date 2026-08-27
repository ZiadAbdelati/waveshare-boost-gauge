package com.boostgauge.app.ui.screens

import com.boostgauge.app.data.api.Obd
import org.junit.Assert.assertEquals
import org.junit.Test

class ObdScannerMappingTest {

    private fun obd(
        state: Int = 0,
        lastError: Long = 0L,
        peer: String = "",
        peerAddr: String = "",
    ) = Obd(state = state, lastError = lastError, peer = peer, peerAddr = peerAddr)

    @Test
    fun `pill maps firmware OBD states to the PARITY labels`() {
        assertEquals("Idle", obdPillLabel(null))
        assertEquals("Idle", obdPillLabel(obd(state = 0)))
        assertEquals("Scanning", obdPillLabel(obd(state = 1)))
        assertEquals("Connecting to vlinker fd+", obdPillLabel(obd(state = 2, peer = "vlinker fd+")))
        assertEquals("Connecting to adapter", obdPillLabel(obd(state = 2)))
        assertEquals("Connected", obdPillLabel(obd(state = 3)))
        assertEquals("Idle", obdPillLabel(obd(state = 4)))
    }

    @Test
    fun `idle pill surfaces a non-zero lastError`() {
        assertEquals("Idle · error 133", obdPillLabel(obd(state = 4, lastError = 133L)))
        assertEquals("Idle · error 1", obdPillLabel(obd(state = 0, lastError = 1L)))
        // Non-idle states never show the stale error.
        assertEquals("Connected", obdPillLabel(obd(state = 3, lastError = 133L)))
    }

    @Test
    fun `peer row shows name plus address, em dash when none`() {
        assertEquals("—", obdPeerLine(null))
        assertEquals("—", obdPeerLine(obd()))
        assertEquals("vlinker fd+ · 11:22:33:44:55:66", obdPeerLine(obd(peer = "vlinker fd+", peerAddr = "11:22:33:44:55:66")))
        assertEquals("vlinker fd+ · —", obdPeerLine(obd(peer = "vlinker fd+")))
        assertEquals("— · 11:22:33:44:55:66", obdPeerLine(obd(peerAddr = "11:22:33:44:55:66")))
    }
}