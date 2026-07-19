package dev.boostgauge

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import dev.boostgauge.api.BoostGaugeApi
import dev.boostgauge.api.DimSchedule
import dev.boostgauge.api.GaugeConfig
import dev.boostgauge.api.GaugeState
import dev.boostgauge.api.GaugeTheme
import dev.boostgauge.api.LogSample
import dev.boostgauge.data.ConnectionStore
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import okhttp3.WebSocket
import java.time.Instant
import java.time.ZoneId
import java.util.TimeZone

data class BoostGaugeUiState(
    val baseUrlInput: String = "http://boostgauge.local",
    val connectedBaseUrl: String = "",
    val connected: Boolean = false,
    val liveMode: String = "idle",
    val busy: Boolean = false,
    val error: String = "",
    val state: GaugeState = GaugeState(),
    val config: GaugeConfig = GaugeConfig(),
    val themes: List<GaugeTheme> = emptyList(),
    val activeThemeId: String = "pit-lane",
    val logs: List<LogSample> = emptyList()
)

class BoostGaugeViewModel(application: Application) : AndroidViewModel(application) {
    private val store = ConnectionStore(application)
    private var api: BoostGaugeApi? = null
    private var websocket: WebSocket? = null
    private var pollJob: Job? = null
    private var eventWatchdogJob: Job? = null
    private var lastEventAt = 0L

    private val _uiState = MutableStateFlow(
        BoostGaugeUiState(
            baseUrlInput = store.loadBaseUrl().ifBlank { "http://boostgauge.local" }
        )
    )
    val uiState: StateFlow<BoostGaugeUiState> = _uiState

    fun setBaseUrl(value: String) {
        _uiState.update { it.copy(baseUrlInput = value, error = "") }
    }

    fun connect() {
        val baseUrl = BoostGaugeApi.normalizeBaseUrl(_uiState.value.baseUrlInput)
        store.saveBaseUrl(baseUrl)
        api = BoostGaugeApi(baseUrl)
        websocket?.cancel()
        pollJob?.cancel()
        eventWatchdogJob?.cancel()
        _uiState.update {
            it.copy(
                busy = true,
                error = "",
                connectedBaseUrl = baseUrl,
                liveMode = "connecting"
            )
        }
        viewModelScope.launch {
            runCatching {
                val nextState = requireNotNull(api).state()
                val nextConfig = requireNotNull(api).config()
                val catalog = requireNotNull(api).themes()
                val logResponse = requireNotNull(api).logs()
                _uiState.update {
                    it.copy(
                        connected = true,
                        busy = false,
                        state = nextState,
                        config = nextConfig,
                        themes = catalog.themes,
                        activeThemeId = catalog.activeThemeId.ifBlank { nextState.activeThemeId },
                        logs = logResponse.samples,
                        liveMode = "events"
                    )
                }
                openEvents()
                startEventWatchdog()
            }.onFailure { error ->
                _uiState.update {
                    it.copy(
                        connected = false,
                        busy = false,
                        liveMode = "offline",
                        error = error.message ?: "Connection failed"
                    )
                }
            }
        }
    }

    fun refreshConfig() {
        val currentApi = api ?: return
        viewModelScope.launch {
            runCatching {
                val nextConfig = currentApi.config()
                val catalog = currentApi.themes()
                _uiState.update {
                    it.copy(
                        config = nextConfig,
                        themes = catalog.themes,
                        activeThemeId = catalog.activeThemeId.ifBlank { nextConfig.activeThemeId },
                        error = ""
                    )
                }
            }.onFailure(::showError)
        }
    }

    fun setBrightnessHigh(value: Int) {
        _uiState.update { it.copy(config = it.config.copy(brightnessHigh = value)) }
    }

    fun setBrightnessLow(value: Int) {
        _uiState.update { it.copy(config = it.config.copy(brightnessLow = value)) }
    }

    fun setScheduleEnabled(enabled: Boolean) {
        _uiState.update {
            it.copy(config = it.config.copy(dimSchedule = it.config.dimSchedule.copy(enabled = enabled)))
        }
    }

    fun setScheduleStart(value: String) {
        updateSchedule(minutes = parseMinutes(value), start = true)
    }

    fun setScheduleEnd(value: String) {
        updateSchedule(minutes = parseMinutes(value), start = false)
    }

    fun saveConfig() {
        val currentApi = api ?: return
        val config = _uiState.value.config
        viewModelScope.launch {
            _uiState.update { it.copy(busy = true) }
            runCatching { currentApi.updateConfig(config) }
                .onSuccess { updated -> _uiState.update { it.copy(config = updated, busy = false, error = "") } }
                .onFailure { error ->
                    _uiState.update { it.copy(busy = false) }
                    showError(error)
                }
        }
    }

    fun syncDeviceTime() {
        val currentApi = api ?: return
        val offsetMinutes = TimeZone.getDefault().getOffset(System.currentTimeMillis()) / 60_000
        viewModelScope.launch {
            runCatching { currentApi.syncTime(System.currentTimeMillis(), offsetMinutes) }
                .onSuccess {
                    _uiState.update {
                        it.copy(
                            config = it.config.copy(timezoneOffsetMinutes = offsetMinutes),
                            error = ""
                        )
                    }
                    refreshConfig()
                }
                .onFailure(::showError)
        }
    }

    fun selectTheme(id: String) {
        val currentApi = api ?: return
        viewModelScope.launch {
            runCatching { currentApi.setActiveTheme(id) }
                .onSuccess {
                    _uiState.update {
                        it.copy(
                            activeThemeId = id,
                            state = it.state.copy(activeThemeId = id),
                            config = it.config.copy(activeThemeId = id),
                            error = ""
                        )
                    }
                }
                .onFailure(::showError)
        }
    }

    fun refreshLogs() {
        val currentApi = api ?: return
        viewModelScope.launch {
            runCatching { currentApi.logs(limit = 180) }
                .onSuccess { response -> _uiState.update { it.copy(logs = response.samples, error = "") } }
                .onFailure(::showError)
        }
    }

    fun clearLogs() {
        val currentApi = api ?: return
        viewModelScope.launch {
            runCatching { currentApi.clearLogs() }
                .onSuccess { _uiState.update { it.copy(logs = emptyList(), error = "") } }
                .onFailure(::showError)
        }
    }

    fun dashboardUrl(): String = api?.dashboardUrl ?: BoostGaugeApi.normalizeBaseUrl(_uiState.value.baseUrlInput)

    fun logsCsvUrl(): String = (api ?: BoostGaugeApi(_uiState.value.baseUrlInput)).logsCsvUri.toString()

    override fun onCleared() {
        websocket?.cancel()
        pollJob?.cancel()
        eventWatchdogJob?.cancel()
        super.onCleared()
    }

    private fun openEvents() {
        val currentApi = api ?: return
        websocket = currentApi.openEvents(object : BoostGaugeApi.EventListener {
            override fun onState(state: GaugeState) {
                lastEventAt = System.currentTimeMillis()
                _uiState.update {
                    it.copy(
                        state = state,
                        connected = true,
                        liveMode = "events",
                        error = ""
                    )
                }
            }

            override fun onFailure(error: Throwable) {
                startPollingFallback(error.message ?: "Event stream unavailable")
            }

            override fun onClosed() {
                startPollingFallback("Event stream closed")
            }
        })
        lastEventAt = System.currentTimeMillis()
    }

    private fun startEventWatchdog() {
        eventWatchdogJob = viewModelScope.launch {
            while (true) {
                delay(3_000)
                if (_uiState.value.liveMode == "events" && System.currentTimeMillis() - lastEventAt > 5_000) {
                    startPollingFallback("No live events; polling")
                }
            }
        }
    }

    private fun startPollingFallback(reason: String) {
        if (pollJob?.isActive == true) return
        _uiState.update { it.copy(liveMode = "polling", error = reason) }
        pollJob = viewModelScope.launch {
            val currentApi = api ?: return@launch
            while (true) {
                runCatching { currentApi.state() }
                    .onSuccess { next ->
                        _uiState.update {
                            it.copy(state = next, connected = true, liveMode = "polling")
                        }
                    }
                    .onFailure { error ->
                        _uiState.update {
                            it.copy(connected = false, liveMode = "offline", error = error.message ?: "Polling failed")
                        }
                    }
                delay(1_000)
            }
        }
    }

    private fun updateSchedule(minutes: Int?, start: Boolean) {
        if (minutes == null) return
        _uiState.update {
            val schedule = if (start) {
                it.config.dimSchedule.copy(startMinutes = minutes)
            } else {
                it.config.dimSchedule.copy(endMinutes = minutes)
            }
            it.copy(config = it.config.copy(dimSchedule = schedule))
        }
    }

    private fun parseMinutes(value: String): Int? {
        val parts = value.trim().split(":")
        if (parts.size != 2) return null
        val hour = parts[0].toIntOrNull() ?: return null
        val minute = parts[1].toIntOrNull() ?: return null
        if (hour !in 0..23 || minute !in 0..59) return null
        return hour * 60 + minute
    }

    private fun showError(error: Throwable) {
        _uiState.update { it.copy(error = error.message ?: "Request failed") }
    }
}

fun minutesLabel(minutes: Int): String = "%02d:%02d".format((minutes / 60).floorMod(24), minutes.floorMod(60))

private fun Int.floorMod(divisor: Int): Int = ((this % divisor) + divisor) % divisor

fun Long.uptimeLabel(): String {
    val seconds = this / 1_000
    val hours = seconds / 3_600
    val minutes = (seconds % 3_600) / 60
    return "${hours}h ${minutes}m"
}

fun Long.wallClockLabel(): String =
    if (this <= 0) "not set" else Instant.ofEpochMilli(this).atZone(ZoneId.systemDefault()).toLocalDateTime().toString()
