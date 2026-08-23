package com.boostgauge.app.data.transport

import android.content.Context
import com.boostgauge.app.data.settings.SettingsStore
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first

typealias TransportFactory = (type: TransportType, address: String) -> GaugeTransport

/** Owns exactly one active transport and persists the selection in DataStore. */
class TransportController(
    private val context: Context,
    private val settingsStore: SettingsStore,
    private val transportFactory: TransportFactory = { type, address ->
        when (type) {
            TransportType.HTTP -> HttpTransport(address)
            TransportType.BLE -> BleTransport(context, address)
        }
    },
) {
    private val _selection = MutableStateFlow(
        TransportSelection(TransportType.HTTP, SettingsStore.DEFAULT_HTTP_ADDRESS, "")
    )
    val selection: StateFlow<TransportSelection> = _selection.asStateFlow()

    private val _transport = MutableStateFlow<GaugeTransport?>(null)
    val transport: StateFlow<GaugeTransport?> = _transport.asStateFlow()

    suspend fun restore() {
        val saved = settingsStore.selection.first()
        val address = if (saved.type == TransportType.HTTP) saved.httpAddress else saved.bleAddress
        select(saved.type, address, persist = false)
    }

    suspend fun select(type: TransportType, address: String, persist: Boolean = true) {
        val old = _transport.value
        runCatching { old?.close() }
        val new = transportFactory(type, address)
        _selection.value = TransportSelection(
            type = type,
            httpAddress = if (type == TransportType.HTTP) {
                address.ifBlank { SettingsStore.DEFAULT_HTTP_ADDRESS }
            } else {
                _selection.value.httpAddress
            },
            bleAddress = if (type == TransportType.BLE) address else _selection.value.bleAddress,
        )
        _transport.value = new
        if (persist) {
            settingsStore.setTransport(type, address)
        }
    }

    fun current(): GaugeTransport =
        checkNotNull(_transport.value) { "no transport selected" }
}
