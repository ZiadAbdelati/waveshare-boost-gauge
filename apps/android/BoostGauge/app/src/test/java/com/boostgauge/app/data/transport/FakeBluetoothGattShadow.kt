package com.boostgauge.app.data.transport

import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothGattService
import org.robolectric.annotation.Implements
import org.robolectric.annotation.RealObject
import java.util.UUID

/**
 * Test-only Robolectric shadow that turns BluetoothGatt into a controllable
 * fake: every connect/discover/MTU/notify step fires its callback immediately
 * (so `BleTransport.connect()` can complete without a radio), and `close()` /
 * `disconnect()` are counted so tests prove the gatt object is torn down on
 * every disconnect/teardown path (the round-8 zombie-GATT invariant).
 *
 * The static `service` controls which characteristics the transport discovers;
 * set it to `null` to make `connect()` fail after the GATT link is established.
 */
@Implements(BluetoothGatt::class)
class FakeBluetoothGattShadow {

    @RealObject
    private lateinit var realGatt: BluetoothGatt

    companion object {
        var callback: BluetoothGattCallback? = null
        var service: BluetoothGattService? = null
        var lastGatt: BluetoothGatt? = null
        var closeCount = 0
        var disconnectCount = 0
        var writeDescriptorCount = 0
        var failWriteDescriptor = false
        var failFirstWriteDescriptor = false

        fun reset() {
            callback = null
            service = null
            lastGatt = null
            closeCount = 0
            disconnectCount = 0
            writeDescriptorCount = 0
            failWriteDescriptor = false
            failFirstWriteDescriptor = false
        }
    }

    @org.robolectric.annotation.Implementation
    protected fun disconnect() {
        disconnectCount++
    }

    @org.robolectric.annotation.Implementation
    protected fun close() {
        closeCount++
    }

    @org.robolectric.annotation.Implementation
    fun requestMtu(mtu: Int): Boolean {
        callback?.onMtuChanged(realGatt, mtu, BluetoothGatt.GATT_SUCCESS)
        return true
    }

    @org.robolectric.annotation.Implementation
    fun discoverServices(): Boolean {
        callback?.onServicesDiscovered(realGatt, BluetoothGatt.GATT_SUCCESS)
        return true
    }

    @org.robolectric.annotation.Implementation
    fun getService(uuid: UUID): BluetoothGattService? = service

    @org.robolectric.annotation.Implementation
    fun setCharacteristicNotification(characteristic: BluetoothGattCharacteristic, enable: Boolean): Boolean = true

    @org.robolectric.annotation.Implementation
    fun writeDescriptor(descriptor: BluetoothGattDescriptor): Boolean {
        writeDescriptorCount++
        val fail = failWriteDescriptor ||
            (failFirstWriteDescriptor && writeDescriptorCount == 1)
        if (fail) return false
        callback?.onDescriptorWrite(realGatt, descriptor, BluetoothGatt.GATT_SUCCESS)
        return true
    }
}
