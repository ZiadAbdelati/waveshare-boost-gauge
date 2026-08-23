package com.boostgauge.app.data.settings

import android.content.Context
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map

private val Context.dataStore by preferencesDataStore(name = "boost_gauge_settings")

enum class TransportType { HTTP, BLE }

data class TransportSelection(
    val type: TransportType,
    val httpAddress: String,
    val bleAddress: String,
)

/** Persists the last transport and per-transport address (one transport at a time). */
class SettingsStore(private val context: Context) {

    val selection: Flow<TransportSelection> = context.dataStore.data.map { prefs ->
        TransportSelection(
            type = runCatching { TransportType.valueOf(prefs[KEY_TRANSPORT] ?: "") }
                .getOrDefault(TransportType.HTTP),
            httpAddress = prefs[KEY_HTTP_ADDRESS] ?: DEFAULT_HTTP_ADDRESS,
            bleAddress = prefs[KEY_BLE_ADDRESS] ?: "",
        )
    }

    suspend fun setTransport(type: TransportType, address: String) {
        context.dataStore.edit { prefs ->
            prefs[KEY_TRANSPORT] = type.name
            when (type) {
                TransportType.HTTP -> prefs[KEY_HTTP_ADDRESS] = address
                TransportType.BLE -> prefs[KEY_BLE_ADDRESS] = address
            }
        }
    }

    companion object {
        const val DEFAULT_HTTP_ADDRESS = "192.168.4.1"
        private val KEY_TRANSPORT = stringPreferencesKey("transport_type")
        private val KEY_HTTP_ADDRESS = stringPreferencesKey("http_address")
        private val KEY_BLE_ADDRESS = stringPreferencesKey("ble_address")
    }
}
