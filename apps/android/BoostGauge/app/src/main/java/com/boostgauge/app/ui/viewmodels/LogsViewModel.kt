package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.LogSample
import com.boostgauge.app.data.api.Status
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

class LogsViewModel(
    private val api: GaugeApi,
    private val repository: GaugeRepository,
    private val ioDispatcher: CoroutineDispatcher = Dispatchers.Default,
) : ViewModel() {

    data class UiState(
        val loading: Boolean = true,
        val window: LogWindow = LogWindow.fiveMinutes,
        val samples: List<LogSample> = emptyList(),
        val error: String? = null,
        val source: String = "Last 5 minutes · 0 samples",
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /**
     * Decoded sample arrays per window limit, so switching 1m/5m/15m chips
     * never re-fetches or re-decodes a window already on screen. Entries are
     * replaced by a forced refresh (the Refresh button) and dropped wholesale
     * when a fresh fetch shows the device restarted (uptime reset → cached
     * windows belong to an earlier boot).
     */
    private val cache = mutableMapOf<Int, List<LogSample>>()

    init {
        load(LogWindow.fiveMinutes.limit)
    }

    fun load(limit: Int = _state.value.window.limit, force: Boolean = false) {
        val window = LogWindow(limit)
        viewModelScope.launch {
            _state.update { it.copy(loading = true, window = window, error = null) }
            val cached = if (force) null else cache[limit]
            if (cached != null) {
                _state.update {
                    it.copy(
                        loading = false,
                        samples = cached,
                        source = "${window.scope} · ${cached.size} samples",
                    )
                }
                return@launch
            }
            runCatching {
                // Offload the fetch + JSON decode so a 4500-sample window never
                // blocks the main thread (mirrors iOS Task.detached).
                withContext(ioDispatcher) { api.getLogs(limit) }
            }
                .onSuccess { payload ->
                    invalidateIfDeviceRestarted(payload.samples)
                    cache[limit] = payload.samples
                    _state.update {
                        it.copy(
                            loading = false,
                            samples = payload.samples,
                            source = "${window.scope} · ${payload.samples.size} samples",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(loading = false, error = e.message ?: "failed to load logs")
                    }
                }
        }
    }

    /**
     * A fresh window whose newest sample is older than a cached window's newest
     * can only mean the device rebooted (its ring uptime reset), so the cached
     * windows describe a previous boot — drop them all.
     */
    private fun invalidateIfDeviceRestarted(fresh: List<LogSample>) {
        val freshNewestMs = fresh.lastOrNull()?.tMs ?: return
        if (cache.any { (_, samples) ->
                samples.lastOrNull()?.let { it.tMs > freshNewestMs + REBOOT_MARGIN_MS } == true
            }) {
            cache.clear()
        }
    }

    /** The /state anchor used to derive absolute wall-clock times for the graph. */
    fun anchor(): Status? = repository.status.value

    /**
     * Reconstructs the /logs.csv columns client-side from the JSON samples,
     * using the same epoch derivation as boost_web.c:logs_csv_get().
     */
    fun exportCsv(): String {
        val state = status()
        val builder = StringBuilder()
        builder.append("timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo\n")
        _state.value.samples.forEach { sample ->
            val epochMs = if (state?.epochMs != null && state.epochMs > 0 && state.uptimeMs > 0) {
                state.epochMs - (state.uptimeMs - sample.tMs)
            } else {
                0L
            }
            val localText = if (epochMs > 0) {
                Instant.ofEpochMilli(epochMs)
                    .atZone(ZoneId.systemDefault())
                    .format(CSV_TIMESTAMP)
            } else {
                ""
            }
            builder.append(localText)
                .append(',')
                .append(state?.timezoneOffsetMinutes ?: 0)
                .append(',')
                .append(epochMs)
                .append(',')
                .append(sample.tMs)
                .append(',')
                .append("%.2f".format(sample.psi))
                .append(',')
                .append("%.2f".format(sample.peakPsi))
                .append(',')
                .append(sample.zone)
                .append(',')
                .append(if (sample.demo) 1 else 0)
                .append('\n')
        }
        return builder.toString()
    }

    private fun status() = repository.status.value

    companion object {
        /** Last 5 minutes at the firmware's 5 Hz log rate (BOOST_LOG_INTERVAL_MS 200). */
        const val LOG_WINDOW_SAMPLES = 1500

        /** A cached window is stale if a fresh fetch's newest sample is this much older. */
        private const val REBOOT_MARGIN_MS = 5_000L
        private val CSV_TIMESTAMP: DateTimeFormatter =
            DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss")
    }
}

/**
 * User-selectable graph window. Defaults to 5 minutes; 1m and 15m map to
 * /logs?limit=300 / 4500. Never the full-hour ring (18,000 samples).
 */
data class LogWindow(val title: String, val limit: Int, val scope: String) {
    companion object {
        val all: List<LogWindow> = listOf(
            LogWindow("1m", 300, "Last 1 minute"),
            LogWindow("5m", 1500, "Last 5 minutes"),
            LogWindow("15m", 4500, "Last 15 minutes"),
        )
        val fiveMinutes: LogWindow = all[1]
    }
}

fun LogWindow(limit: Int): LogWindow =
    LogWindow.all.firstOrNull { it.limit == limit } ?: LogWindow.fiveMinutes
