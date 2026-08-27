package com.boostgauge.app.data.transport

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportSettingsStore
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.service.ForegroundServiceLauncher
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

private class FakeSettingsStore(initial: TransportSelection) : TransportSettingsStore {
    private val _selection = MutableStateFlow(initial)
    override val selection: Flow<TransportSelection> = _selection
    val persisted = mutableListOf<TransportSelection>()
    override suspend fun setTransport(type: TransportType, address: String, name: String?) {
        val current = _selection.value
        val next = TransportSelection(
            type = type,
            httpAddress = if (type == TransportType.HTTP) address else current.httpAddress,
            bleAddress = if (type == TransportType.BLE) address else current.bleAddress,
            bleName = if (type == TransportType.BLE) (name ?: current.bleName) else current.bleName,
        )
        _selection.value = next
        persisted += next
    }
}

private class FakeServiceLauncher : ForegroundServiceLauncher {
    val starts = mutableListOf<String>()
    var stops = 0
    override fun startBleService(name: String) { starts += name }
    override fun stopBleService() { stops++ }
}

@RunWith(RobolectricTestRunner::class)
class TransportControllerMigrationTest {

    private fun newController(
        initial: TransportSelection,
        factory: TransportFactory = { _, _ -> FakeBleGaugeTransport() },
        launcher: FakeServiceLauncher = FakeServiceLauncher(),
    ): Pair<TransportController, FakeSettingsStore> {
        val store = FakeSettingsStore(initial)
        val ctx = ApplicationProvider.getApplicationContext<Context>()
        val controller = TransportController(ctx, store, factory, launcher)
        return controller to store
    }

    @Test
    fun restoreMigratesPersistedHttpToBleEmpty() = runTest {
        val initial = TransportSelection(TransportType.HTTP, "192.168.4.1", "", "")
        val (controller, store) = newController(initial)
        controller.restore()
        assertEquals(TransportType.BLE, controller.selection.value.type)
        assertEquals("", controller.selection.value.bleAddress)
        assertNull(controller.transport.value)
        // Must have persisted the migration to BLE
        assertTrue(store.persisted.any { it.type == TransportType.BLE && it.bleAddress == "" })
    }

    @Test
    fun restoreWithBleBlankStaysBleEmptyWithoutHttpFallback() = runTest {
        val initial = TransportSelection(TransportType.BLE, "", "", "")
        val (controller, _) = newController(initial)
        controller.restore()
        assertEquals(TransportType.BLE, controller.selection.value.type)
        assertEquals("", controller.selection.value.bleAddress)
        assertNull(controller.transport.value)
    }

    @Test
    fun restoreWithBleAddressCreatesBleTransport() = runTest {
        val initial = TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge")
        var createdType: TransportType? = null
        var createdAddress: String? = null
        val factory: TransportFactory = { type, address ->
            createdType = type
            createdAddress = address
            FakeBleGaugeTransport()
        }
        val (controller, _) = newController(initial, factory)
        controller.restore()
        assertEquals(TransportType.BLE, createdType)
        assertEquals("AA:BB:CC:DD:EE:FF", createdAddress)
        assertTrue(controller.transport.value != null)
        assertEquals(TransportType.BLE, controller.selection.value.type)
    }

    @Test
    fun selectBleStartsForegroundService() = runTest {
        val initial = TransportSelection(TransportType.BLE, "", "", "")
        val launcher = FakeServiceLauncher()
        val (controller, _) = newController(initial, launcher = launcher)
        controller.select(TransportType.BLE, "AA:BB:CC:DD:EE:FF", "BoostGauge")
        assertEquals(1, launcher.starts.size)
        assertEquals("BoostGauge", launcher.starts.first())
    }

    @Test
    fun disconnectStopsForegroundServiceAndKeepsSavedPeer() = runTest {
        val initial = TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge")
        val launcher = FakeServiceLauncher()
        val (controller, store) = newController(initial, launcher = launcher)
        controller.restore()
        // restore with address will have started service
        launcher.starts.clear()
        controller.disconnect()
        assertEquals(1, launcher.stops)
        // The live link is gone…
        assertNull(controller.transport.value)
        // …but the remembered peer survives for the "Saved gauge" row and for
        // auto-reconnect on a fresh app launch (BLE session resilience).
        assertEquals("AA:BB:CC:DD:EE:FF", controller.selection.value.bleAddress)
        assertEquals("BoostGauge", controller.selection.value.bleName)
        assertEquals("AA:BB:CC:DD:EE:FF", store.selection.first().bleAddress)
    }

    @Test
    fun httpNeverBecomesActiveTransportAtStartup() = runTest {
        // Simulate legacy persisted HTTP with SoftAP default
        val legacy = TransportSelection(TransportType.HTTP, "192.168.4.1", "", "")
        val (controller, _) = newController(legacy)
        controller.restore()
        // Must NOT be HTTP
        assertTrue(controller.selection.value.type != TransportType.HTTP)
        // Must NOT have created an HttpTransport
        assertTrue(controller.transport.value == null || controller.transport.value !is HttpTransport)
    }
}
