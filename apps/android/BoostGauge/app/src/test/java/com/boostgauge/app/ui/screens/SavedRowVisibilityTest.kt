package com.boostgauge.app.ui.screens

import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.ui.displayLabel
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Round-8 saved-row visibility matrix (see apps/PARITY.md): the saved gauge
 * row is visible whenever a peer is remembered AND the link is not connected
 * (incl. auto-reconnect + fresh launch), hidden entirely while connected, and
 * shows no Connect button while the loop is reconnecting.
 */
class SavedRowVisibilityTest {

    @Test
    fun visibilityMatrix() {
        // Connected: identity row WITHOUT a Connect action (mirrors iOS).
        assertEquals(SavedRowAction.Connected, savedRowAction(ConnectionStatus.Connected, peerKnown = true))
        // Fresh launch / no remembered peer: never a saved row, in any state.
        assertNull(savedRowAction(ConnectionStatus.Connected, peerKnown = false))
        assertNull(savedRowAction(ConnectionStatus.Disconnected, peerKnown = false))
        assertNull(savedRowAction(ConnectionStatus.Reconnecting, peerKnown = false))
        // Post-disconnect with a remembered peer: row with a Connect action.
        assertEquals(SavedRowAction.Connect, savedRowAction(ConnectionStatus.Disconnected, peerKnown = true))
        // Auto-reconnecting with a remembered peer: row present, but NO Connect
        // button — the "Reconnecting… (attempt N)" banner carries the state.
        assertEquals(SavedRowAction.Reconnecting, savedRowAction(ConnectionStatus.Reconnecting, peerKnown = true))
    }

    @Test
    fun pillLabelIsTheSingleStatusString() {
        // The pill/footer share one displayLabel helper: a remembered peer at
        // Disconnected renders exactly one "Disconnected"; no peer renders
        // "Not connected" (never the word "Disconnected" on a fresh launch).
        assertEquals("Live · BLE", ConnectionStatus.Connected.displayLabel(peerKnown = true, reconnectAttempt = null))
        assertEquals(
            "Reconnecting… (attempt 3)",
            ConnectionStatus.Reconnecting.displayLabel(peerKnown = true, reconnectAttempt = 3),
        )
        assertEquals(
            "Disconnected",
            ConnectionStatus.Disconnected.displayLabel(peerKnown = true, reconnectAttempt = null),
        )
        assertEquals(
            "Not connected",
            ConnectionStatus.Disconnected.displayLabel(peerKnown = false, reconnectAttempt = null),
        )
    }
}