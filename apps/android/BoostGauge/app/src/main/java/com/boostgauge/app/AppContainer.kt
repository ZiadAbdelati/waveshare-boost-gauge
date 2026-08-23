package com.boostgauge.app

import android.content.Context
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.settings.SettingsStore
import com.boostgauge.app.data.transport.TransportController

/** Manual dependency container; one active transport at a time. */
class AppContainer(appContext: Context) {
    val settingsStore = SettingsStore(appContext)
    val transportController = TransportController(appContext, settingsStore)
    val api = GaugeApi { transportController.current() }
    val repository = GaugeRepository(api, transportController.transport)

    /** Restores the persisted transport selection before the live loop starts. */
    suspend fun initialize() {
        transportController.restore()
    }
}
