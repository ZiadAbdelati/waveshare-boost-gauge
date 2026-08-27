package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.ThemesPayload
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class StatusViewModel(
    private val repository: GaugeRepository,
    private val api: GaugeApi,
) : ViewModel() {

    val status: StateFlow<com.boostgauge.app.data.api.Status?> = repository.status
    val connectionStatus: StateFlow<ConnectionStatus> = repository.connectionStatus
    val reconnectAttempt: StateFlow<Int?> = repository.reconnectAttempt
    val lastError: StateFlow<String?> = repository.lastError

    private val _themeNames = MutableStateFlow<Map<String, String>>(emptyMap())
    val themeNames: StateFlow<Map<String, String>> = _themeNames.asStateFlow()
    private val _config = MutableStateFlow<Config?>(null)
    val config: StateFlow<Config?> = _config.asStateFlow()
    private val _themes = MutableStateFlow<ThemesPayload?>(null)
    val themes: StateFlow<ThemesPayload?> = _themes.asStateFlow()

    init {
        viewModelScope.launch {
            runCatching { api.getThemes() }
                .getOrNull()
                ?.let { payload ->
                    _themes.value = payload
                    _themeNames.value = payload.themes.associate { it.id to it.name }
                }
            _config.value = runCatching { api.getConfig() }.getOrNull()
        }
    }

    fun refresh() {
        viewModelScope.launch {
            repository.refresh()
            _config.value = runCatching { api.getConfig() }.getOrNull() ?: _config.value
            runCatching { api.getThemes() }.getOrNull()?.let { payload ->
                _themes.value = payload
                _themeNames.value = payload.themes.associate { it.id to it.name }
            }
        }
    }
}
