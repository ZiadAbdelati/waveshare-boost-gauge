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
import com.boostgauge.app.ui.PressureUnit
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
    var contextProvider: android.content.Context? = null,
    private val forgetTransport: suspend () -> Unit = {},
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
        /**
         * Canonical PSI each pressure string was last rendered from. The
         * strings above are display-only (rounded to the unit's decimals), so
         * Save must send THIS value when the user did not edit the field —
         * reconstructing PSI from the rounded string silently drifts the
         * gauge geometry (bar) or trips the firmware's -30 psi floor (kPa).
         */
        val psiMinValue: Double = -15.0,
        val psiMaxValue: Double = 10.0,
        val psiOverboostValue: Double = 8.0,
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
        /** Display unit for every pressure field/readout on this form. */
        val pressureUnit: PressureUnit = PressureUnit.default,
        /** Display reference for every boost-side numeral (atmospheric vs absolute). */
        val pressureAbsolute: Boolean = false,
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
            psiMin = pressureUnit.format(config.psiMin, 1),
            psiMax = pressureUnit.format(config.psiMax, 1),
            psiOverboost = pressureUnit.format(config.psiOverboost, 1),
            psiMinValue = config.psiMin,
            psiMaxValue = config.psiMax,
            psiOverboostValue = config.psiOverboost,
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
            pressureUnit = PressureUnit.fromWire(themes.pressureUnit),
            pressureAbsolute = themes.pressureAbsolute,
        )

        fun withTpms(tpms: TpmsConfig): FieldState = copy(
            lowPsi = pressureUnit.format(tpms.lowPsi, 1),
            staleAfterMs = tpms.staleAfterMs.toString(),
        )

        /**
         * Re-formats ONLY the pressure strings from the authoritative PSI
         * payloads after the device unit changed, leaving every other field
         * (and its unsaved edits) untouched. Seeding from the payload — rather
         * than converting the displayed strings — keeps psi values exact on a
         * unit round-trip.
         */
        fun withPressureUnit(config: Config?, tpms: TpmsConfig?, unit: PressureUnit): FieldState = copy(
            pressureUnit = unit,
            psiMin = config?.let { unit.format(it.psiMin, 1) } ?: psiMin,
            psiMax = config?.let { unit.format(it.psiMax, 1) } ?: psiMax,
            psiOverboost = config?.let { unit.format(it.psiOverboost, 1) } ?: psiOverboost,
            psiMinValue = config?.psiMin ?: psiMinValue,
            psiMaxValue = config?.psiMax ?: psiMaxValue,
            psiOverboostValue = config?.psiOverboost ?: psiOverboostValue,
            lowPsi = tpms?.let { unit.format(it.lowPsi, 1) } ?: lowPsi,
        )

        companion object {
            /**
             * Initialises the form once per explicit load; null payload keeps
             * defaults. Themes first: it carries `pressureUnit`, and the
             * config/tpms pressure strings are formatted in that unit.
             */
            fun from(config: Config?, themes: ThemesPayload?, tpms: TpmsConfig?): FieldState {
                var fields = FieldState()
                if (themes != null) fields = fields.withThemes(themes)
                if (config != null) fields = fields.withConfig(config)
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
        val networkStatus: com.boostgauge.app.data.api.NetworkStatus? = null,
        val wifiNetworks: List<com.boostgauge.app.data.api.WifiNetwork> = emptyList(),
        val scanningWifi: Boolean = false,
        val wifiSsid: String = "",
        val wifiPassword: String = "",
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
                val network = runCatching { api.getNetworkStatus() }.getOrNull()
                _state.update {
                    it.copy(
                        loading = false,
                        config = config,
                        tpms = tpms,
                        themes = themes,
                        fields = it.fields.withThemes(themes).withConfig(config).withTpms(tpms),
                        networkStatus = network,
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

    /**
     * Shared save skeleton (mirrors the iOS SettingsViewModel
     * save(_:method:path:body:onSuccess:)): mark saving, run the request, then
     * fold the WHOLE response into state atomically inside one update so the
     * render never races the save, and publish the message or error. The fold
     * returns a state transform so it can re-seed the form fields from the
     * server response.
     */
    private fun <T> save(
        body: suspend () -> T,
        message: String,
        errorMessage: String = "save failed",
        fold: (T) -> (UiState) -> UiState,
    ) {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching { body() }
                .onSuccess { result ->
                    _state.update { state -> fold(result)(state).copy(saving = false, message = message) }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: errorMessage) }
                }
        }
    }

    fun saveDisplay() {
        val fields = _state.value.fields
        save(
            body = {
                val configPatch = buildJsonObject {
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
                }
                val config = api.updateConfig(configPatch)
                val themePatch = buildJsonObject {
                    put("rotation", fields.rotation)
                    put("regionDBuf", fields.regionDBuf)
                    put("teSync", fields.teSync)
                    put("teScanline", fields.teScanline)
                    put("pixelShift", fields.pixelShift)
                    fields.pixelShiftSec.toIntOrNull()?.let { put("pixelShiftSec", it) }
                }
                val themes = api.updateThemesConfig(themePatch)
                Pair(config, themes)
            },
            message = "Display saved",
        ) { (config, themes) ->
            { s -> s.copy(config = config, themes = themes, fields = s.fields.withThemes(themes).withConfig(config)) }
        }
    }

    fun saveDemoMode() {
        val fields = _state.value.fields
        val patch = buildJsonObject {
            put("demoMode", fields.demoMode)
            put("demoFastSweep", fields.demoFastSweep)
        }
        save(
            body = { api.updateThemesConfig(patch) },
            message = "Demo settings saved",
        ) { themes ->
            { s -> s.copy(themes = themes, fields = s.fields.withThemes(themes)) }
        }
    }

    fun saveConfig() = saveDisplay()

    fun saveRange() {
        val fields = _state.value.fields
        val unit = fields.pressureUnit
        // An untouched field sends the canonical PSI it was rendered from;
        // only a field the user actually edited converts back from the display
        // unit. Reconstructing PSI from the rounded display string drifts the
        // gauge geometry (bar: 10.0 -> 10.0076) and trips the firmware's
        // -30 psi floor (kPa), so it must never happen for an untouched field.
        fun canonical(display: String, retained: Double): Double? =
            if (display == unit.format(retained, 1)) retained
            else display.toDoubleOrNull()?.let { unit.toPsi(it) }
        val patch = buildJsonObject {
            canonical(fields.psiMin, fields.psiMinValue)?.let { put("psiMin", it) }
            canonical(fields.psiMax, fields.psiMaxValue)?.let { put("psiMax", it) }
            canonical(fields.psiOverboost, fields.psiOverboostValue)?.let { put("psiOverboost", it) }
            fields.zeroAngle.toDoubleOrNull()?.let { put("zeroAngle", it) }
        }
        save(
            body = { api.updateConfig(patch) },
            message = "Range saved",
        ) { config ->
            { s -> s.copy(config = config, fields = s.fields.withConfig(config)) }
        }
    }

    /**
     * Persist the pressure unit on its own (themes/config accepts partial
     * patches, mirroring [saveTpmsBle]). The echoed payload carries the unit;
     * the pressure fields are re-formatted from the last known server values
     * so no psi string is left in the previous unit.
     */
    fun savePressureUnit(unit: PressureUnit) {
        save(
            body = { api.updateThemesConfig(buildJsonObject { put("pressureUnit", unit.wire) }) },
            message = "Pressure unit set to ${unit.suffix}",
        ) { themes ->
            { s ->
                s.copy(
                    themes = themes,
                    fields = s.fields.withPressureUnit(
                        config = s.config,
                        tpms = s.tpms,
                        unit = PressureUnit.fromWire(themes.pressureUnit),
                    ),
                )
            }
        }
    }

    /**
     * Persist the pressure reference on its own (themes/config accepts partial
     * patches, mirroring [savePressureUnit]). Atmospheric (`false`) is today's
     * gauge behaviour; absolute (`true`) adds the ambient reference to every
     * boost-side numeral. The echoed payload carries the flag back.
     */
    fun savePressureReference(absolute: Boolean) {
        save(
            body = { api.updateThemesConfig(buildJsonObject { put("pressureAbsolute", absolute) }) },
            message = if (absolute) "Pressure reference set to absolute" else "Pressure reference set to atmospheric",
        ) { themes ->
            { s -> s.copy(themes = themes, fields = s.fields.withThemes(themes)) }
        }
    }

    fun saveThemeFlags() = saveDemoMode()

    fun saveTpms() {
        val fields = _state.value.fields
        // The field edits the display unit; the wire lowPsi is always PSI.
        val lowPsi = fields.lowPsi.toDoubleOrNull()?.let { fields.pressureUnit.toPsi(it) } ?: return
        val staleAfterMs = fields.staleAfterMs.toLongOrNull() ?: return
        save(
            body = { api.updateTpmsConfig(lowPsi, staleAfterMs) },
            message = "TPMS config saved",
        ) { tpms ->
            { s -> s.copy(tpms = tpms, fields = s.fields.withTpms(tpms)) }
        }
    }

    /** Timezone-only sync: sends the SELECTED timezone (not the phone's zone —
     * a phone-derived string overwrote the gauge's real POSIX TZ and the picker
     * then flapped back to "Custom" on reload). */
    fun syncTime() {
        val tz = _state.value.fields.timezoneTz.ifBlank { Timezones.forDefault().posix }
        val offset = _state.value.fields.timezoneOffsetMinutes
        save(
            body = { api.syncTime(offset, tz) },
            message = "Timezone sent to gauge",
            errorMessage = "timezone sync failed",
        ) { status ->
            { s -> s.copy(fields = s.fields.copy(timezoneOffsetMinutes = status.timezoneOffsetMinutes, timezoneTz = tz)) }
        }
    }

    /** Apply a timezone selection locally only — the gauge is updated only when
     *  the user taps "Sync timezone to gauge" (previous immediate push caused
     *  double toasts on every picker tap). */
    fun applyTimezone(offsetMinutes: Int, timezoneTz: String) {
        val tz = timezoneTz.trim()
        if (tz.isEmpty()) {
            _state.update { it.copy(error = "Timezone string cannot be empty") }
            return
        }
        _state.update {
            it.copy(
                fields = it.fields.copy(
                    timezoneOffsetMinutes = offsetMinutes,
                    timezoneTz = tz,
                ),
                error = null,
                message = null,
            )
        }
    }

    /** Persist the TPMS BLE link toggle on its own (themes/config accepts partial patches). */
    fun saveTpmsBle() {
        val enabled = _state.value.fields.tpmsBle
        save(
            body = { api.updateThemesConfig(buildJsonObject { put("tpmsBle", enabled) }) },
            message = if (enabled) "TPMS BLE link enabled" else "TPMS BLE link disabled",
        ) { themes ->
            { s -> s.copy(themes = themes, fields = s.fields.withThemes(themes)) }
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

    /** Forget the saved gauge: disconnect + erase the persisted peer. */
    fun forgetSavedGauge() {
        viewModelScope.launch {
            runCatching { forgetTransport() }
            _state.update { it.copy(message = null, error = null) }
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

    fun refreshWifi() {
        viewModelScope.launch {
            runCatching { api.getNetworkStatus() }.onSuccess { net ->
                _state.update { it.copy(networkStatus = net) }
            }.onFailure { e -> _state.update { it.copy(error = e.message ?: "wifi status failed") } }
        }
    }

    fun scanWifi() {
        viewModelScope.launch {
            _state.update { it.copy(scanningWifi = true, error = null) }
            // One retry: the gauge's first scan after connect can return an
            // empty list while the radio settles.
            var result: com.boostgauge.app.data.api.WifiScanPayload? = null
            var failed = false
            for (attempt in 0..1) {
                runCatching { api.scanWifi() }
                    .onSuccess { scanned ->
                        result = scanned
                    }
                    .onFailure { e ->
                        failed = true
                        _state.update { it.copy(scanningWifi = false, error = e.message ?: "scan failed") }
                    }
                if (failed) break
                if (result?.networks?.isNotEmpty() == true) break
                if (attempt == 0) kotlinx.coroutines.delay(700)
            }
            if (!failed) {
                _state.update { it.copy(scanningWifi = false, wifiNetworks = result?.networks ?: emptyList()) }
            }
        }
    }

    fun saveWifi() {
        val ssid = _state.value.wifiSsid.trim()
        if (ssid.isBlank()) { _state.update { it.copy(error = "SSID required") }; return }
        val password = _state.value.wifiPassword.takeIf { it.isNotBlank() }
        save(
            body = { api.updateNetwork(ssid, password) },
            message = "Wi-Fi saved",
        ) { net ->
            { s -> s.copy(networkStatus = net, wifiPassword = "") }
        }
    }

    /** Sends the Wi-Fi the PHONE is on to the gauge (SSID only; the gauge
     *  reuses the stored PSK for that SSID if it has one, else errors). */
    fun usePhoneWifi() {
        val context = contextProvider ?: return
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            val ssid = runCatching {
                val wm = context.getSystemService(android.content.Context.WIFI_SERVICE) as android.net.wifi.WifiManager
                wm.connectionInfo?.ssid?.removeSurrounding("\"") ?: ""
            }.getOrDefault("")
            if (ssid.isBlank() || ssid == "<unknown ssid>") {
                _state.update { it.copy(saving = false, error = "Could not read this phone's Wi-Fi network (grant Location, then retry)") }
                return@launch
            }
            runCatching { api.usePhoneWifi(ssid) }
                .onSuccess { net ->
                    _state.update { it.copy(saving = false, networkStatus = net, message = "Gauge joining $ssid…") }
                }
                .onFailure { e ->
                    _state.update { it.copy(saving = false, error = e.message ?: "failed") }
                }
        }
    }

    fun deleteSavedWifi(ssid: String) {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching { api.deleteSavedNetwork(ssid) }.onSuccess { net ->
                _state.update { it.copy(saving = false, networkStatus = net, message = "Removed $ssid") }
            }.onFailure { e ->
                _state.update { it.copy(saving = false, error = e.message ?: "delete failed") }
            }
        }
    }

    fun reconnectWifi() {
        viewModelScope.launch {
            _state.update { it.copy(saving = true, error = null, message = null) }
            runCatching { api.reconnectNetwork() }.onSuccess { net ->
                _state.update { it.copy(saving = false, networkStatus = net, message = "Reconnecting…") }
            }.onFailure { e ->
                _state.update { it.copy(saving = false, error = e.message ?: "reconnect failed") }
            }
        }
    }

    fun updateWifiSsid(value: String) { _state.update { it.copy(wifiSsid = value) } }
    fun updateWifiPassword(value: String) { _state.update { it.copy(wifiPassword = value) } }

    fun clearMessage() {
        _state.update { it.copy(message = null, error = null) }
    }
}
