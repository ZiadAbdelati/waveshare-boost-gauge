package com.boostgauge.app.data.transport

import android.Manifest
import android.app.Application
import android.bluetooth.BluetoothDevice
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
import org.robolectric.Shadows.shadowOf
import org.robolectric.shadows.ShadowBluetoothGatt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext

/**
 * Stale-bond recovery: when the phone's stack caches old bond keys (a previous
 * firmware flash wiped the board's RAM/NVS bond, so encryption with the cached
 * LTK fails with 0x0D PIN_OR_KEY_MISSING) the GATT link connects and discovers
 * fine but the CCCD write fails or the link is not encrypted. connect() must
 * then remove the stale bond, wait for BOND_NONE, re-bond fresh, await
 * BOND_BONDED, and retry the CCCD write exactly once — never looping recovery
 * more than once per connect().
 *
 * These tests drive the REAL BleTransport against FakeBluetoothGattShadow and
 * an injectable fake [BondOperations] (the removeBond reflection shim is
 * behind the seam, so no real Bluetooth API is touched).
 */
@OptIn(ExperimentalCoroutinesApi::class)
@RunWith(RobolectricTestRunner::class)
@Config(shadows = [FakeBluetoothGattShadow::class], sdk = [33])
class BleTransportStaleBondRecoveryTest {

    private val context: Context = ApplicationProvider.getApplicationContext()
    private val address = "AA:BB:CC:DD:EE:FF"

    @Before
    fun setUp() {
        FakeBluetoothGattShadow.reset()
        FakeBluetoothGattShadow.service = boostService()
        // Recovery performs removeBond/createBond, which need BLUETOOTH_CONNECT
        // (granted in production before scanning/connecting).
        shadowOf(ApplicationProvider.getApplicationContext<Application>())
            .grantPermissions(Manifest.permission.BLUETOOTH_CONNECT)
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

    /**
     * Controllable bond stack. `state` is the value bondState() reports (real
     * stacks keep reporting BONDED while the old LTK is cached), `encrypted`
     * models whether the connected GATT link is actually encrypted, and
     * removeBond/createBond transition state exactly like a fresh pairing.
     */
    private class FakeBondOperations(
        var state: Int = BluetoothDevice.BOND_BONDED,
        var encrypted: Boolean = true,
    ) : BondOperations {
        var removeCalls = 0
        var createCalls = 0
        val awaitedTargets = mutableListOf<Int>()

        override fun bondState(device: BluetoothDevice): Int = state

        override fun createBond(device: BluetoothDevice): Boolean {
            createCalls++
            state = BluetoothDevice.BOND_BONDED
            return true
        }

        override fun removeBond(device: BluetoothDevice): Boolean {
            removeCalls++
            state = BluetoothDevice.BOND_NONE
            return true
        }

        override fun isLinkEncrypted(device: BluetoothDevice): Boolean = encrypted

        override suspend fun awaitBondState(device: BluetoothDevice, target: Int, timeoutMs: Long) {
            awaitedTargets += target
            state = target
        }

        override suspend fun awaitBondSettled(device: BluetoothDevice, timeoutMs: Long) {
            state = BluetoothDevice.BOND_BONDED
        }
    }

    /** Transport with an injected fake bond stack and full GATT sequence. */
    private fun newTransport(bond: FakeBondOperations): BleTransport =
        BleTransport(context, address, bondOps = bond) { device, callback ->
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

    private fun connectOnRealDispatcher(transport: BleTransport): Result<Unit> =
        runBlocking {
            runCatching { withContext(Dispatchers.Default) { transport.connect() } }
        }

    @Test
    fun insufficientEncryptionTriggersStaleBondRecoveryBeforeCccdWrite() {
        // Stack reports BONDED (stale cache) but the link is not encrypted:
        // recovery must remove + re-bond BEFORE the CCCD write, which then runs
        // exactly once.
        val bond = FakeBondOperations(
            state = BluetoothDevice.BOND_BONDED,
            encrypted = false,
        )
        val transport = newTransport(bond)
        val result = connectOnRealDispatcher(transport)

        assertTrue("connect should recover and succeed: ${result.exceptionOrNull()}", result.isSuccess)
        assertTrue(transport.linkUp.value)
        assertEquals("stale bond must be removed once", 1, bond.removeCalls)
        assertEquals("fresh bond must be created once", 1, bond.createCalls)
        assertEquals(
            "recovery must wait BOND_NONE then BOND_BONDED before the CCCD write",
            listOf(BluetoothDevice.BOND_NONE, BluetoothDevice.BOND_BONDED),
            bond.awaitedTargets,
        )
        assertEquals("CCCD written once after recovery", 1, FakeBluetoothGattShadow.writeDescriptorCount)
    }

    @Test
    fun failedCccdWriteTriggersStaleBondRecoveryAndRetriesOnce() {
        // Link claims encrypted but the first CCCD write fails with the stale
        // key: recovery removes + re-bonds, then retries the CCCD write once
        // and succeeds.
        val bond = FakeBondOperations(
            state = BluetoothDevice.BOND_BONDED,
            encrypted = true,
        )
        FakeBluetoothGattShadow.failFirstWriteDescriptor = true
        val transport = newTransport(bond)
        val result = connectOnRealDispatcher(transport)

        assertTrue("first CCCD failure must recover and retry once: ${result.exceptionOrNull()}", result.isSuccess)
        assertTrue(transport.linkUp.value)
        assertEquals(1, bond.removeCalls)
        assertEquals(1, bond.createCalls)
        assertEquals("two CCCD writes total: initial + one retry", 2, FakeBluetoothGattShadow.writeDescriptorCount)
    }

    @Test
    fun staleBondRecoveryRunsAtMostOncePerConnect() {
        // CCCD keeps failing after recovery: connect() must fail rather than
        // repeatedly remove/re-bond inside a single connect() call.
        val bond = FakeBondOperations(
            state = BluetoothDevice.BOND_BONDED,
            encrypted = true,
        )
        FakeBluetoothGattShadow.failWriteDescriptor = true
        val transport = newTransport(bond)
        val result = connectOnRealDispatcher(transport)

        assertTrue("persistent CCCD failure must fail connect", result.isFailure)
        assertFalse(transport.linkUp.value)
        assertEquals("recovery must not loop", 1, bond.removeCalls)
        assertEquals("recovery must not loop", 1, bond.createCalls)
        assertEquals("one initial write + one retry, then fail", 2, FakeBluetoothGattShadow.writeDescriptorCount)
    }
}
