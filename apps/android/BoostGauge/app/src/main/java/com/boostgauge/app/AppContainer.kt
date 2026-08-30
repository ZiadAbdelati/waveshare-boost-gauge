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

    /**
     * Restores the persisted transport selection before the live loop starts.
     * [simBle] (Intent extra `transport=simBle`) swaps in the in-process BLE
     * simulator for emulator iteration; it never persists. The simulator only
     * exists in debug builds (DebugHooks), so release degrades to restore().
     */
    suspend fun initialize(simBle: Boolean = false) {
        val factory = if (simBle) DebugHooks.load()?.simTransportFactory else null
        if (factory != null) transportController.useSimBle(factory) else transportController.restore()
    }
}
