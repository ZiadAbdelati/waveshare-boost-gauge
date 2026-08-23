package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.ThemeInfo
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class ThemesViewModel(private val api: GaugeApi) : ViewModel() {

    data class UiState(
        val loading: Boolean = true,
        val themes: List<ThemeInfo> = emptyList(),
        val activeThemeId: String = "",
        val activatingId: String? = null,
        val error: String? = null,
    )

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    init {
        load()
    }

    fun load() {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            runCatching { api.getThemes() }
                .onSuccess { payload ->
                    _state.value = UiState(
                        loading = false,
                        themes = payload.themes,
                        activeThemeId = payload.activeThemeId,
                        error = null,
                    )
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(loading = false, error = e.message ?: "failed to load themes")
                    }
                }
        }
    }

    fun activate(id: String) {
        viewModelScope.launch {
            _state.update { it.copy(activatingId = id, error = null) }
            runCatching { api.activateTheme(id) }
                .onSuccess { payload ->
                    _state.value = _state.value.copy(
                        activatingId = null,
                        themes = payload.themes,
                        activeThemeId = payload.activeThemeId,
                        error = null,
                    )
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(
                            activatingId = null,
                            error = e.message ?: "failed to activate theme",
                        )
                    }
                }
        }
    }
}
