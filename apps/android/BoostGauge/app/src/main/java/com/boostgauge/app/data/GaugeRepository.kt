package com.boostgauge.app.data

import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.transport.BleGaugeTransport
import com.boostgauge.app.data.transport.GaugeTransport
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.dropWhile
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

/**
 * Live status source: 1 Hz HTTP polling, or the BLE Status-characteristic
 * notifications (with a polling fallback when notifications stall).
 *
 * start() is intentionally re-entrant: MainActivity passes its lifecycleScope,
 * and that scope is cancelled and replaced on every recreation (rotation,
 * uiMode change). A once-only guard would silently freeze the dashboard after
 * the first recreation, so each start() cancels the previous poll job and
 * launches a fresh one in the current scope.
 *
 * Connection state is SINGLE-WRITER: every link transition goes through
 * [setLink], which owns [_connectionStatus] and the derived [connected] flow.
 * The reconnect loop keys off the transport's own `linkUp` event flow — never
 * off a status-frame timeout — so a stalled /state stream can never demote a
 * live link to Reconnecting (the Live·BLE ↔ Reconnecting ping-pong).
 */
class GaugeRepository(
    private val api: GaugeApi,
    private val transport: StateFlow<GaugeTransport?>,
) {
    private val _status = MutableStateFlow<Status?>(null)
    val status: StateFlow<Status?> = _status.asStateFlow()

    private val _reconnectAttempt = MutableStateFlow<Int?>(null)
    val reconnectAttempt: StateFlow<Int?> = _reconnectAttempt.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    /** The single connection-state source every screen reads for its connection display. */
    private val _connectionStatus = MutableStateFlow(ConnectionStatus.Disconnected)
    val connectionStatus: StateFlow<ConnectionStatus> = _connectionStatus.asStateFlow()

    /**
     * Back-compat link flag for tests; written only by [setLink] in lock-step
     * with [_connectionStatus], so it can never diverge. Screens read
     * [connectionStatus], not this.
     */
    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    /**
     * The ONLY writer for the connection state. `up=false` with a live
     * transport means Reconnecting (a peer is known and the loop is retrying);
     * `up=false` with no transport means Disconnected. [attempt] feeds the
     * "Reconnecting… (attempt N)" banner.
     */
    private fun setLink(up: Boolean, attempt: Int? = _reconnectAttempt.value) {
        _reconnectAttempt.value = attempt
        _connected.value = up
        _connectionStatus.value = when {
            transport.value == null -> ConnectionStatus.Disconnected
            up -> ConnectionStatus.Connected
            else -> ConnectionStatus.Reconnecting
        }
    }

    /** Test/screenshot hook: force the displayed connection state without a real GATT link. */
    fun debugSetReconnectState(connected: Boolean, attempt: Int?) {
        setLink(connected, attempt)
    }

    /**
     * Explicit transport teardown (user Disconnect): drop the link flags and
     * the last-known /state payload immediately so every screen shows
     * not-connected placeholders, never stale values.
     */
    fun onTransportDisconnected() {
        setLink(false, null)
        _status.value = null
        _lastError.value = null
    }

    private var pollJob: Job? = null
    private var lastBleFrame: String? = null

    fun start(scope: CoroutineScope) {
        pollJob?.cancel()
        pollJob = scope.launch { statusLoop() }
    }

    suspend fun refresh(): Boolean {
        return try {
            (transport.value as? BleGaugeTransport)?.connect()
            _status.value = api.getState()
            setLink(true, null)
            _lastError.value = null
            true
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // Only surface the error as a banner when there is no known BLE peer.
            // When a peer is known we are in the reconnecting state — the banner
            // is "Reconnecting… (attempt N)", never "Disconnected".
            if (transport.value == null) {
                _lastError.value = null
            } else {
                _lastError.value = e.message ?: "gauge unreachable"
            }
            setLink(false)
            false
        }
    }

    private suspend fun statusLoop() {
        while (true) {
            val active = transport.value
            if (active is BleGaugeTransport) {
                if (connectionStatus.value != ConnectionStatus.Connected) {
                    // Exponential reconnect: 1→1s, 2→2s, 3→5s, 4→10s, 5→30s, 6+→60s.
                    val attempt = _reconnectAttempt.value?.let {
                        it
                    } ?: run {
                        _reconnectAttempt.value = 1
                        1
                    }
                    delay(backoffDelayMs(attempt))
                    val ok = refresh()
                    if (ok) {
                        _reconnectAttempt.value = null
                    } else {
                        _reconnectAttempt.value = attempt + 1
                    }
                    continue
                }
                // Link is up: read status frames for display. A frame stall is
                // NOT link loss — the transport's linkUp event owns demotion.
                // If frames stall while the link claims up, probe with a refresh
                // so a half-dead link self-heals instead of freezing the readout.
                val frame = withTimeoutOrNull(BLE_STATUS_TIMEOUT_MS) {
                    if (lastBleFrame == null) {
                        active.statusLine.first { it != null }
                    } else {
                        active.statusLine.dropWhile { it == lastBleFrame }.first { it != null }
                    }
                }
                if (frame != null) {
                    lastBleFrame = frame
                    applyStatusJson(frame)
                    setLink(true, null)
                } else if (!refresh()) {
                    setLink(false)
                }
            } else {
                if (active == null) {
                    // No peer selected (BLE empty) — pre-first-connection "Not connected".
                    // Drop the last-known payload too so no screen shows stale values.
                    setLink(false, null)
                    _status.value = null
                    _lastError.value = null
                    delay(1_000L)
                } else {
                    refresh()
                    delay(HTTP_POLL_MS)
                }
            }
        }
    }

    private fun applyStatusJson(json: String) {
        runCatching { ApiJson.json.decodeFromString<Status>(json) }
            .onSuccess { _status.value = it }
    }

    companion object {
        private const val HTTP_POLL_MS = 1_000L
        private const val BLE_STATUS_TIMEOUT_MS = 10_000L

        /** 1→1s, 2→2s, 3→5s, 4→10s, 5→30s, 6+→60s — resets on any successful connect. */
        fun backoffDelayMs(attempt: Int): Long = when {
            attempt <= 1 -> 1_000L
            attempt == 2 -> 2_000L
            attempt == 3 -> 5_000L
            attempt == 4 -> 10_000L
            attempt == 5 -> 30_000L
            else -> 60_000L
        }
    }
}