package com.boostgauge.app.data

/**
 * Single source of truth for the live-link state. Derived by GaugeRepository
 * from the TransportController transport flow plus the link flags, and exposed
 * to every screen so the Connection pill, the dashboard footer and the
 * disconnect affordances can never diverge.
 */
enum class ConnectionStatus {
    /** Link is up and the last /state sample was delivered. */
    Connected,

    /** A peer is known but the link is down; the reconnect loop is retrying. */
    Reconnecting,

    /** No transport at all (pre-first-connection or explicit Disconnect). */
    Disconnected,
}