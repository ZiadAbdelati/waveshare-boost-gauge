package com.boostgauge.app.data

import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.transport.BleTransport
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
 */
class GaugeRepository(
    private val api: GaugeApi,
    private val transport: StateFlow<GaugeTransport?>,
) {
    private val _status = MutableStateFlow<Status?>(null)
    val status: StateFlow<Status?> = _status.asStateFlow()

    private val _connected = MutableStateFlow(false)
    val connected: StateFlow<Boolean> = _connected.asStateFlow()

    private val _lastError = MutableStateFlow<String?>(null)
    val lastError: StateFlow<String?> = _lastError.asStateFlow()

    private var pollJob: Job? = null
    private var lastBleFrame: String? = null

    fun start(scope: CoroutineScope) {
        pollJob?.cancel()
        pollJob = scope.launch { statusLoop() }
    }

    suspend fun refresh(): Boolean {
        return try {
            _status.value = api.getState()
            _connected.value = true
            _lastError.value = null
            true
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            _connected.value = false
            _lastError.value = e.message ?: "gauge unreachable"
            false
        }
    }

    private suspend fun statusLoop() {
        while (true) {
            val active = transport.value
            if (active is BleTransport) {
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
                    _connected.value = true
                    _lastError.value = null
                } else {
                    refresh()
                }
            } else {
                refresh()
                delay(HTTP_POLL_MS)
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
    }
}
