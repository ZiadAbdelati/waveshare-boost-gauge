package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

class StatusViewModel(
    private val repository: GaugeRepository,
    private val api: GaugeApi,
) : ViewModel() {

    val status: StateFlow<com.boostgauge.app.data.api.Status?> = repository.status
    val connected: StateFlow<Boolean> = repository.connected
    val lastError: StateFlow<String?> = repository.lastError

    private val _themeNames = MutableStateFlow<Map<String, String>>(emptyMap())
    val themeNames: StateFlow<Map<String, String>> = _themeNames.asStateFlow()

    init {
        viewModelScope.launch {
            runCatching { api.getThemes() }
                .getOrNull()
                ?.let { payload ->
                    _themeNames.value = payload.themes.associate { it.id to it.name }
                }
        }
    }

    fun refresh() {
        viewModelScope.launch { repository.refresh() }
    }
}
