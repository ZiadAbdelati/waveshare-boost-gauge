package com.boostgauge.app.data.transport

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.os.ParcelUuid
import androidx.core.content.ContextCompat
import com.boostgauge.app.data.api.ApiJson
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put
import java.io.IOException
import java.util.UUID
import java.util.concurrent.Executors

/** GATT identifiers for the BoostGauge service (docs/bluetooth-gatt.md). */
object GaugeGatt {
    val service: UUID = UUID.fromString("b6a00000-0000-4000-8000-00000000b6a0")
    val control: UUID = UUID.fromString("b6a00001-0000-4000-8000-00000000b6a0")
    val status: UUID = UUID.fromString("b6a00002-0000-4000-8000-00000000b6a0")
    val log: UUID = UUID.fromString("b6a00003-0000-4000-8000-00000000b6a0")
    val deviceInfo: UUID = UUID.fromString("b6a00004-0000-4000-8000-00000000b6a0")
    val clientCharacteristicConfig: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
}

data class BleScanResult(val address: String, val name: String)

private data class GattResponse(val status: Int, val body: String)

internal data class BleLogRow(
    val tMs: Long,
    val psi: Double,
    val peakPsi: Double,
    val zone: String,
    val demo: Boolean,
)

internal object BleLogParser {
    /**
     * "BGL1\n" then one CSV row per sample: t_ms,psi,peak_psi,zone,demo.
     * Kept pure so it is unit-testable without a radio.
     */
    fun parse(payload: String): List<BleLogRow> {
        val body = payload.removePrefix("BGL1\n")
        return body.lineSequence()
            .filter { it.isNotBlank() }
            .mapNotNull { line ->
                val parts = line.split(",")
                if (parts.size < 5) return@mapNotNull null
                BleLogRow(
                    tMs = parts[0].trim().toLongOrNull() ?: 0L,
                    psi = parts[1].trim().toDoubleOrNull() ?: 0.0,
                    peakPsi = parts[2].trim().toDoubleOrNull() ?: 0.0,
                    zone = parts[3].trim(),
                    demo = parts[4].trim().toIntOrNull() == 1,
                )
            }
            .toList()
    }
}

private sealed interface GattEvent {
    data class ConnectionState(val gatt: BluetoothGatt, val status: Int, val newState: Int) : GattEvent
    data class ServicesDiscovered(val gatt: BluetoothGatt, val status: Int) : GattEvent
    data class MtuChanged(val mtu: Int, val status: Int) : GattEvent
    data class CharacteristicRead(
        val characteristic: BluetoothGattCharacteristic,
        val value: ByteArray,
        val status: Int,
    ) : GattEvent
    data class CharacteristicChanged(
        val characteristic: BluetoothGattCharacteristic,
        val value: ByteArray,
    ) : GattEvent
    data class CharacteristicWritten(
        val characteristic: BluetoothGattCharacteristic,
        val status: Int,
    ) : GattEvent
    data class DescriptorWritten(val descriptor: BluetoothGattDescriptor, val status: Int) : GattEvent

    /** Internal command: write a control request from the event-loop dispatcher. */
    data class CommandWrite(
        val characteristic: BluetoothGattCharacteristic,
        val id: Int,
        val value: ByteArray,
    ) : GattEvent
}

/**
 * BLE transport for the BoostGauge GATT service.
 *
 * A single executor-backed dispatcher consumes a Channel of Gatt callbacks and
 * also issues every Gatt call, so all callbacks and calls are serialized there.
 * Requests carry a u32 id; responses are matched by id with a timeout.
 */
@SuppressLint("MissingPermission")
class BleTransport(
    private val context: Context,
    private val deviceAddress: String,
) : GaugeTransport {

    private val bleDispatcher = Executors.newSingleThreadExecutor { r ->
        Thread(r, "boost-ble").apply { isDaemon = true }
    }.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + bleDispatcher)
    private val events = Channel<GattEvent>(Channel.UNLIMITED)

    private val mutex = Mutex()
    private val pendingResponses = mutableMapOf<Int, CancellableContinuation<GattResponse>>()
    private val pendingWaits = mutableMapOf<String, CancellableContinuation<GattEvent>>()
    private var nextRequestId = 1

    private var gatt: BluetoothGatt? = null
    private var controlCharacteristic: BluetoothGattCharacteristic? = null
    private var statusCharacteristic: BluetoothGattCharacteristic? = null
    private var logCharacteristic: BluetoothGattCharacteristic? = null
    private var deviceInfoCharacteristic: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private var closed = false

    private val _statusLine = MutableStateFlow<String?>(null)

    /** Raw /state-shaped JSON published by the Status characteristic (~1 Hz). */
    val statusLine: StateFlow<String?> = _statusLine.asStateFlow()

    init {
        scope.launch {
            for (event in events) {
                handleEvent(event)
            }
        }
    }

    private fun handleEvent(event: GattEvent) {
        when (event) {
            is GattEvent.ConnectionState -> {
                if (event.newState == BluetoothProfile.STATE_CONNECTED) {
                    gatt = event.gatt
                    completeWait("connect", event)
                } else {
                    failAll("gauge disconnected")
                }
            }
            is GattEvent.ServicesDiscovered -> completeWait("discover", event)
            is GattEvent.MtuChanged -> {
                if (event.mtu in 23..517) mtu = event.mtu
                completeWait("mtu", event)
            }
            is GattEvent.CharacteristicChanged -> {
                when (event.characteristic.uuid) {
                    GaugeGatt.control -> handleControlNotify(event.value)
                    GaugeGatt.status -> _statusLine.value = event.value.decodeToString()
                    else -> Unit
                }
            }
            is GattEvent.CharacteristicRead -> {
                completeWait("read:${event.characteristic.uuid}", event)
            }
            is GattEvent.CharacteristicWritten -> {
                completeWait("write:${event.characteristic.uuid}", event)
            }
            is GattEvent.DescriptorWritten -> {
                completeWait("desc:${event.descriptor.uuid}", event)
            }
            is GattEvent.CommandWrite -> {
                val characteristic = event.characteristic
                characteristic.value = event.value
                characteristic.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                val ok = runCatching { gatt?.writeCharacteristic(characteristic) == true }.getOrDefault(false)
                if (!ok) {
                    pendingResponses.remove(event.id)?.resumeWith(
                        Result.failure(TransportException("writeCharacteristic failed")),
                    )
                }
            }
        }
    }

    private fun handleControlNotify(bytes: ByteArray) {
        val text = bytes.decodeToString()
        val obj = runCatching { ApiJson.json.parseToJsonElement(text).jsonObject }.getOrNull() ?: return
        val id = obj["id"]?.jsonPrimitive?.intOrNull ?: return
        val pending = pendingResponses.remove(id) ?: return
        val status = obj["status"]?.jsonPrimitive?.intOrNull ?: -1
        val body = obj["body"]
        val bodyText = if (body == null || body is JsonNull) "" else body.toString()
        pending.resumeWith(Result.success(GattResponse(status, bodyText)))
    }

    private fun completeWait(key: String, event: GattEvent) {
        pendingWaits.remove(key)?.resumeWith(Result.success(event))
    }

    private fun failAll(message: String) {
        val failure = TransportException(message)
        pendingWaits.values.forEach { it.resumeWith(Result.failure(failure)) }
        pendingWaits.clear()
        pendingResponses.values.forEach { it.resumeWith(Result.failure(failure)) }
        pendingResponses.clear()
    }

    private suspend fun await(key: String, timeoutMs: Long, eventMatches: (GattEvent) -> Boolean): GattEvent {
        return withTimeout(timeoutMs) {
            val event = suspendCancellableCoroutine<GattEvent> { cont ->
                pendingWaits[key] = cont
                cont.invokeOnCancellation { if (pendingWaits[key] === cont) pendingWaits.remove(key) }
            }
            if (!eventMatches(event)) {
                throw TransportException("unexpected GATT event for '$key'")
            }
            event
        }
    }

    /** Connect, discover, negotiate MTU and subscribe to control/status notifications. */
    suspend fun connect() {
        check(!closed) { "transport closed" }
        val g = withContext(bleDispatcher) {
            val device = requireDevice()
            attemptBond(device)
            device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
                ?: throw TransportException("connectGatt returned null")
        }
        gatt = g
        await("connect", CONNECT_TIMEOUT_MS) {
            it is GattEvent.ConnectionState && it.status == BluetoothGatt.GATT_SUCCESS
        }
        withContext(bleDispatcher) {
            if (!g.discoverServices()) throw TransportException("discoverServices failed")
        }
        await("discover", DISCOVER_TIMEOUT_MS) {
            it is GattEvent.ServicesDiscovered && it.status == BluetoothGatt.GATT_SUCCESS
        }

        withContext(bleDispatcher) {
            runCatching { g.requestMtu(MTU_TARGET) }
        }
        runCatching { await("mtu", MTU_TIMEOUT_MS) { it is GattEvent.MtuChanged } }

        val service = g.getService(GaugeGatt.service)
            ?: throw TransportException("BoostGauge service not found")
        val control = service.getCharacteristic(GaugeGatt.control)
            ?: throw TransportException("control characteristic missing")
        val status = service.getCharacteristic(GaugeGatt.status)
            ?: throw TransportException("status characteristic missing")
        val log = service.getCharacteristic(GaugeGatt.log)
            ?: throw TransportException("log characteristic missing")
        val info = service.getCharacteristic(GaugeGatt.deviceInfo)
            ?: throw TransportException("device info characteristic missing")
        controlCharacteristic = control
        statusCharacteristic = status
        logCharacteristic = log
        deviceInfoCharacteristic = info
        enableNotify(control)
        enableNotify(status)
    }

    private suspend fun attemptBond(device: BluetoothDevice) {
        if (device.bondState == BluetoothDevice.BOND_BONDED) return
        if (ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) !=
            PackageManager.PERMISSION_GRANTED
        ) {
            return
        }
        val created = try {
            device.createBond()
        } catch (t: Throwable) {
            false
        }
        if (created) {
            try {
                awaitBond(device)
            } catch (t: Throwable) {
                // Best-effort bonding; an encrypted service may still reject reads/writes.
            }
        }
    }

    private suspend fun awaitBond(device: BluetoothDevice) {
        withTimeout(BOND_TIMEOUT_MS) {
            suspendCancellableCoroutine { cont ->
                val receiver = object : BroadcastReceiver() {
                    override fun onReceive(context: Context, intent: Intent) {
                        if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                        val extra = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                        if (extra?.address != device.address) return
                        if (device.bondState == BluetoothDevice.BOND_BONDED ||
                            intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, -1) == BluetoothDevice.BOND_NONE
                        ) {
                            context.unregisterReceiver(this)
                            cont.resumeWith(Result.success(Unit))
                        }
                    }
                }
                context.registerReceiver(receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
                cont.invokeOnCancellation { runCatching { context.unregisterReceiver(receiver) } }
            }
        }
    }

    private suspend fun enableNotify(characteristic: BluetoothGattCharacteristic) {
        val g = requireGatt()
        withContext(bleDispatcher) {
            if (!g.setCharacteristicNotification(characteristic, true)) {
                throw TransportException("setCharacteristicNotification failed")
            }
            val descriptor = characteristic.getDescriptor(GaugeGatt.clientCharacteristicConfig)
            if (descriptor == null) return@withContext
            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            if (!g.writeDescriptor(descriptor)) {
                throw TransportException("writeDescriptor failed")
            }
        }
        await("desc:${GaugeGatt.clientCharacteristicConfig}", WRITE_TIMEOUT_MS) {
            it is GattEvent.DescriptorWritten &&
                it.descriptor.uuid == GaugeGatt.clientCharacteristicConfig &&
                it.status == BluetoothGatt.GATT_SUCCESS
        }
    }

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp = mutex.withLock {
        val char = requireControl()
        val id = nextRequestId++
        val body: JsonElement = bodyJson?.let {
            runCatching { ApiJson.json.parseToJsonElement(it) }.getOrDefault(buildJsonObject {})
        } ?: buildJsonObject {}
        val request = buildJsonObject {
            put("id", id)
            put("path", path)
            put("method", method.uppercase())
            put("body", body)
        }
        val encoded = request.toString().toByteArray(Charsets.UTF_8)
        if (encoded.size > MAX_REQUEST_BYTES) {
            throw TransportException("BLE request exceeds $MAX_REQUEST_BYTES bytes")
        }
        withTimeout(REQUEST_TIMEOUT_MS) {
            val response = suspendCancellableCoroutine<GattResponse> { cont ->
                pendingResponses[id] = cont
                cont.invokeOnCancellation { pendingResponses.remove(id) }
                events.trySend(GattEvent.CommandWrite(char, id, encoded))
            }
            if (path == "state") _statusLine.value = response.body
            Resp(response.status, response.body)
        }
    }

    override suspend fun get(path: String): Resp = send("GET", path, null)

    /** Read the Log characteristic (Android reassembles GATT long reads). */
    suspend fun readLog(): String = readValue(requireLog())

    /** Read the DeviceInfo characteristic. */
    suspend fun readDeviceInfo(): String = readValue(requireDeviceInfo())

    private suspend fun readValue(char: BluetoothGattCharacteristic): String {
        val g = requireGatt()
        withContext(bleDispatcher) {
            if (!g.readCharacteristic(char)) throw TransportException("readCharacteristic failed")
        }
        val event = await("read:${char.uuid}", READ_TIMEOUT_MS) { it is GattEvent.CharacteristicRead }
        if (event !is GattEvent.CharacteristicRead || event.status != BluetoothGatt.GATT_SUCCESS) {
            throw TransportException("characteristic read failed")
        }
        return event.value.decodeToString()
    }

    override suspend fun close() {
        if (closed) return
        closed = true
        failAll("transport closed")
        scope.cancel()
        withContext(Dispatchers.IO) {
            val g = gatt
            gatt = null
            runCatching { g?.disconnect() }
            runCatching { g?.close() }
        }
    }

    private fun requireGatt(): BluetoothGatt =
        gatt ?: throw TransportException("BLE not connected")

    private fun requireControl(): BluetoothGattCharacteristic =
        controlCharacteristic ?: throw TransportException("control characteristic unavailable")

    private fun requireLog(): BluetoothGattCharacteristic =
        logCharacteristic ?: throw TransportException("log characteristic unavailable")

    private fun requireDeviceInfo(): BluetoothGattCharacteristic =
        deviceInfoCharacteristic ?: throw TransportException("device info characteristic unavailable")

    private fun requireDevice(): BluetoothDevice {
        val adapter = bluetoothAdapter()
            ?: throw TransportException("Bluetooth adapter unavailable")
        return adapter.getRemoteDevice(deviceAddress)
    }

    private fun bluetoothAdapter(): BluetoothAdapter? {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        return manager?.adapter
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            events.trySend(GattEvent.ConnectionState(gatt, status, newState))
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            events.trySend(GattEvent.ServicesDiscovered(gatt, status))
        }

        override fun onMtuChanged(gatt: BluetoothGatt, mtu: Int, status: Int) {
            events.trySend(GattEvent.MtuChanged(mtu, status))
        }

        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            events.trySend(GattEvent.CharacteristicRead(characteristic, characteristic.value, status))
        }

        @androidx.annotation.RequiresApi(Build.VERSION_CODES.TIRAMISU)
        override fun onCharacteristicRead(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            events.trySend(GattEvent.CharacteristicRead(characteristic, value, status))
        }

        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            events.trySend(GattEvent.CharacteristicChanged(characteristic, characteristic.value))
        }

        @androidx.annotation.RequiresApi(Build.VERSION_CODES.TIRAMISU)
        override fun onCharacteristicChanged(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            events.trySend(GattEvent.CharacteristicChanged(characteristic, value))
        }

        override fun onCharacteristicWrite(
            gatt: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            events.trySend(GattEvent.CharacteristicWritten(characteristic, status))
        }

        override fun onDescriptorWrite(
            gatt: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            events.trySend(GattEvent.DescriptorWritten(descriptor, status))
        }
    }

    companion object {
        const val MAX_REQUEST_BYTES = 480
        const val MTU_TARGET = 512
        private const val CONNECT_TIMEOUT_MS = 15_000L
        private const val DISCOVER_TIMEOUT_MS = 15_000L
        private const val MTU_TIMEOUT_MS = 10_000L
        private const val WRITE_TIMEOUT_MS = 10_000L
        private const val READ_TIMEOUT_MS = 15_000L
        private const val REQUEST_TIMEOUT_MS = 20_000L
        private const val BOND_TIMEOUT_MS = 15_000L
    }
}

@SuppressLint("MissingPermission")
class BleScanner(private val context: Context) {

    suspend fun scan(timeoutMs: Long = 12_000L): List<BleScanResult> {
        val adapter = bluetoothAdapter()
            ?: throw TransportException("Bluetooth adapter unavailable")
        val scanner = adapter.bluetoothLeScanner
            ?: throw TransportException("Bluetooth LE scanner unavailable")
        val results = mutableListOf<BleScanResult>()
        val failures = Channel<IOException>(Channel.CONFLATED)
        val filter = ScanFilter.Builder()
            .setServiceUuid(ParcelUuid(GaugeGatt.service))
            .build()
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        val callback = object : ScanCallback() {
            override fun onScanResult(callbackType: Int, result: ScanResult) {
                val device = result.device
                val name = device.name
                val matchesService = result.scanRecord?.serviceUuids
                    ?.any { it.equals(ParcelUuid(GaugeGatt.service)) } == true
                if (name == "BoostGauge" || matchesService) {
                    if (results.none { it.address == device.address }) {
                        results += BleScanResult(device.address, name ?: "BoostGauge")
                    }
                }
            }

            override fun onBatchScanResults(resultsList: List<ScanResult>) {
                resultsList.forEach { onScanResult(0, it) }
            }

            override fun onScanFailed(errorCode: Int) {
                failures.trySend(TransportException("BLE scan failed (code $errorCode)"))
            }
        }
        scanner.startScan(listOf(filter), settings, callback)
        return try {
            withTimeoutOrNull(timeoutMs) { failures.receive() }?.let { throw it }
            results.toList()
        } finally {
            runCatching { scanner.stopScan(callback) }
        }
    }

    private fun bluetoothAdapter(): BluetoothAdapter? {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
        return manager?.adapter
    }
}
