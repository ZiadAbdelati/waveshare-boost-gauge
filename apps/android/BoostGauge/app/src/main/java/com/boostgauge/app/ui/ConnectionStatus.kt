package com.boostgauge.app.ui

import com.boostgauge.app.data.ConnectionStatus

/**
 * Single display mapping for the connection state. Both the Connection page
 * status pill and the dashboard footer render through this so they can never
 * diverge after the same transport event.
 */
fun ConnectionStatus.displayLabel(peerKnown: Boolean, reconnectAttempt: Int?): String = when (this) {
    ConnectionStatus.Connected -> "Live · BLE"
    ConnectionStatus.Reconnecting -> "Reconnecting… (attempt ${reconnectAttempt ?: 1})"
    ConnectionStatus.Disconnected -> if (peerKnown) "Disconnected" else "Not connected"
}