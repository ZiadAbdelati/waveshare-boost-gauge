package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.ThemesPayload
import com.boostgauge.app.data.api.TpmsConfig
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.BleScanResult
import com.boostgauge.app.ui.Format
import com.boostgauge.app.ui.Timezones
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

class SettingsViewModel(
    private val api: GaugeApi,
    private val selection: StateFlow<TransportSelection>,
    private val selectTransport: suspend (TransportType, String, String?) -> Unit,
    private val repository: GaugeRepository,
    private val scanDevices: suspend () -> List<BleScanResult> = { emptyList() },
    private val disconnectTransport: suspend () -> Unit = {},
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
        val rotation: Int = 0,
        val regionDBuf: Boolean = false,
        val teSync: Boolean = false,
        val teScanline: Boolean = false,
        val pixelShift: Boolean = false,
        val pixelShiftSec: String = "90",
        val tpmsBle: Boolean = false,
        val lowPsi: String = "32.0",
        val staleAfterMs: String = "15000",
        val timezoneOffsetMinutes: Int = 0,
        val timezoneTz: String = "",
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
            timezoneOffsetMinutes = config.timezoneOffsetMinutes,
            timezoneTz = config.timezoneTz,
        )

        fun withThemes(themes: ThemesPayload): FieldState = copy(
            demoMode = themes.demoMode,
            demoFastSweep = themes.demoFastSweep,
            rotation = themes.rotation,
            regionDBuf = themes.regionDBuf,
            teSync = themes.teSync,
            teScanline = themes.teScanline,
            pixelShift = themes.pixelShift,
            pixelShiftSec = themes.pixelShiftSec.toString(),
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
        val scanCompleted: Boolean = false,
        val scannedDevices: List<BleScanResult> = emptyList(),
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    val transportSelection: StateFlow<TransportSelection> = selection
    val connectionStatus: StateFlow<ConnectionStatus> = repository.connectionStatus
    val reconnectAttempt: StateFlow<Int?> = repository.reconnectAttempt
    val status: StateFlow<Status?> = repository.status

    init {
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
                val msg = e.message ?: "failed to load settings"
                // Disconnected (pre-first-connection OR explicit Disconnect) has
                // no transport: suppress the noisy "no transport selected"
                // banner — the Connection page already shows the canonical
                // Disconnected/Not connected status.
                if (msg.contains("no transport selected", ignoreCase = true) &&
                    repository.connectionStatus.value != ConnectionStatus.Connected
                ) {
                    _state.update { it.copy(loading = false, error = null) }
                } else {
                    _state.update { it.copy(loading = false, error = msg) }
                }
            }
        }
    }

    /** Local-only form edit (typing/toggling); never a server round trip. */
    fun updateFields(transform: (FieldState) -> FieldState) {
        _state.update { it.copy(fields = transform(it.fields)) }
    }

    fun saveDisplay() {
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
                            message = "Display saved",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "save failed") }
                }
        }
    }

    fun saveConfig() = saveDisplay()

    fun saveRange() {
        val fields = _state.value.fields
        val patch = buildJsonObject {
            fields.psiMin.toDoubleOrNull()?.let { put("psiMin", it) }
            fields.psiMax.toDoubleOrNull()?.let { put("psiMax", it) }
            fields.psiOverboost.toDoubleOrNull()?.let { put("psiOverboost", it) }
            fields.zeroAngle.toDoubleOrNull()?.let { put("zeroAngle", it) }
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
                            message = "Range saved",
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
            put("rotation", fields.rotation)
            put("regionDBuf", fields.regionDBuf)
            put("teSync", fields.teSync)
            put("teScanline", fields.teScanline)
            put("pixelShift", fields.pixelShift)
            fields.pixelShiftSec.toIntOrNull()?.let { put("pixelShiftSec", it) }
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

    /** Timezone-only sync: the gauge's RTC keeps time; this only sets the zone. */
    fun syncTime() {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            val zone = Timezones.forDefault()
            runCatching { api.syncTime(zone.offsetMinutes, zone.posix) }
                .onSuccess { status ->
                    _state.update {
                        it.copy(
                            saving = false,
                            fields = it.fields.copy(
                                timezoneOffsetMinutes = status.timezoneOffsetMinutes,
                                timezoneTz = zone.posix,
                            ),
                            message = "Timezone sent to gauge",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "timezone sync failed") }
                }
        }
    }

    /** Apply a timezone selection (curated entry or custom raw string) immediately. */
    fun applyTimezone(offsetMinutes: Int, timezoneTz: String) {
        val tz = timezoneTz.trim()
        if (tz.isEmpty()) {
            _state.update { it.copy(error = "Timezone string cannot be empty") }
            return
        }
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching { api.syncTime(offsetMinutes, tz) }
                .onSuccess { status ->
                    _state.update {
                        it.copy(
                            saving = false,
                            fields = it.fields.copy(
                                timezoneOffsetMinutes = status.timezoneOffsetMinutes,
                                timezoneTz = tz,
                            ),
                            message = "Timezone applied",
                        )
                    }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "timezone sync failed") }
                }
        }
    }

    /** Persist the TPMS BLE link toggle on its own (themes/config accepts partial patches). */
    fun saveTpmsBle() {
        val enabled = _state.value.fields.tpmsBle
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching {
                api.updateThemesConfig(buildJsonObject { put("tpmsBle", enabled) })
            }.onSuccess { themes ->
                _state.update {
                    it.copy(
                        saving = false,
                        themes = themes,
                        fields = it.fields.withThemes(themes),
                        message = if (enabled) "TPMS BLE link enabled" else "TPMS BLE link disabled",
                    )
                }
            }.onFailure { e ->
                _state.update { it.copy(saving = false, error = e.message ?: "save failed") }
            }
        }
    }

    /**
     * Clear the gauge's stored OBD peer via POST /api/v1/obd/forget (firmware
     * erases NVS `obd_peer` and drops any live link), then report the gauge's
     * actual state honestly from /state.
     */
    fun forgetObdPeer() {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching {
                api.forgetObdPeer()
                repository.refresh()
            }.onSuccess {
                val cleared = repository.status.value?.obd?.peerAddr.isNullOrBlank()
                _state.update {
                    it.copy(
                        saving = false,
                        message = if (cleared) {
                            "OBD peer forgotten"
                        } else {
                            "Forget sent — gauge still reports a peer"
                        },
                    )
                }
            }.onFailure { e ->
                _state.update { it.copy(saving = false, error = e.message ?: "forget failed") }
            }
        }
    }

    fun connectBle(device: BleScanResult) {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching {
                selectTransport(TransportType.BLE, device.address, device.name)
                repository.refresh()
            }.onSuccess {
                // select() no longer throws on a failed BLE connect — the
                // reconnect loop keeps retrying — so only claim "Connected"
                // when the link actually came up; otherwise leave the pill
                // ("Reconnecting… (attempt N)") to carry the state.
                if (repository.connectionStatus.value == ConnectionStatus.Connected) {
                    refreshAll()
                    _state.update { it.copy(saving = false, message = "Connected to ${device.name}") }
                } else {
                    _state.update { it.copy(saving = false, message = null) }
                }
            }
            .onFailure { e -> _state.update { it.copy(saving = false, error = e.message) } }
        }
    }

    fun disconnectBle() {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null) }
            runCatching { disconnectTransport() }
                .onSuccess {
                    // Drop the live link AND the last-known payload immediately so
                    // the pill/footer/page rows all read the single Disconnected
                    // state, never a stale Connected flag or stale sensor values.
                    repository.onTransportDisconnected()
                    // No message toast here: the pill already shows the canonical
                    // status, and echoing "Disconnected" would duplicate the word
                    // on the page (round-8 no-duplicate-status invariant).
                    _state.update { it.copy(saving = false, message = null) }
                }
                .onFailure { e -> _state.update { it.copy(saving = false, error = e.message) } }
        }
    }

    /** Reconnect to the persisted/remembered gauge without a fresh scan. */
    fun connectSavedGauge() {
        val saved = selection.value
        if (saved.bleAddress.isBlank()) return
        connectBle(BleScanResult(saved.bleAddress, saved.bleName.ifBlank { "BoostGauge" }))
    }

    fun scanForDevices() {
        viewModelScope.launch {
            _state.update { it.copy(scanning = true, error = null) }
            runCatching { scanDevices() }
                .onSuccess { devices ->
                    _state.update { it.copy(scanning = false, scanCompleted = true, scannedDevices = devices) }
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(
                            scanning = false,
                            scanCompleted = true,
                            scannedDevices = emptyList(),
                            error = e.message ?: "scan failed",
                        )
                    }
                }
        }
    }

    fun clearMessage() {
        _state.update { it.copy(message = null, error = null) }
    }
}
