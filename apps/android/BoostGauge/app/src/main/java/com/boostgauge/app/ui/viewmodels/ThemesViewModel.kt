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
        /** User color edits only (keyed themeId → key → hex). Never pre-filled
         *  from the server; every entry here is sent verbatim by saveOptions. */
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

    /**
     * Server-side color baseline per theme (keyed themeId → key → hex), filled
     * from every themes payload the VM accepts. This is what colorHex falls
     * back to and what saveOptions must NOT re-send: the firmware seeds colors
     * from current values and overwrites only the named keys, so a partial
     * `colors` object is always correct. Keeping server colors out of
     * themeColorEdits is what stops stale palette keys from riding along with
     * a neon preset change (field report 2026-09-09).
     */
    private val serverColorBaselines = MutableStateFlow<Map<String, Map<String, String>>>(emptyMap())

    /** Flattens a theme's server colors into the baseline map shape. */
    private fun baselineColorsOf(theme: ThemeInfo): Map<String, String> = buildMap {
        if (theme.colors.face.isNotBlank()) put("face", theme.colors.face)
        if (theme.colors.track.isNotBlank()) put("track", theme.colors.track)
        if (theme.colors.text.isNotBlank()) put("text", theme.colors.text)
        if (theme.colors.muted.isNotBlank()) put("muted", theme.colors.muted)
        if (theme.colors.vacuum.isNotBlank()) put("vacuum", theme.colors.vacuum)
        if (theme.colors.boost.isNotBlank()) put("boost", theme.colors.boost)
        if (theme.colors.overboost.isNotBlank()) put("overboost", theme.colors.overboost)
        if (theme.colors.zero.isNotBlank()) put("zero", theme.colors.zero)
    }

    /**
     * Refreshes baselines from a response payload so the colorHex fallback
     * always reads the board's latest colors (same values the old per-key
     * ThemeInfo probe returned). Optionally clears user edits: ALL of them
     * after a full load (the board is authoritative; unapplied edits are
     * dropped with it), or one theme's after its save/reset echo (the edits
     * became server state). Other echoes (activate/resync/neon field PUT)
     * keep pending edits untouched.
     */
    private fun adoptBaselines(
        payload: ThemesPayload,
        clearAllEdits: Boolean = false,
        clearThemeId: String? = null,
    ) {
        val next = serverColorBaselines.value.toMutableMap()
        for (theme in payload.themes) {
            next[theme.id] = baselineColorsOf(theme)
        }
        serverColorBaselines.value = next
        if (!clearAllEdits && clearThemeId == null) return
        _state.update { prev ->
            val clearedEdits = if (clearAllEdits) {
                emptyMap()
            } else {
                prev.themeColorEdits.toMutableMap().apply { remove(clearThemeId) }
            }
            prev.copy(themeColorEdits = clearedEdits)
        }
    }

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

    /**
     * Board is authoritative: adopt its activeThemeId if it moved (reconnect,
     * tab re-entry, or a foreground resync). Skipped while the initial load is
     * in flight so a re-entry never races it; seq-guarded so a resync that
     * started before a newer activation cannot clobber the newer selection.
     */
    internal fun resyncActiveTheme() {
        if (_state.value.loading) return
        val seq = ++requestSeq
        viewModelScope.launch {
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.getThemes() }.getOrNull()
            } ?: return@launch
            // A newer load/activation superseded this resync while it was in
            // flight: drop it so it cannot apply a stale activeThemeId.
            if (seq != requestSeq) return@launch
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
        val seq = ++requestSeq
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
            if (seq != requestSeq) {
                // A newer load/activation/resync superseded this one: the stale
                // response (captured before the board processed the newer
                // selection) must not clobber the newer state. Always clear the
                // loading flag so a superseded refresh cannot wedge the editor.
                _state.update { it.copy(loading = false) }
                return@launch
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
        // Adopt the board's colors as the baseline and clear user edits for
        // every theme: a fresh load is authoritative, any pending edit the
        // user had not applied is dropped with it.
        adoptBaselines(payload, clearAllEdits = true)
        _state.update { prev ->
            prev.copy(
                loading = false,
                themes = payload.themes,
                activeThemeId = payload.activeThemeId,
                error = null,
                payload = payload,
                config = config,
                status = status,
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
     * Monotonic guard shared by every list-apply path (load/resync/activate).
     * The activation echoes carry their own seq, but a load()/resync() apply
     * carries none — a slow response captured before the board processed a
     * newer selection could otherwise clobber it. Each request captures the
     * current value and applies only while it is still the newest.
     */
    private var requestSeq = 0

    /**
     * Theme activation is bounded: a half-dead BLE link (e.g. a request racing
     * the reconnect loop) could otherwise hold the request for the transport's
     * full retry ladder (~20 s × 5) and pin the row spinner for 30 s+.
     */
    fun activate(id: String) {
        val activationSeq = ++this.activationSeq
        ++requestSeq
        viewModelScope.launch {
            _state.update { it.copy(activatingId = id, activatingSeq = activationSeq, error = null) }
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.activateTheme(id) }
            }
            // Rapid-fire activations complete out of order. A response is applied
            // only while this activation is still the newest requested one: a late
            // response for a superseded (or already-cleared) activation must never
            // overwrite a newer selection — including the activatingId == null
            // window, which the old guard wrongly treated as "accept anything".
            val isCurrent = state.value.activatingSeq == activationSeq && state.value.activatingId == id
            when {
                result == null -> if (isCurrent) {
                    _state.update { it.copy(activatingId = null, error = "theme request timed out") }
                    // The PUT may still have reached the board even though the
                    // echo was lost: reconcile with a fresh GET instead of
                    // leaving the UI frozen on the old theme.
                    reconcileAfterLostEcho(id, activationSeq)
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
                else -> if (isCurrent) {
                    _state.update {
                        it.copy(
                            activatingId = null,
                            error = result.exceptionOrNull()?.message ?: "failed to activate theme",
                        )
                    }
                    reconcileAfterLostEcho(id, activationSeq)
                }
            }
        }
    }

    /**
     * A select PUT's echo can be lost on a half-dead BLE link even though the
     * board applied the switch. Reconcile with a fresh GET: if the board now
     * reports the requested theme, the operation actually succeeded — adopt it
     * and clear the failure. If it is still on the old theme, keep the failure
     * but adopt the board's real state so the row never lies.
     */
    private fun reconcileAfterLostEcho(requestedId: String, activationSeq: Int) {
        viewModelScope.launch {
            val board = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.getThemes() }.getOrNull()
            } ?: return@launch
            // Only reconcile while this activation is still the newest requested
            // one; a newer tap wins regardless.
            if (state.value.activatingSeq != activationSeq) return@launch
            val applied = board.activeThemeId == requestedId
            _state.update {
                it.copy(
                    activatingId = null,
                    themes = board.themes,
                    activeThemeId = board.activeThemeId,
                    payload = board,
                    error = if (applied) null else it.error,
                )
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
        // Fallback chain: user edits first, then the server baseline (kept in
        // sync with every accepted themes payload). Same values the old
        // per-key ThemeInfo probe returned — the baseline is blank-filtered
        // by construction.
        return serverColorBaselines.value[theme.id]?.get(key)
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

    /**
     * Neon controls apply each change immediately with a single-field PUT
     * (web parity — the web UI puts each neon control on its own
     * `PUT /api/v1/themes/config`). The firmware seeds colors from the
     * current values and overwrites only the named keys, so a one-field body
     * never repaints a stale palette — the exact bug the old
     * colors-always-attached saveOptions had.
     */
    private fun putNeonField(key: String, value: JsonPrimitive) {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            val reqObj = buildJsonObject { put(key, value) }
            val result = withTimeoutOrNull(THEME_OP_TIMEOUT_MS) {
                runCatching { api.updateThemesConfig(reqObj) }
            }
            when {
                result == null -> _state.update {
                    it.copy(loading = false, error = "theme options request timed out")
                }
                result.isSuccess -> {
                    val payload = result.getOrThrow()
                    adoptBaselines(payload)
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

    fun setNeonPreset(value: Int) {
        if (value !in 0..3) return
        _state.update { it.copy(neonPreset = value) }
        putNeonField("neonPreset", JsonPrimitive(value))
    }

    fun setNeonLayout(value: Int) {
        if (value !in 0..2) return
        _state.update { it.copy(neonLayout = value) }
        putNeonField("neonLayout", JsonPrimitive(value))
    }

    fun setNeonFont(value: Int) {
        if (value !in 0..1) return
        _state.update { it.copy(neonFont = value) }
        putNeonField("neonFont", JsonPrimitive(value))
    }

    fun setNeonMarqueeSpin(value: Boolean) {
        _state.update { it.copy(neonMarqueeSpin = value) }
        putNeonField("neonMarqueeSpin", JsonPrimitive(value))
    }

    fun saveOptions(themeId: String) {
        viewModelScope.launch {
            val cur = _state.value
            _state.update { it.copy(loading = true, error = null) }
            // Only user edits go out, only under their own keys. The firmware
            // seeds colors from the current stored values before applying the
            // patch, so a partial `colors` object can never repaint a zone
            // with a value the user never touched (the old behavior
            // pre-filled edits from the server echo and always attached all
            // of them — a preset change then re-sent the old palette over it).
            val editedColors = cur.themeColorEdits[themeId]
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
                if (!editedColors.isNullOrEmpty()) {
                    putJsonObject("colors") {
                        for ((key, hex) in editedColors) {
                            put(key, hex)
                        }
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
                    // The save echo is authoritative for the saved theme:
                    // reseed its baseline and drop its user edits (they are
                    // now server state).
                    adoptBaselines(payload, clearThemeId = themeId)
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
                    // The reset echo is authoritative for the reset theme:
                    // reseed its baseline and drop its user edits.
                    adoptBaselines(payload, clearThemeId = themeId)
                    _state.update {
                        it.copy(
                            loading = false,
                            themes = payload.themes,
                            activeThemeId = payload.activeThemeId,
                            payload = payload,
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
