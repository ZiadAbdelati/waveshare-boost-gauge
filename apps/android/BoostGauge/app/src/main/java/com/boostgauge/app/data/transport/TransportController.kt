package com.boostgauge.app.data.transport

import android.content.Context
import com.boostgauge.app.data.service.ForegroundServiceLauncher
import com.boostgauge.app.data.service.RealForegroundServiceLauncher
import com.boostgauge.app.data.settings.SettingsStore
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportSettingsStore
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.BleGaugeTransport
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch

typealias TransportFactory = (type: TransportType, address: String) -> GaugeTransport

/** Owns exactly one active transport and persists the selection in DataStore. */
class TransportController(
    private val context: Context,
    private val settingsStore: TransportSettingsStore,
    private val transportFactory: TransportFactory = { type, address ->
        when (type) {
            TransportType.HTTP -> HttpTransport(address)
            TransportType.BLE -> BleTransport(context, address)
        }
    },
    private val serviceLauncher: ForegroundServiceLauncher = RealForegroundServiceLauncher(context),
) {
    // App-lifetime scope for background connect attempts (restore/select must
    // never block startup or the UI on a slow or absent board).
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private val _selection = MutableStateFlow(
        TransportSelection(TransportType.BLE, "", "")
    )
    val selection: StateFlow<TransportSelection> = _selection.asStateFlow()

    private val _transport = MutableStateFlow<GaugeTransport?>(null)
    val transport: StateFlow<GaugeTransport?> = _transport.asStateFlow()

    suspend fun restore() {
        val saved = settingsStore.selection.first()
        // Bluetooth-first: never restore HTTP as the active transport. Any
        // persisted HTTP selection (legacy 192.168.4.1 SoftAP default) is
        // migrated to BLE with an empty address so the user picks a device.
        if (saved.type == TransportType.HTTP) {
            val bleEmpty = TransportSelection(TransportType.BLE, "", "", "")
            _selection.value = bleEmpty
            _transport.value = null
            // Persist the migration so the legacy HTTP default never resurfaces.
            runCatching { settingsStore.setTransport(TransportType.BLE, "", "") }
            return
        }
        if (saved.bleAddress.isBlank()) {
            // No known peer yet — stay disconnected, do not fall back to HTTP.
            _selection.value = saved.copy(type = TransportType.BLE)
            _transport.value = null
            // If the store still held an HTTP default, overwrite it.
            if (saved.httpAddress == "192.168.4.1") {
                runCatching { settingsStore.setTransport(TransportType.BLE, "", "") }
            }
            return
        }
        val restored = transportFactory(TransportType.BLE, saved.bleAddress)
        _selection.value = saved
        _transport.value = restored
        // Never block startup on connect: a board that is not advertising (e.g.
        // after a previous session leaked its GATT and the ACL stayed up) would
        // stall MainActivity for the whole connect timeout. The repository
        // reconnect loop also drives connect() — both are serialized by the
        // transport's connect mutex. Start the foreground service only once the
        // link is actually up.
        scope.launch {
            val connected = runCatching { (restored as? BleGaugeTransport)?.connect() ?: Unit }.isSuccess
            if (connected) {
                runCatching { serviceLauncher.startBleService(saved.bleName.ifBlank { "BoostGauge" }) }
            }
        }
    }

    suspend fun select(
        type: TransportType,
        address: String,
        name: String? = null,
        persist: Boolean = true,
    ) {
        val new = transportFactory(type, address)
        val old = _transport.value
        runCatching { old?.close() }
        _selection.value = TransportSelection(
            type = type,
            httpAddress = if (type == TransportType.HTTP) {
                address
            } else {
                _selection.value.httpAddress
            },
            bleAddress = if (type == TransportType.BLE) address else _selection.value.bleAddress,
            bleName = if (type == TransportType.BLE) {
                name?.takeIf { it.isNotBlank() } ?: _selection.value.bleName.ifBlank { "BoostGauge" }
            } else {
                _selection.value.bleName
            },
        )
        // Publish the transport FIRST so the repository reconnect loop has a
        // peer to drive; a failed initial connect is not fatal — the loop keeps
        // retrying and the UI shows "Reconnecting… (attempt N)", never a dead
        // error state with no way forward.
        _transport.value = new
        if (new is BleGaugeTransport) {
            val connected = runCatching { new.connect() }.isSuccess
            if (connected) {
                runCatching { serviceLauncher.startBleService(name?.takeIf { it.isNotBlank() } ?: "BoostGauge") }
            }
        }
        if (persist) {
            settingsStore.setTransport(type, address, name)
        }
    }

    /**
     * Explicit user disconnect: tears down the GATT session and stops the
     * foreground service, but KEEPS the remembered peer (name + address) so the
     * Connection page can offer a "Saved gauge" Connect row. The transport
     * being null is the disconnected signal; a fresh app launch still restores
     * this peer and auto-reconnects (BLE session resilience).
     */
    suspend fun disconnect() {
        runCatching { _transport.value?.close() }
        _transport.value = null
        runCatching { serviceLauncher.stopBleService() }
    }

    /** Forget the saved gauge: disconnect AND erase the persisted peer, so the
     *  Connection page drops the saved row and a fresh scan re-adds it. */
    suspend fun forget() {
        disconnect()
        runCatching { settingsStore.setTransport(TransportType.BLE, "", "") }
        _selection.value = TransportSelection(TransportType.BLE, "", "", "")
    }

    /** Screenshot/test hook: directly set the displayed selection and transport. */
    fun debugSetForScreenshot(selection: TransportSelection, transport: GaugeTransport?) {
        _selection.value = selection
        _transport.value = transport
    }

    fun current(): GaugeTransport =
        checkNotNull(_transport.value) { "no transport selected" }

    /**
     * Debug/emulator hook: swap in the in-process BLE simulator without
     * touching the persisted selection. Triggered by the launch Intent extra
     * `transport=simBle` (the emulator cannot do real BLE).
     */
    suspend fun useSimBle() {
        val old = _transport.value
        runCatching { old?.close() }
        _transport.value = SimBleTransport()
        _selection.value = TransportSelection(
            type = TransportType.BLE,
            httpAddress = _selection.value.httpAddress,
            bleAddress = "sim",
            bleName = "Simulated BoostGauge",
        )
    }
}
