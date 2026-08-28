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
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.asCoroutineDispatcher
import kotlinx.coroutines.cancel
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import kotlinx.coroutines.TimeoutCancellationException
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
        if (!payload.startsWith("BGL1\n")) {
            throw TransportException("unsupported BLE log format")
        }
        val body = payload.removePrefix("BGL1\n")
        return body.lineSequence()
            .filter { it.isNotBlank() }
            .mapNotNull { line ->
                val parts = line.split(",")
                if (parts.size < 5) return@mapNotNull null
                if (parts[0].trim() == "t_ms") return@mapNotNull null
                val tMs = parts[0].trim().toLongOrNull() ?: return@mapNotNull null
                val psi = parts[1].trim().toDoubleOrNull() ?: return@mapNotNull null
                val peakPsi = parts[2].trim().toDoubleOrNull() ?: return@mapNotNull null
                BleLogRow(
                    tMs = tMs,
                    psi = psi,
                    peakPsi = peakPsi,
                    zone = parts[3].trim(),
                    demo = parts[4].trim().toIntOrNull() == 1,
                )
            }
            .toList()
    }
}

/** Reassembles concatenated or fragmented JSON objects from Control notifications. */
internal class BleControlFramer {
    private val buffer = StringBuilder()

    fun clear() = buffer.setLength(0)

    fun append(bytes: ByteArray): List<String> {
        buffer.append(bytes.decodeToString())
        val frames = mutableListOf<String>()
        while (true) {
            val end = firstObjectEnd() ?: break
            frames += buffer.substring(0, end + 1)
            buffer.delete(0, end + 1)
        }
        return frames
    }

    private fun firstObjectEnd(): Int? {
        var depth = 0
        var started = false
        var inString = false
        var escaped = false
        for (index in buffer.indices) {
            val ch = buffer[index]
            if (inString) {
                when {
                    escaped -> escaped = false
                    ch == '\\' -> escaped = true
                    ch == '"' -> inString = false
                }
                continue
            }
            when (ch) {
                '"' -> inString = true
                '{' -> {
                    started = true
                    depth++
                }
                '}' -> if (started && --depth == 0) return index
            }
        }
        return null
    }
}

/**
 * Seam for the BluetoothDevice bond operations used by [BleTransport].
 *
 * The production implementation talks to the real Bluetooth stack; tests inject
 * a fake so stale-bond recovery can be driven without a radio. The seam exists
 * because the stale-bond deadlock needs two actions Android does not expose
 * through normal APIs:
 *  - `removeBond()` (hidden API, reached through Java reflection on Android 12)
 *    to flush the stack/kernel key cache that survives the Settings "no bonded
 *    device" state, and
 *  - bounded waits that observe the BOND_NONE / BOND_BONDED transitions.
 */
interface BondOperations {
    fun bondState(device: BluetoothDevice): Int

    fun createBond(device: BluetoothDevice): Boolean

    /** Best-effort removal of a stale bond (reflection-backed in production). */
    fun removeBond(device: BluetoothDevice): Boolean

    /**
     * Whether the connected GATT link is encrypted. Android has no public
     * GATT-encryption getter; a completed bond is the closest proxy because
     * Android 12 auto-encrypts bonded LE links. Overridable so tests (or a
     * future stronger signal) can model the stale-cache case where
     * bondState says BONDED while the link is not actually encrypted.
     */
    fun isLinkEncrypted(device: BluetoothDevice): Boolean =
        bondState(device) == BluetoothDevice.BOND_BONDED

    /** Suspends until `device` reaches `target` bond state; throws [TransportException] on timeout. */
    suspend fun awaitBondState(device: BluetoothDevice, target: Int, timeoutMs: Long)

    /**
     * Suspends until a best-effort bonding attempt settles at BOND_BONDED or
     * BOND_NONE; throws [TransportException] on timeout.
     */
    suspend fun awaitBondSettled(device: BluetoothDevice, timeoutMs: Long)
}

/** Production [BondOperations] backed by the real Android Bluetooth stack. */
class AndroidBondOperations(private val context: Context) : BondOperations {
    override fun bondState(device: BluetoothDevice): Int = device.bondState

    override fun createBond(device: BluetoothDevice): Boolean =
        runCatching { device.createBond() }.getOrDefault(false)

    /**
     * BluetoothDevice.removeBond() exists on Android 12 (hidden API, present
     * through reflection) and is the standard workaround for a stale-bond key
     * cache: Settings shows no bond while the stack still encrypts with the
     * old LTK from a previous firmware flash. Guarded and best-effort — a
     * refusal just means recovery fails and connect() reports it.
     */
    override fun removeBond(device: BluetoothDevice): Boolean = runCatching {
        val removeBond = BluetoothDevice::class.java.getMethod("removeBond")
        removeBond.invoke(device) as Boolean
    }.getOrDefault(false)

    override suspend fun awaitBondState(device: BluetoothDevice, target: Int, timeoutMs: Long) {
        awaitBondUntil(device, timeoutMs) { it == target }
    }

    override suspend fun awaitBondSettled(device: BluetoothDevice, timeoutMs: Long) {
        awaitBondUntil(device, timeoutMs) {
            it == BluetoothDevice.BOND_BONDED || it == BluetoothDevice.BOND_NONE
        }
    }

    private suspend fun awaitBondUntil(device: BluetoothDevice, timeoutMs: Long, done: (Int) -> Boolean) {
        try {
            withTimeout(timeoutMs) {
                suspendCancellableCoroutine<Unit> { cont ->
                    if (done(device.bondState)) {
                        cont.resumeWith(Result.success(Unit))
                        return@suspendCancellableCoroutine
                    }
                    val receiver = object : BroadcastReceiver() {
                        override fun onReceive(context: Context, intent: Intent) {
                            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
                            val extra = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                                intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java)
                            } else {
                                @Suppress("DEPRECATION")
                                intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
                            }
                            if (extra?.address != device.address) return
                            val state = intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, device.bondState)
                            if (done(state)) {
                                runCatching { context.unregisterReceiver(this) }
                                cont.resumeWith(Result.success(Unit))
                            }
                        }
                    }
                    context.registerReceiver(receiver, IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED))
                    cont.invokeOnCancellation {
                        runCatching { context.unregisterReceiver(receiver) }
                    }
                }
            }
        } catch (e: TimeoutCancellationException) {
            // Never let a bond timeout masquerade as coroutine cancellation:
            // the repository reconnect loop dies forever on CancellationException.
            throw TransportException("bond state did not settle within ${timeoutMs} ms", e)
        }
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
    /** Test seam for bond operations (state, create/removeBond, encrypted-link
     *  check and bounded state waits). Production uses the real Bluetooth
     *  stack; tests inject a fake to exercise stale-bond recovery without a
     *  radio. */
    private val bondOps: BondOperations = AndroidBondOperations(context),
    /** Test seam: how a BluetoothGatt is created for a device. Production uses
     *  connectGatt; tests inject a controllable fake to prove the gatt object
     *  is closed on every disconnect/teardown path. */
    private val gattFactory: (device: BluetoothDevice, callback: BluetoothGattCallback) -> BluetoothGatt? =
        { device, callback -> device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE) },
) : BleGaugeTransport {

    private val bleDispatcher = Executors.newSingleThreadExecutor { r ->
        Thread(r, "boost-ble").apply { isDaemon = true }
    }.asCoroutineDispatcher()
    private val scope = CoroutineScope(SupervisorJob() + bleDispatcher)
    private val events = Channel<GattEvent>(Channel.UNLIMITED)

    private val mutex = Mutex()
    private val connectMutex = Mutex()
    private val controlFramer = BleControlFramer()
    private val statusFramer = BleControlFramer()
    private var statusPollJob: Job? = null
    private val pendingResponses = mutableMapOf<Int, CancellableContinuation<GattResponse>>()
    private val pendingWaits = mutableMapOf<String, CancellableContinuation<GattEvent>>()
    private var nextRequestId = 1
    private var activeRequestId: Int? = null

    private var gatt: BluetoothGatt? = null
    private var controlCharacteristic: BluetoothGattCharacteristic? = null
    private var logCharacteristic: BluetoothGattCharacteristic? = null
    private var deviceInfoCharacteristic: BluetoothGattCharacteristic? = null
    private var mtu = 23
    private var closed = false
    @Volatile private var linkReady = false

    override val transportKind: String = "BLE"

    private val _statusLine = MutableStateFlow<String?>(null)

    /** Raw /state-shaped JSON published by the Status characteristic (~1 Hz). */
    override val statusLine: StateFlow<String?> = _statusLine.asStateFlow()

    private val _linkUp = MutableStateFlow(false)

    /** True while the GATT link is established (see BleGaugeTransport.linkUp). */
    override val linkUp: StateFlow<Boolean> = _linkUp.asStateFlow()

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
                if (event.gatt !== gatt) return
                if (event.newState == BluetoothProfile.STATE_CONNECTED) {
                    gatt = event.gatt
                    completeWait("connect", event)
                } else {
                    linkReady = false
                    _linkUp.value = false
                    failAll("gauge disconnected")
                    // A dropped link is a ZOMBIE unless the client is torn down
                    // for good: disconnect() alone leaves the ACL up, so the
                    // board never restarts advertising and the next reconnect
                    // attempt times out forever. close() releases the link.
                    val dead = gatt
                    gatt = null
                    controlCharacteristic = null
                    logCharacteristic = null
                    deviceInfoCharacteristic = null
                    scope.launch {
                        runCatching { dead?.disconnect() }
                        runCatching { dead?.close() }
                    }
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
                    GaugeGatt.status -> handleStatusNotify(event.value)
                    else -> Unit
                }
            }
            is GattEvent.CharacteristicRead -> {
                completeWait("read:${event.characteristic.uuid}", event)
            }
            is GattEvent.CharacteristicWritten -> {
                completeWait("write:${event.characteristic.uuid}", event)
                if (event.characteristic.uuid == GaugeGatt.control &&
                    event.status != BluetoothGatt.GATT_SUCCESS
                ) {
                    activeRequestId?.let { id ->
                        pendingResponses.remove(id)?.resumeWith(
                            Result.failure(TransportException("control write failed (${event.status})")),
                        )
                    }
                    activeRequestId = null
                }
            }
            is GattEvent.DescriptorWritten -> {
                completeWait("desc:${event.descriptor.characteristic.uuid}", event)
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
                    if (activeRequestId == event.id) activeRequestId = null
                }
            }
        }
    }

    private fun handleControlNotify(bytes: ByteArray) {
        for (text in controlFramer.append(bytes)) {
            val obj = runCatching { ApiJson.json.parseToJsonElement(text).jsonObject }.getOrNull() ?: continue
            val id = obj["id"]?.jsonPrimitive?.intOrNull ?: continue
            val pending = pendingResponses.remove(id) ?: continue
            if (activeRequestId == id) activeRequestId = null
            val status = obj["status"]?.jsonPrimitive?.intOrNull ?: -1
            val body = obj["body"]
            val bodyText = if (body == null || body is JsonNull) "" else body.toString()
            pending.resumeWith(Result.success(GattResponse(status, bodyText)))
        }
    }

    private fun handleStatusNotify(bytes: ByteArray) {
        for (text in statusFramer.append(bytes)) {
            _statusLine.value = text
        }
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
        activeRequestId = null
        controlFramer.clear()
        statusFramer.clear()
        stopStatusPoll()
    }

    private suspend fun await(
        key: String,
        timeoutMs: Long,
        start: (() -> Boolean)? = null,
        eventMatches: (GattEvent) -> Boolean,
    ): GattEvent {
        // A GATT operation timeout must surface as a TransportException, never a
        // TimeoutCancellationException: the reconnect loop treats
        // CancellationException as "this loop is cancelled" and dies forever,
        // which is exactly the "app never reconnects after restart" bug.
        try {
            return withTimeout(timeoutMs) {
                val event = suspendCancellableCoroutine<GattEvent> { cont ->
                    scope.launch {
                        pendingWaits[key] = cont
                        cont.invokeOnCancellation {
                            scope.launch {
                                if (pendingWaits[key] === cont) pendingWaits.remove(key)
                            }
                        }
                        if (start != null && !runCatching(start).getOrDefault(false)) {
                            pendingWaits.remove(key)
                            cont.resumeWith(Result.failure(TransportException("GATT operation '$key' did not start")))
                        }
                    }
                }
                if (!eventMatches(event)) {
                    throw TransportException("unexpected GATT event for '$key'")
                }
                event
            }
        } catch (e: TimeoutCancellationException) {
            throw TransportException("GATT operation '$key' timed out", e)
        }
    }

    /** Connect, discover, negotiate MTU and subscribe to control notifications. */
    override suspend fun connect() = connectMutex.withLock {
        if (linkReady) return@withLock
        check(!closed) { "transport closed" }
        gatt?.let { stale ->
            runCatching { stale.disconnect() }
            runCatching { stale.close() }
        }
        gatt = null
        _linkUp.value = false
        val device = requireDevice()
        attemptBond(device)
        try {
            val connected = await("connect", CONNECT_TIMEOUT_MS, start = {
                val created = gattFactory(device, callback)
                gatt = created
                created != null
            }) {
                it is GattEvent.ConnectionState && it.status == BluetoothGatt.GATT_SUCCESS
            }
            val g = (connected as GattEvent.ConnectionState).gatt
            await("discover", DISCOVER_TIMEOUT_MS, start = { g.discoverServices() }) {
                it is GattEvent.ServicesDiscovered && it.status == BluetoothGatt.GATT_SUCCESS
            }

            runCatching {
                await("mtu", MTU_TIMEOUT_MS, start = { g.requestMtu(MTU_TARGET) }) {
                    it is GattEvent.MtuChanged
                }
            }

            val service = g.getService(GaugeGatt.service)
                ?: throw TransportException("BoostGauge service not found")
            val control = service.getCharacteristic(GaugeGatt.control)
                ?: throw TransportException("control characteristic missing")
            val log = service.getCharacteristic(GaugeGatt.log)
                ?: throw TransportException("log characteristic missing")
            val info = service.getCharacteristic(GaugeGatt.deviceInfo)
                ?: throw TransportException("device info characteristic missing")
            controlCharacteristic = control
            logCharacteristic = log
            deviceInfoCharacteristic = info
            // The Status characteristic is deliberately NOT subscribed: its full
            // /state mirror (~1 KB) fragments into ~5 ATT notifications per sample,
            // and the flood dropped the BLE link during the Android E2E matrix.
            // Full state is served by the Control /state route via statusPollJob.
            enableNotifyWithStaleBondRecovery(device, control)
            linkReady = true
            _linkUp.value = true
            startStatusPoll()
        } catch (error: Throwable) {
            // A failed or aborted connect must release the freshly created gatt
            // (disconnect alone leaves the ACL up, so the board never restarts
            // advertising) and never masquerade a dead link as healthy.
            val failed = gatt
            gatt = null
            controlCharacteristic = null
            logCharacteristic = null
            deviceInfoCharacteristic = null
            runCatching { failed?.disconnect() }
            runCatching { failed?.close() }
            _linkUp.value = false
            throw error
        }
    }

    /**
     * Poll the Control /state route ~1 Hz so statusLine stays fresh without the
     * Status-characteristic notification flood. Mirrors iOS
     * BleTransport.liveStatusStream (which polls get("state")).
     */
    private fun startStatusPoll() {
        statusPollJob?.cancel()
        statusPollJob = scope.launch {
            while (isActive) {
                if (linkReady) runCatching { get("state") }
                delay(STATUS_POLL_MS)
            }
        }
    }

    private fun stopStatusPoll() {
        statusPollJob?.cancel()
        statusPollJob = null
    }

    private fun hasBondPermission(): Boolean =
        ContextCompat.checkSelfPermission(context, Manifest.permission.BLUETOOTH_CONNECT) ==
            PackageManager.PERMISSION_GRANTED

    private suspend fun attemptBond(device: BluetoothDevice) {
        if (bondOps.bondState(device) == BluetoothDevice.BOND_BONDED) return
        if (!hasBondPermission()) return
        val created = bondOps.createBond(device)
        if (created) {
            try {
                bondOps.awaitBondSettled(device, BOND_TIMEOUT_MS)
            } catch (t: Throwable) {
                // Best-effort bonding; an encrypted service may still reject reads/writes.
                // The encrypted-link check after discovery owns the real gate.
            }
        }
    }

    /**
     * Subscribe to Control notifications, treating an insufficient encrypted
     * link or a failed CCCD write as a stale-bond condition. On detection the
     * bond is removed and recreated fresh, then the CCCD write is retried
     * exactly once. Recovery never runs more than once per connect() call:
     * if a recovered subscribe still fails, connect() reports the error and
     * the repository backoff loop retries from a clean bond state.
     */
    private suspend fun enableNotifyWithStaleBondRecovery(
        device: BluetoothDevice,
        characteristic: BluetoothGattCharacteristic,
    ) {
        var recovered = false
        if (!bondOps.isLinkEncrypted(device)) {
            recoverStaleBond(device)
            recovered = true
        }
        try {
            enableNotify(characteristic)
        } catch (error: TransportException) {
            if (recovered) throw error
            recoverStaleBond(device)
            recovered = true
            enableNotify(characteristic)
        }
    }

    /**
     * Stale-bond recovery: remove the bond (best-effort reflection, guards
     * against the stack cache that survives Settings "no bond"), wait for
     * BOND_NONE (bounded ~5 s), re-bond fresh and wait for BOND_BONDED
     * (bounded 15 s). Must be invoked at most once per connect() by the
     * caller.
     */
    private suspend fun recoverStaleBond(device: BluetoothDevice) {
        if (!hasBondPermission()) {
            throw TransportException("Bluetooth connect permission missing for stale-bond recovery")
        }
        // Best-effort remove; the stack may already report BOND_NONE while its
        // key cache still holds the old LTK, so the NONE wait below is also the
        // flush window.
        bondOps.removeBond(device)
        try {
            bondOps.awaitBondState(device, BluetoothDevice.BOND_NONE, BOND_NONE_TIMEOUT_MS)
        } catch (error: TransportException) {
            throw TransportException("stale bond did not clear within $BOND_NONE_TIMEOUT_MS ms", error)
        }
        if (!bondOps.createBond(device)) {
            throw TransportException("re-bond after stale bond did not start")
        }
        bondOps.awaitBondState(device, BluetoothDevice.BOND_BONDED, BOND_TIMEOUT_MS)
    }

    private suspend fun enableNotify(characteristic: BluetoothGattCharacteristic) {
        val g = requireGatt()
        val descriptor = withContext(bleDispatcher) {
            if (!g.setCharacteristicNotification(characteristic, true)) {
                throw TransportException("setCharacteristicNotification failed")
            }
            characteristic.getDescriptor(GaugeGatt.clientCharacteristicConfig)
                ?: throw TransportException("CCCD missing for ${characteristic.uuid}")
        }
        val key = "desc:${characteristic.uuid}"
        await(key, WRITE_TIMEOUT_MS, start = {
            descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            g.writeDescriptor(descriptor)
        }) {
            it is GattEvent.DescriptorWritten &&
                it.descriptor.characteristic.uuid == characteristic.uuid &&
                it.status == BluetoothGatt.GATT_SUCCESS
        }
    }

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp = mutex.withLock {
        var lastFailure: Throwable? = null
        for ((attempt, delayMs) in RETRY_DELAYS_MS.withIndex()) {
            if (delayMs > 0) kotlinx.coroutines.delay(delayMs)
            try {
                return@withLock sendOnce(method, path, bodyJson)
            } catch (error: Throwable) {
                if (!isTransientRequestFailure(error)) throw error
                lastFailure = error
                if (attempt == RETRY_DELAYS_MS.lastIndex) break
            }
        }
        throw TransportException("BLE request failed after ${RETRY_DELAYS_MS.size} attempts", lastFailure)
    }

    private suspend fun sendOnce(method: String, path: String, bodyJson: String?): Resp {
        if (closed) throw TransportException("transport closed")
        if (!linkReady) throw TransportException("BLE not connected")
        val char = requireControl()
        val id = nextRequestId++
        // GaugeApi uses HTTP-style relative routes ("state", "config", ...),
        // while the GATT contract routes are slash-prefixed ("/state").
        val route = if (path.startsWith('/')) path else "/$path"
        val body: JsonElement = bodyJson?.let {
            runCatching { ApiJson.json.parseToJsonElement(it) }.getOrDefault(buildJsonObject {})
        } ?: buildJsonObject {}
        val request = buildJsonObject {
            put("id", id)
            put("path", route)
            put("method", method.uppercase())
            put("body", body)
        }
        val encoded = request.toString().toByteArray(Charsets.UTF_8)
        if (encoded.size > MAX_REQUEST_BYTES) {
            throw TransportException("BLE request exceeds $MAX_REQUEST_BYTES bytes")
        }
        controlFramer.clear()
        val response = try {
            withTimeout(REQUEST_TIMEOUT_MS) {
                suspendCancellableCoroutine<GattResponse> { cont ->
                    scope.launch {
                        activeRequestId = id
                        pendingResponses[id] = cont
                        cont.invokeOnCancellation {
                            scope.launch {
                                pendingResponses.remove(id)
                                if (activeRequestId == id) activeRequestId = null
                            }
                        }
                        events.trySend(GattEvent.CommandWrite(char, id, encoded))
                    }
                }
            }
        } catch (error: TimeoutCancellationException) {
            throw error
        }
        if (route == "/state") _statusLine.value = response.body
        return Resp(response.status, response.body)
    }

    private fun isTransientRequestFailure(error: Throwable): Boolean =
        error is TimeoutCancellationException ||
            (error is TransportException &&
                (error.message?.contains("write", ignoreCase = true) == true ||
                    error.message?.contains("busy", ignoreCase = true) == true))

    override suspend fun get(path: String): Resp = send("GET", path, null)

    /** Read the Log characteristic (Android reassembles GATT long reads). */
    override suspend fun readLog(): String = mutex.withLock {
        readValue(requireLog(), LOG_READ_TIMEOUT_MS).also { BleLogParser.parse(it) }
    }

    /** Read the DeviceInfo characteristic. */
    override suspend fun readDeviceInfo(): String = mutex.withLock { readValue(requireDeviceInfo()) }

    /**
     * Full /state via the Control route (iOS readStatus parity). The Status
     * characteristic is neither subscribed nor read (its notification flood
     * dropped the Android link), so return the latest polled Control /state
     * sample, or fetch one directly.
     */
    override suspend fun readStatus(): String {
        _statusLine.value?.let { return it }
        val response = send("GET", "state", null)
        if (response.status !in 200..299) {
            throw TransportException("status read failed (${response.status})")
        }
        return response.body
    }

    private suspend fun readValue(
        char: BluetoothGattCharacteristic,
        timeoutMs: Long = READ_TIMEOUT_MS,
    ): String {
        val g = requireGatt()
        val event = await("read:${char.uuid}", timeoutMs, start = { g.readCharacteristic(char) }) {
            it is GattEvent.CharacteristicRead && it.characteristic.uuid == char.uuid
        }
        if (event !is GattEvent.CharacteristicRead || event.status != BluetoothGatt.GATT_SUCCESS) {
            throw TransportException("characteristic read failed")
        }
        return event.value.decodeToString()
    }

    override suspend fun close() {
        if (closed) return
        closed = true
        linkReady = false
        _linkUp.value = false
        failAll("transport closed")
        scope.cancel()
        withContext(Dispatchers.IO) {
            val g = gatt
            gatt = null
            controlCharacteristic = null
            logCharacteristic = null
            deviceInfoCharacteristic = null
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
        private const val LOG_READ_TIMEOUT_MS = 20_000L
        private const val REQUEST_TIMEOUT_MS = 20_000L
        private const val BOND_TIMEOUT_MS = 15_000L
        private const val BOND_NONE_TIMEOUT_MS = 5_000L
        private const val STATUS_POLL_MS = 1_000L
        private val RETRY_DELAYS_MS = longArrayOf(0L, 200L, 800L, 1_500L, 2_500L)
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
        // Scan broadly and filter in the callback. Some controllers do not
        // deliver this legacy advertisement to a hardware service filter when
        // the UUID and local name arrive across ADV + scan-response packets.
        scanner.startScan(null, settings, callback)
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
