package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.ThemeInfo
import com.boostgauge.app.data.api.ThemesPayload
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.async
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import kotlinx.serialization.json.putJsonObject

class ThemesViewModel(
    private val api: GaugeApi,
    connectionStatus: StateFlow<ConnectionStatus> = MutableStateFlow(ConnectionStatus.Connected),
) : ViewModel() {

    data class UiState(
        val loading: Boolean = true,
        val themes: List<ThemeInfo> = emptyList(),
        val activeThemeId: String = "",
        val activatingId: String? = null,
        val activatingSeq: Int = 0,
        val error: String? = null,
        val payload: ThemesPayload? = null,
        val config: Config? = null,
        val status: Status? = null,
        val themeColorEdits: Map<String, Map<String, String>> = emptyMap(),
        val arcGradient: Boolean = false,
        val hudGradient: Boolean = false,
        val hudTrueBlack: Boolean = false,
        val bigDigitStaticBg: Boolean = false,
        val bigDigitColorText: Boolean = false,
        val bigDigitStaticColor: String = "#000000",
        val bigDigitTextColor: String = "#ffffff",
        val vaultFace: String = "",
        val vaultVignette: Int = 0,
        val vaultNeedleRed: Boolean = false,
        val vaultNeedleTail: Boolean = false,
        val neonLayout: Int = 0,
        val neonFont: Int = 0,
        val neonPreset: Int = 0,
        val neonMarqueeSpin: Boolean = false,
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    /** Concurrent load result: themes (mandatory) + best-effort config/status. */
    private data class Loaded(
        val themes: ThemesPayload,
        val config: Config?,
        val status: Status?,
    )

    init {
        load()
        // Transport loss must never leave the previous live /state on the
        // preview: hide the stale gauge preview (status/config) so the Themes
        // tab shows the palette list, never frozen live values. The theme list
        // itself is not a live payload and stays cached. When the link comes
        // back the preview is restored, so it never stays collapsed after a
        // transient reconnect.
        viewModelScope.launch {
            connectionStatus.collect { status ->
                if (status == ConnectionStatus.Connected) {
                    // Tab re-entry / reconnect: the board may have had its theme
                    // changed elsewhere (other phone, web UI). Re-sync the list
                    // and preview so the tab never shows a stale active theme.
                    refreshPreview()
                    if (_state.value.activeThemeId.isNotBlank()) resyncActiveTheme()
                } else if (_state.value.status != null || _state.value.config != null) {
                    _state.update { it.copy(status = null, config = null) }
                }
            }
        }
    }

    /** Board is authoritative: adopt its activeThemeId if it moved. */
    private fun resyncActiveTheme() {
        viewModelScope.launch {
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.getThemes() }.getOrNull()
            } ?: return@launch
            _state.update {
                it.copy(
                    themes = result.themes,
                    activeThemeId = result.activeThemeId,
                    payload = result,
                )
            }
        }
    }

    private fun refreshPreview() {
        viewModelScope.launch {
            val config = runCatching { api.getConfig() }.getOrNull()
            val status = runCatching { api.getState() }.getOrNull()
            if (config != null && status != null) {
                _state.update { it.copy(config = config, status = status) }
            }
        }
    }

    /** Parallel load: themes/config/status fetched concurrently so a slow themes
     *  round trip never serializes the smaller config/state reads behind it. */
    fun load() {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            val result = withTimeoutOrNull(THEMES_LOAD_TIMEOUT_MS) {
                runCatching {
                    coroutineScope {
                        val themes = async { api.getThemes() }
                        val config = async { runCatching { api.getConfig() }.getOrNull() }
                        val status = async { runCatching { api.getState() }.getOrNull() }
                        Loaded(themes.await(), config.await(), status.await())
                    }
                }
            }
            when {
                result == null -> _state.update { it.copy(loading = false, error = "themes load timed out") }
                result.isSuccess -> applyLoaded(result.getOrThrow())
                else -> _state.update {
                    it.copy(loading = false, error = result.exceptionOrNull()?.message ?: "failed to load themes")
                }
            }
        }
    }

    private fun applyLoaded(loaded: Loaded) {
        val payload = loaded.themes
        val config = loaded.config
        val status = loaded.status
        _state.update { prev ->
            val initialColors = mutableMapOf<String, Map<String, String>>()
            for (theme in payload.themes) {
                val colors = mutableMapOf<String, String>()
                if (theme.colors.face.isNotBlank()) colors["face"] = theme.colors.face
                if (theme.colors.track.isNotBlank()) colors["track"] = theme.colors.track
                if (theme.colors.text.isNotBlank()) colors["text"] = theme.colors.text
                if (theme.colors.muted.isNotBlank()) colors["muted"] = theme.colors.muted
                if (theme.colors.vacuum.isNotBlank()) colors["vacuum"] = theme.colors.vacuum
                if (theme.colors.boost.isNotBlank()) colors["boost"] = theme.colors.boost
                if (theme.colors.overboost.isNotBlank()) colors["overboost"] = theme.colors.overboost
                if (theme.colors.zero.isNotBlank()) colors["zero"] = theme.colors.zero
                initialColors[theme.id] = colors
            }
            prev.copy(
                loading = false,
                themes = payload.themes,
                activeThemeId = payload.activeThemeId,
                error = null,
                payload = payload,
                config = config,
                status = status,
                themeColorEdits = initialColors,
                arcGradient = payload.arcGradient,
                hudGradient = payload.hudGradient,
                hudTrueBlack = payload.hudTrueBlack,
                bigDigitStaticBg = payload.bigDigitStaticBg,
                bigDigitColorText = payload.bigDigitColorText,
                bigDigitStaticColor = payload.bigDigitStaticColor.ifBlank { "#000000" },
                bigDigitTextColor = payload.bigDigitTextColor.ifBlank { "#ffffff" },
                vaultFace = payload.vaultFace,
                vaultVignette = payload.vaultVignette,
                vaultNeedleRed = payload.vaultNeedleRed,
                vaultNeedleTail = payload.vaultNeedleTail,
                neonLayout = payload.neonLayout,
                neonFont = payload.neonFont,
                neonPreset = payload.neonPreset,
                neonMarqueeSpin = payload.neonMarqueeSpin,
            )
        }
    }

    private var activationSeq = 0

    /**
     * Theme activation is bounded: a half-dead BLE link (e.g. a request racing
     * the reconnect loop) could otherwise hold the request for the transport's
     * full retry ladder (~20 s × 5) and pin the row spinner for 30 s+.
     */
    fun activate(id: String) {
        val seq = ++activationSeq
        viewModelScope.launch {
            _state.update { it.copy(activatingId = id, activatingSeq = seq, error = null) }
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.activateTheme(id) }
            }
            // Rapid-fire activations complete out of order. A response is applied
            // only while this activation is still the newest requested one: a late
            // response for a superseded (or already-cleared) activation must never
            // overwrite a newer selection — including the activatingId == null
            // window, which the old guard wrongly treated as "accept anything".
            val isCurrent = state.value.activatingSeq == seq && state.value.activatingId == id
            when {
                result == null -> if (isCurrent) _state.update {
                    it.copy(activatingId = null, error = "theme request timed out")
                }
                result.isSuccess -> {
                    val payload = result.getOrThrow()
                    if (isCurrent) {
                        _state.update {
                            it.copy(
                                activatingId = null,
                                themes = payload.themes,
                                activeThemeId = payload.activeThemeId,
                                error = null,
                                payload = payload,
                            )
                        }
                    }
                }
                else -> if (isCurrent) _state.update {
                    it.copy(
                        activatingId = null,
                        error = result.exceptionOrNull()?.message ?: "failed to activate theme",
                    )
                }
            }
        }
    }

    fun setColor(themeId: String, key: String, hex: String) {
        _state.update { prev ->
            val themeEdits = prev.themeColorEdits[themeId]?.toMutableMap() ?: mutableMapOf()
            themeEdits[key] = hex
            val updatedMap = prev.themeColorEdits.toMutableMap()
            updatedMap[themeId] = themeEdits
            prev.copy(themeColorEdits = updatedMap)
        }
    }

    fun colorHex(theme: ThemeInfo, key: String): String? {
        val edits = _state.value.themeColorEdits[theme.id]
        if (edits != null && edits.containsKey(key)) {
            return edits[key]
        }
        return when (key) {
            "face" -> theme.colors.face.takeIf { it.isNotBlank() }
            "track" -> theme.colors.track.takeIf { it.isNotBlank() }
            "text" -> theme.colors.text.takeIf { it.isNotBlank() }
            "muted" -> theme.colors.muted.takeIf { it.isNotBlank() }
            "vacuum" -> theme.colors.vacuum.takeIf { it.isNotBlank() }
            "boost" -> theme.colors.boost.takeIf { it.isNotBlank() }
            "overboost" -> theme.colors.overboost.takeIf { it.isNotBlank() }
            "zero" -> theme.colors.zero.takeIf { it.isNotBlank() }
            else -> null
        }
    }

    fun updateArcGradient(value: Boolean) { _state.update { it.copy(arcGradient = value) } }
    fun updateHudGradient(value: Boolean) { _state.update { it.copy(hudGradient = value) } }
    fun updateHudTrueBlack(value: Boolean) { _state.update { it.copy(hudTrueBlack = value) } }
    fun updateBigDigitStaticBg(value: Boolean) { _state.update { it.copy(bigDigitStaticBg = value) } }
    fun updateBigDigitColorText(value: Boolean) { _state.update { it.copy(bigDigitColorText = value) } }
    fun updateBigDigitStaticColor(value: String) { _state.update { it.copy(bigDigitStaticColor = value) } }
    fun updateBigDigitTextColor(value: String) { _state.update { it.copy(bigDigitTextColor = value) } }
    fun updateVaultFace(value: String) { _state.update { it.copy(vaultFace = value) } }
    fun updateVaultVignette(value: Int) { _state.update { it.copy(vaultVignette = value) } }
    fun updateVaultNeedleRed(value: Boolean) { _state.update { it.copy(vaultNeedleRed = value) } }
    fun updateVaultNeedleTail(value: Boolean) { _state.update { it.copy(vaultNeedleTail = value) } }
    fun updateNeonLayout(value: Int) { _state.update { it.copy(neonLayout = value) } }
    fun updateNeonFont(value: Int) { _state.update { it.copy(neonFont = value) } }
    fun updateNeonPreset(value: Int) { _state.update { it.copy(neonPreset = value) } }
    fun updateNeonMarqueeSpin(value: Boolean) { _state.update { it.copy(neonMarqueeSpin = value) } }

    fun saveOptions(themeId: String) {
        viewModelScope.launch {
            val cur = _state.value
            _state.update { it.copy(loading = true, error = null) }
            val editable = cur.themeColorEdits[themeId] ?: emptyMap()
            val reqObj = buildJsonObject {
                put("id", themeId)
                when (themeId) {
                    "dyno-cell" -> {
                        put("arcGradient", cur.arcGradient)
                    }
                    "vault-tec" -> {
                        put("vaultFace", cur.vaultFace)
                        put("vaultVignette", cur.vaultVignette)
                        put("vaultNeedleRed", cur.vaultNeedleRed)
                        put("vaultNeedleTail", cur.vaultNeedleTail)
                    }
                    "night-city" -> {
                        put("hudGradient", cur.hudGradient)
                        put("hudTrueBlack", cur.hudTrueBlack)
                    }
                    "big-digit" -> {
                        put("bigDigitStaticBg", cur.bigDigitStaticBg)
                        put("bigDigitColorText", cur.bigDigitColorText)
                        put("bigDigitStaticColor", cur.bigDigitStaticColor)
                        put("bigDigitTextColor", cur.bigDigitTextColor)
                    }
                    "neon" -> {
                        put("neonLayout", cur.neonLayout)
                        put("neonFont", cur.neonFont)
                        put("neonPreset", cur.neonPreset)
                        put("neonMarqueeSpin", cur.neonMarqueeSpin)
                    }
                }
                if (editable.isNotEmpty()) {
                    putJsonObject("colors") {
                        editable["vacuum"]?.let { put("vacuum", it) }
                        editable["boost"]?.let { put("boost", it) }
                        editable["overboost"]?.let { put("overboost", it) }
                    }
                }
            }

            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.updateThemesConfig(reqObj) }
            }
            when {
                result == null -> _state.update {
                    it.copy(loading = false, error = "theme options request timed out")
                }
                result.isSuccess -> {
                    val payload = result.getOrThrow()
                    _state.update {
                        it.copy(
                            loading = false,
                            themes = payload.themes,
                            activeThemeId = payload.activeThemeId,
                            payload = payload,
                            arcGradient = payload.arcGradient,
                            hudGradient = payload.hudGradient,
                            hudTrueBlack = payload.hudTrueBlack,
                            bigDigitStaticBg = payload.bigDigitStaticBg,
                            bigDigitColorText = payload.bigDigitColorText,
                            bigDigitStaticColor = payload.bigDigitStaticColor.ifBlank { "#000000" },
                            bigDigitTextColor = payload.bigDigitTextColor.ifBlank { "#ffffff" },
                            vaultFace = payload.vaultFace,
                            vaultVignette = payload.vaultVignette,
                            vaultNeedleRed = payload.vaultNeedleRed,
                            vaultNeedleTail = payload.vaultNeedleTail,
                            neonLayout = payload.neonLayout,
                            neonFont = payload.neonFont,
                            neonPreset = payload.neonPreset,
                            neonMarqueeSpin = payload.neonMarqueeSpin,
                        )
                    }
                }
                else -> _state.update {
                    it.copy(
                        loading = false,
                        error = result.exceptionOrNull()?.message ?: "failed to save theme options",
                    )
                }
            }
        }
    }

    fun resetColors(themeId: String) {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching {
                    api.updateThemesConfig(buildJsonObject {
                        put("id", themeId)
                        put("reset", true)
                    })
                }
            }
            when {
                result == null -> _state.update {
                    it.copy(loading = false, error = "theme reset timed out")
                }
                result.isSuccess -> {
                    val payload = result.getOrThrow()
                    _state.update {
                        val updatedColors = it.themeColorEdits.toMutableMap()
                        val targetTheme = payload.themes.firstOrNull { t -> t.id == themeId }
                        if (targetTheme != null) {
                            val colors = mutableMapOf<String, String>()
                            if (targetTheme.colors.face.isNotBlank()) colors["face"] = targetTheme.colors.face
                            if (targetTheme.colors.track.isNotBlank()) colors["track"] = targetTheme.colors.track
                            if (targetTheme.colors.text.isNotBlank()) colors["text"] = targetTheme.colors.text
                            if (targetTheme.colors.muted.isNotBlank()) colors["muted"] = targetTheme.colors.muted
                            if (targetTheme.colors.vacuum.isNotBlank()) colors["vacuum"] = targetTheme.colors.vacuum
                            if (targetTheme.colors.boost.isNotBlank()) colors["boost"] = targetTheme.colors.boost
                            if (targetTheme.colors.overboost.isNotBlank()) colors["overboost"] = targetTheme.colors.overboost
                            if (targetTheme.colors.zero.isNotBlank()) colors["zero"] = targetTheme.colors.zero
                            updatedColors[themeId] = colors
                        }
                        it.copy(
                            loading = false,
                            themes = payload.themes,
                            activeThemeId = payload.activeThemeId,
                            payload = payload,
                            themeColorEdits = updatedColors,
                        )
                    }
                }
                else -> _state.update {
                    it.copy(
                        loading = false,
                        error = result.exceptionOrNull()?.message ?: "failed to reset theme colors",
                    )
                }
            }
        }
    }

    companion object {
        val paletteKeys = listOf("face", "track", "text", "muted", "vacuum", "boost", "overboost", "zero")
        val zoneKeys = listOf("vacuum", "boost", "overboost")

        /**
         * Bound on a single theme operation (activate/save/reset). A dead BLE
         * link can otherwise hold the request for the transport's full retry
         * ladder (~20 s × 5 ≈ 100 s) and pin the row spinner for 30 s+.
         */
        const val THEME_OP_TIMEOUT_MS = 10_000L

        /** Bound on the full themes-page load (list + config + status). */
        const val THEMES_LOAD_TIMEOUT_MS = 15_000L
    }
}
