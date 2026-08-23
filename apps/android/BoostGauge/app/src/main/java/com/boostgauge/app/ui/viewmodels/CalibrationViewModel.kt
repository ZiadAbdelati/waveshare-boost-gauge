package com.boostgauge.app.ui.viewmodels

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.boostgauge.app.data.api.Calibration
import com.boostgauge.app.data.api.GaugeApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch

class CalibrationViewModel(private val api: GaugeApi) : ViewModel() {

    /** Mutually exclusive, exhaustive UI branches for the Calibrate tab. */
    enum class UiMode { LOADING, CONTENT, ERROR, EMPTY }

    data class UiState(
        val loading: Boolean = true,
        val calibration: Calibration? = null,
        val calibrating: Boolean = false,
        val error: String? = null,
        val message: String? = null,
    ) {
        /**
         * The UI must render exactly one branch: a successful load always
         * produces CONTENT (live diagnostics + saved calibration + button),
         * a failed load produces ERROR, and an untouched load produces EMPTY.
         * Loading only hides the content while nothing has been loaded yet so
         * a refresh never blanks an already-visible calibration.
         */
        fun mode(): UiMode = when {
            loading && calibration == null -> UiMode.LOADING
            calibration != null -> UiMode.CONTENT
            error != null -> UiMode.ERROR
            else -> UiMode.EMPTY
        }
    }

    private val _state = MutableStateFlow(UiState())
    val state: StateFlow<UiState> = _state.asStateFlow()

    init {
        load()
    }

    fun load() {
        viewModelScope.launch {
            _state.update { it.copy(loading = true, error = null) }
            runCatching { api.getCalibration() }
                .onSuccess { calibration ->
                    _state.update { it.copy(loading = false, calibration = calibration) }
                }
                .onFailure { e ->
                    _state.update { it.copy(loading = false, error = e.message ?: "failed to load calibration") }
                }
        }
    }

    fun calibrate() {
        viewModelScope.launch {
            _state.update { it.copy(calibrating = true, error = null, message = null) }
            runCatching { api.calibrateAtmosphere() }
                .onSuccess { calibration ->
                    _state.update {
                        it.copy(calibrating = false, calibration = calibration, message = "Calibration stored")
                    }
                }
                .onFailure { e ->
                    _state.update {
                        it.copy(calibrating = false, error = e.message ?: "calibration failed")
                    }
                }
        }
    }
}
