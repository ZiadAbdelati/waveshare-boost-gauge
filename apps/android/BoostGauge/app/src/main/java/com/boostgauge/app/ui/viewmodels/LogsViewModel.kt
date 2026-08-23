package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.LogSample
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

class LogsViewModel(
    private val api: GaugeApi,
    private val repository: GaugeRepository,
) : ViewModel() {

    data class UiState(
        val loading: Boolean = true,
        val limit: Int = 300,
        val samples: List<LogSample> = emptyList(),
        val error: String? = null,
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    init {
        load(300)
    }

    fun load(limit: Int = _state.value.limit) {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, limit = limit, error = null) }
            runCatching { api.getLogs(limit) }
                .onSuccess { payload ->
                    _state.update { it.copy(loading = false, samples = payload.samples) }
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(loading = false, error = e.message ?: "failed to load logs")
                    }
                }
        }
    }

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
        private val CSV_TIMESTAMP: DateTimeFormatter =
            DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss")
    }
}
