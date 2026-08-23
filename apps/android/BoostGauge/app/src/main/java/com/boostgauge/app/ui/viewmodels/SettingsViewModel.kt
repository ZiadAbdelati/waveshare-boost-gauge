package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.ThemesPayload
import com.boostgauge.app.data.api.TpmsConfig
import com.boostgauge.app.data.settings.SettingsStore
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.BleScanResult
import com.boostgauge.app.ui.Format
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import java.util.TimeZone

class SettingsViewModel(
    private val api: GaugeApi,
    private val selection: StateFlow<TransportSelection>,
    private val selectTransport: suspend (TransportType, String) -> Unit,
    private val repository: GaugeRepository,
    private val scanDevices: suspend () -> List<BleScanResult> = { emptyList() },
) : ViewModel() {

    /**
     * Editable form text/toggles, owned by the ViewModel instead of
     * remember(config) inside LazyColumn items: scrolling disposes and
     * re-composes item content, and a remember-keyed field would re-seed
     * itself from server state and silently drop what the user typed.
     * Server values only overwrite these fields on an explicit reload
     * (refreshAll) or on save-response fold-back (withConfig/withThemes/withTpms).
     */
    data class FieldState(
        val brightnessHigh: String = "92",
        val brightnessLow: String = "10",
        val dimEnabled: Boolean = false,
        val dimStart: String = "1380",
        val dimEnd: String = "360",
        val psiMin: String = "-15.0",
        val psiMax: String = "10.0",
        val psiOverboost: String = "8.0",
        val zeroAngle: String = "90.0",
        val appBle: Boolean = false,
        val demoMode: Boolean = false,
        val demoFastSweep: Boolean = false,
        val tpmsBle: Boolean = false,
        val lowPsi: String = "32.0",
        val staleAfterMs: String = "15000",
        val httpAddress: String = SettingsStore.DEFAULT_HTTP_ADDRESS,
    ) {
        fun withConfig(config: Config): FieldState = copy(
            brightnessHigh = config.brightnessHigh.toString(),
            brightnessLow = config.brightnessLow.toString(),
            dimEnabled = config.dimSchedule.enabled,
            dimStart = config.dimSchedule.startMinutes.toString(),
            dimEnd = config.dimSchedule.endMinutes.toString(),
            psiMin = Format.fmt(config.psiMin, 1),
            psiMax = Format.fmt(config.psiMax, 1),
            psiOverboost = Format.fmt(config.psiOverboost, 1),
            zeroAngle = Format.fmt(config.zeroAngle, 0),
            appBle = config.appBle,
        )

        fun withThemes(themes: ThemesPayload): FieldState = copy(
            demoMode = themes.demoMode,
            demoFastSweep = themes.demoFastSweep,
            tpmsBle = themes.tpmsBle,
        )

        fun withTpms(tpms: TpmsConfig): FieldState = copy(
            lowPsi = Format.fmt(tpms.lowPsi, 1),
            staleAfterMs = tpms.staleAfterMs.toString(),
        )

        companion object {
            /** Initialises the form once per explicit load; null payload keeps defaults. */
            fun from(config: Config?, themes: ThemesPayload?, tpms: TpmsConfig?): FieldState {
                var fields = FieldState()
                if (config != null) fields = fields.withConfig(config)
                if (themes != null) fields = fields.withThemes(themes)
                if (tpms != null) fields = fields.withTpms(tpms)
                return fields
            }
        }
    }

    data class UiState(
        val loading: Boolean = true,
        val config: Config? = null,
        val tpms: TpmsConfig? = null,
        val themes: ThemesPayload? = null,
        val fields: FieldState = FieldState(),
        val saving: Boolean = false,
        val error: String? = null,
        val message: String? = null,
        val scanning: Boolean = false,
        val scannedDevices: List<BleScanResult> = emptyList(),
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    val transportSelection: StateFlow<TransportSelection> = selection
    val connected: StateFlow<Boolean> = repository.connected

    init {
        viewModelScope.launch {
            selection.collect { selected ->
                _state.update {
                    if (selected.type == TransportType.HTTP) {
                        it.copy(fields = it.fields.copy(httpAddress = selected.httpAddress))
                    } else {
                        it
                    }
                }
            }
        }
        refreshAll()
    }

    fun refreshAll() {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            runCatching {
                Triple(api.getConfig(), api.getTpmsConfig(), api.getThemes())
            }.onSuccess { (config, tpms, themes) ->
                _state.update {
                    it.copy(
                        loading = false,
                        config = config,
                        tpms = tpms,
                        themes = themes,
                        fields = it.fields.withConfig(config).withThemes(themes).withTpms(tpms),
                    )
                }
            }.onFailure { e ->
                _state.update { it.copy(loading = false, error = e.message ?: "failed to load settings") }
            }
        }
    }

    /** Local-only form edit (typing/toggling); never a server round trip. */
    fun updateFields(transform: (FieldState) -> FieldState) {
        _state.update { it.copy(fields = transform(it.fields)) }
    }

    fun saveConfig() {
        val fields = _state.value.fields
        val patch = buildJsonObject {
            fields.brightnessHigh.toIntOrNull()?.let { put("brightnessHigh", it) }
            fields.brightnessLow.toIntOrNull()?.let { put("brightnessLow", it) }
            put(
                "dimSchedule",
                buildJsonObject {
                    put("enabled", fields.dimEnabled)
                    fields.dimStart.toIntOrNull()?.let { put("startMinutes", it) }
                    fields.dimEnd.toIntOrNull()?.let { put("endMinutes", it) }
                },
            )
            fields.psiMin.toDoubleOrNull()?.let { put("psiMin", it) }
            fields.psiMax.toDoubleOrNull()?.let { put("psiMax", it) }
            fields.psiOverboost.toDoubleOrNull()?.let { put("psiOverboost", it) }
            fields.zeroAngle.toDoubleOrNull()?.let { put("zeroAngle", it) }
            put("appBle", fields.appBle)
        }
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { api.updateConfig(patch) }
                .onSuccess { config ->
                    _state.update {
                        it.copy(
                            saving = false,
                            config = config,
                            fields = it.fields.withConfig(config),
                            message = "Config saved",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "save failed") }
                }
        }
    }

    fun saveThemeFlags() {
        val fields = _state.value.fields
        val patch = buildJsonObject {
            put("demoMode", fields.demoMode)
            put("demoFastSweep", fields.demoFastSweep)
            put("tpmsBle", fields.tpmsBle)
        }
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { api.updateThemesConfig(patch) }
                .onSuccess { themes ->
                    _state.update {
                        it.copy(
                            saving = false,
                            themes = themes,
                            fields = it.fields.withThemes(themes),
                            message = "Theme settings saved",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "save failed") }
                }
        }
    }

    fun saveTpms() {
        val fields = _state.value.fields
        val lowPsi = fields.lowPsi.toDoubleOrNull() ?: return
        val staleAfterMs = fields.staleAfterMs.toLongOrNull() ?: return
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { api.updateTpmsConfig(lowPsi, staleAfterMs) }
                .onSuccess { tpms ->
                    _state.update {
                        it.copy(
                            saving = false,
                            tpms = tpms,
                            fields = it.fields.withTpms(tpms),
                            message = "TPMS config saved",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "save failed") }
                }
        }
    }

    fun syncTime() {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            val now = System.currentTimeMillis()
            val offset = TimeZone.getDefault().getOffset(now) / 60_000
            runCatching { api.syncTime(now, offset) }
                .onSuccess { _state.update { it.copy(saving = false, message = "Time synced") } }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "time sync failed") }
                }
        }
    }

    fun saveHttpAddress() {
        val address = _state.value.fields.httpAddress
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { selectTransport(TransportType.HTTP, address) }
                .onSuccess { _state.update { it.copy(saving = false, message = "HTTP transport selected") } }
                .onFailure { e -> _state.update { it.copy(saving = false, error = e.message) } }
        }
    }

    fun connectBle(address: String) {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { selectTransport(TransportType.BLE, address) }
                .onSuccess { _state.update { it.copy(saving = false, message = "BLE transport selected") } }
                .onFailure { e -> _state.update { it.copy(saving = false, error = e.message) } }
        }
    }

    fun scanForDevices() {
        viewModelScope.launch {
            _state.update { it.copy(scanning = true, error = null) }
            runCatching { scanDevices() }
                .onSuccess { devices ->
                    _state.update { it.copy(scanning = false, scannedDevices = devices) }
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(scanning = false, scannedDevices = emptyList(), error = e.message ?: "scan failed")
                    }
                }
        }
    }

    fun clearMessage() {
        _state.update { it.copy(message = null, error = null) }
    }
}
