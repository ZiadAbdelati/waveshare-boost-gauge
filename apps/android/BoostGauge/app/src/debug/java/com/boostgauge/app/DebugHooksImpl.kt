package com.boostgauge.app

import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.SimBleTransport

/**
 * Debug-only provider (registered via META-INF/services in src/debug): enables
 * the in-process BLE simulator for emulator iteration and Connection-page
 * screenshots (Intent extra `transport=simBle`). Release builds omit this
 * class entirely, so the simulator is never compiled in.
 */
class DebugHooksImpl : DebugHooks {
    override val simTransportFactory: (() -> GaugeTransport)? = { SimBleTransport() }
}