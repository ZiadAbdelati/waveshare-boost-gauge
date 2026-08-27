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
    val bleName: String = "",
)

interface TransportSettingsStore {
    val selection: Flow<TransportSelection>
    suspend fun setTransport(type: TransportType, address: String, name: String? = null)
}

/** Persists the last transport and per-transport address (one transport at a time). */
class SettingsStore(private val context: Context) : TransportSettingsStore {

    override val selection: Flow<TransportSelection> = context.dataStore.data.map { prefs ->
        TransportSelection(
            type = runCatching { TransportType.valueOf(prefs[KEY_TRANSPORT] ?: "") }
                .getOrDefault(TransportType.BLE),
            httpAddress = prefs[KEY_HTTP_ADDRESS] ?: "",
            bleAddress = prefs[KEY_BLE_ADDRESS] ?: "",
            bleName = prefs[KEY_BLE_NAME] ?: "",
        )
    }

    override suspend fun setTransport(type: TransportType, address: String, name: String?) {
        context.dataStore.edit { prefs ->
            prefs[KEY_TRANSPORT] = type.name
            when (type) {
                TransportType.HTTP -> prefs[KEY_HTTP_ADDRESS] = address
                TransportType.BLE -> {
                    prefs[KEY_BLE_ADDRESS] = address
                    if (!name.isNullOrBlank()) prefs[KEY_BLE_NAME] = name
                }
            }
        }
    }

    companion object {
        // Retained only for log-fetch URL construction from device-info IP;
        // never used as a persisted default or user-visible address.
        private val KEY_TRANSPORT = stringPreferencesKey("transport_type")
        private val KEY_HTTP_ADDRESS = stringPreferencesKey("http_address")
        private val KEY_BLE_ADDRESS = stringPreferencesKey("ble_address")
        private val KEY_BLE_NAME = stringPreferencesKey("ble_name")
    }
}
