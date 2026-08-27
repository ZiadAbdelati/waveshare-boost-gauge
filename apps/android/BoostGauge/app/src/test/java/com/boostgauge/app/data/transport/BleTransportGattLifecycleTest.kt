package com.boostgauge.app.data.transport

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import android.bluetooth.BluetoothProfile
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowBluetoothGatt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withContext

/**
 * Round-8 zombie-GATT guard: every path that drops or kills the transport must
 * fully close the BluetoothGatt (close(), not just disconnect()), so the board
 * releases the ACL and restarts advertising. A leaked gatt object is exactly
 * what the field report reproduced (control writes continued with NO
 * phone-connected event; the app never reconnected after restart).
 *
 * These tests drive the REAL BleTransport against a controllable fake
 * BluetoothGatt (a custom Robolectric shadow) whose connect/discover/MTU/notify
 * steps fire their callbacks immediately, so connect() completes without a
 * radio and the teardown paths can be asserted on.
 */
@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
@Config(shadows = [FakeBluetoothGattShadow::class], sdk = [33])
class BleTransportGattLifecycleTest {

    private val context: Context = ApplicationProvider.getApplicationContext()
    private val address = "AA:BB:CC:DD:EE:FF"

    @Before
    fun setUp() {
        FakeBluetoothGattShadow.reset()
        FakeBluetoothGattShadow.service = boostService()
    }

    @After
    fun tearDown() {
        FakeBluetoothGattShadow.reset()
    }

    private fun boostService(): BluetoothGattService {
        val service = BluetoothGattService(GaugeGatt.service, BluetoothGattService.SERVICE_TYPE_PRIMARY)
        val control = BluetoothGattCharacteristic(GaugeGatt.control, 0x10, 0x01)
        control.addDescriptor(BluetoothGattDescriptor(GaugeGatt.clientCharacteristicConfig, 0x02))
        service.addCharacteristic(control)
        service.addCharacteristic(BluetoothGattCharacteristic(GaugeGatt.log, 0x02, 0x01))
        service.addCharacteristic(BluetoothGattCharacteristic(GaugeGatt.deviceInfo, 0x02, 0x01))
        return service
    }

    /** Transport whose gattFactory drives the full GATT sequence to CONNECTED. */
    private fun newTransport(): BleTransport =
        BleTransport(context, address) { device, callback ->
            val gatt = ShadowBluetoothGatt.newInstance(device)
            FakeBluetoothGattShadow.callback = callback
            FakeBluetoothGattShadow.lastGatt = gatt
            callback.onConnectionStateChange(
                gatt,
                BluetoothGatt.GATT_SUCCESS,
                BluetoothProfile.STATE_CONNECTED,
            )
            gatt
        }

    /**
     * Runs connect on a real dispatcher (real time) so the BLE event loop can
     * deliver the shadow's callbacks deterministically — the test scheduler's
     * virtual clock would otherwise race the transport's withTimeout.
     */
    private fun connectOnRealDispatcher(transport: BleTransport): Result<Unit> =
        runBlocking {
            runCatching { withContext(Dispatchers.Default) { transport.connect() } }
        }

    @Test
    fun explicitDisconnectClosesTheGatt() = runBlocking {
        val transport = newTransport()
        val connected = connectOnRealDispatcher(transport)
        assertTrue("connect should succeed with the fake: ${connected.exceptionOrNull()}", connected.isSuccess)
        assertTrue(transport.linkUp.value)

        transport.close()

        // close() must tear down the ACL (disconnect + close), never leak it.
        assertEquals(1, FakeBluetoothGattShadow.disconnectCount)
        assertEquals(1, FakeBluetoothGattShadow.closeCount)
        assertFalse(transport.linkUp.value)
    }

    @Test
    fun linkLossEventClosesTheGatt() = runBlocking {
        val transport = newTransport()
        val connected = connectOnRealDispatcher(transport)
        assertTrue("connect should succeed with the fake: ${connected.exceptionOrNull()}", connected.isSuccess)
        assertTrue(transport.linkUp.value)

        // Board/OS drops the link: the transport must close the gatt so the
        // board releases the ACL and restarts advertising.
        FakeBluetoothGattShadow.callback?.onConnectionStateChange(
            FakeBluetoothGattShadow.lastGatt,
            BluetoothGatt.GATT_SUCCESS,
            BluetoothProfile.STATE_DISCONNECTED,
        )
        // Let the real BLE event loop process the drop.
        Thread.sleep(50)

        assertEquals("link loss must close the gatt, not just disconnect()", 1, FakeBluetoothGattShadow.closeCount)
        assertEquals(1, FakeBluetoothGattShadow.disconnectCount)
        assertFalse(transport.linkUp.value)
    }

    @Test
    fun failedConnectClosesTheFreshGatt() = runBlocking {
        // No BoostGauge service discovered -> connect() throws after the GATT
        // link is established; the freshly created gatt must still be closed.
        FakeBluetoothGattShadow.service = null
        val transport = newTransport()
        val result = connectOnRealDispatcher(transport)

        assertTrue("connect must fail when the service is missing", result.isFailure)
        assertEquals(1, FakeBluetoothGattShadow.closeCount)
        assertEquals(1, FakeBluetoothGattShadow.disconnectCount)
        assertFalse(transport.linkUp.value)
    }

    @Test
    fun timedOutConnectClosesTheFreshGatt() = runTest {
        // A connect that never receives a connection event must time out and
        // close its gatt rather than leave a zombie behind. Virtual time drives
        // the connect timeout; no real BLE events are delivered.
        val transport = BleTransport(context, address) { device, _ ->
            ShadowBluetoothGatt.newInstance(device)
        }
        val result = runCatching { transport.connect() }
        advanceTimeBy(16_000L)
        runCurrent()

        assertTrue("connect must time out with no connection event", result.isFailure)
        assertTrue(result.exceptionOrNull() is TransportException)
        assertEquals("timed-out connect must close the gatt", 1, FakeBluetoothGattShadow.closeCount)
        assertEquals(1, FakeBluetoothGattShadow.disconnectCount)
        assertFalse(transport.linkUp.value)
    }
}