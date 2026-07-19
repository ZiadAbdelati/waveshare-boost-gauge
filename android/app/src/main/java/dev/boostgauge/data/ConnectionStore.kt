package dev.boostgauge.data

import android.content.Context

class ConnectionStore(context: Context) {
    private val prefs = context.getSharedPreferences("boost-gauge", Context.MODE_PRIVATE)

    fun loadBaseUrl(): String = prefs.getString(KEY_BASE_URL, null).orEmpty()

    fun saveBaseUrl(url: String) {
        prefs.edit().putString(KEY_BASE_URL, url).apply()
    }

    private companion object {
        const val KEY_BASE_URL = "baseUrl"
    }
}
