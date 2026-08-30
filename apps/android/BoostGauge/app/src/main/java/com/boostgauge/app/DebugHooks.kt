package com.boostgauge.app

import com.boostgauge.app.data.transport.GaugeTransport

/**
 * Build-type hook for the emulator-only in-process BLE simulator. A provider
 * is registered via META-INF/services only in src/debug; release has no
 * provider, so [simTransportFactory] is null and the `transport=simBle`
 * launch extra degrades to the normal restore() path.
 */
interface DebugHooks {
    val simTransportFactory: (() -> GaugeTransport)?

    companion object {
        /** Debug builds register DebugHooksImpl; release resolves none. */
        fun load(): DebugHooks? =
            java.util.ServiceLoader.load(DebugHooks::class.java, DebugHooks::class.java.classLoader)
                .firstOrNull()
    }
}