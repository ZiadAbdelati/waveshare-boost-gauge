package com.boostgauge.app

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import com.boostgauge.app.ui.BoostGaugeApp
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    companion object {
        var debugInitialSettingsPage: String? = null
        var initialNavRoute: String? = null
    }
    private val notificationPermissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestPermission(),
    ) { }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val isScreenshot = intent.getStringExtra("screenshotState") != null || intent.getStringExtra("transport") == "simBle"
        if (!isScreenshot && Build.VERSION.SDK_INT >= 33) {
            if (ContextCompat.checkSelfPermission(this, Manifest.permission.POST_NOTIFICATIONS)
                != PackageManager.PERMISSION_GRANTED
            ) {
                notificationPermissionLauncher.launch(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        // Keep Settings sub-page direct-open for Connection screenshots.
        val screenshotState = intent.getStringExtra("screenshotState")
        val transportExtra = intent.getStringExtra("transport")
        if (screenshotState == "reconnecting" || screenshotState == "disconnected" ||
            screenshotState == "connected" || screenshotState == "fresh-no-peer" || transportExtra == "simBle"
        ) {
            debugInitialSettingsPage = "Connection"
            initialNavRoute = "settings"
        }
        if (screenshotState == "disconnected-status") {
            initialNavRoute = "status"
        }
        val container = (application as BoostGaugeApp).container
        lifecycleScope.launch {
            // Screenshot states set up the transport/connection state directly
            // and must NOT go through restore()+refresh(): a persisted peer
            // would block refresh() on a real GATT connect for up to 15 s and
            // the capture would land on the pre-override screen.
            when (screenshotState) {
                "connected" -> {
                    debugInitialSettingsPage = "Connection"
                    container.initialize(simBle = true)
                    container.repository.start(lifecycleScope)
                    // simBle is already in place: force a live sample so the
                    // pill reads "Live · BLE" and the saved gauge row is hidden
                    // (never a Connect button on a live link).
                    container.repository.refresh()
                }
                "reconnecting" -> {
                    debugInitialSettingsPage = "Connection"
                    val sel = com.boostgauge.app.data.settings.TransportSelection(
                        com.boostgauge.app.data.settings.TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge",
                    )
                    val dummy = object : com.boostgauge.app.data.transport.BleGaugeTransport {
                        override val transportKind = "BLE"
                        override val statusLine = kotlinx.coroutines.flow.MutableStateFlow<String?>(null)
                        override val linkUp = kotlinx.coroutines.flow.MutableStateFlow(false)
                        override suspend fun get(path: String): com.boostgauge.app.data.transport.Resp {
                            val clean = path.substringBefore("?")
                            if (clean == "state") throw com.boostgauge.app.data.transport.TransportException("not connected")
                            return when (clean) {
                                "config" -> com.boostgauge.app.data.transport.Resp(200, """{"brightnessHigh":92,"brightnessLow":10,"dimSchedule":{"enabled":true,"startMinutes":1380,"endMinutes":360},"timezoneOffsetMinutes":-240,"timezoneTz":"EST5EDT,M3.2.0/2,M11.1.0/2","activeThemeId":"dyno-cell","psiMin":-15.0,"psiMax":10.0,"psiOverboost":8.0,"zeroAngle":90.0,"appBle":false}""")
                                "themes" -> com.boostgauge.app.data.transport.Resp(200, """{"activeThemeId":"dyno-cell","bigDigitStaticBg":true,"bigDigitColorText":true,"bigDigitStaticColor":"#16181c","bigDigitTextColor":"#ffffff","arcGradient":false,"hudGradient":false,"hudTrueBlack":false,"neonMarqueeSpin":false,"teSync":false,"regionDBuf":true,"teScanline":false,"rotation":0,"vaultFace":"#05281a","vaultVignette":22,"vaultNeedleRed":false,"vaultNeedleTail":false,"neonLayout":0,"neonPreset":0,"demoMode":false,"demoFastSweep":false,"tpmsBle":false,"pixelShift":false,"pixelShiftSec":90,"themes":[{"id":"dyno-cell","name":"Dyno Cell","style":"arc","colors":{"face":"#090a0d","track":"#20242c","text":"#f5f7fa","muted":"#8c95a3","vacuum":"#4dd2ff","boost":"#b8f35a","overboost":"#ff4f6d","zero":"#ffffff"},"customized":false},{"id":"vault-tec","name":"Vault-Tec","style":"vault","colors":{"face":"#05281a","track":"#0c3d24","text":"#38f08a","muted":"#1f7a4d","vacuum":"#38f08a","boost":"#38f08a","overboost":"#eafc50","zero":"#38f08a"},"customized":false},{"id":"night-city","name":"Night City","style":"hud","colors":{"face":"#080a08","track":"#1a1c0a","text":"#fcee0a","muted":"#5a7a0a","vacuum":"#00e5ff","boost":"#fcee0a","overboost":"#ff003c","zero":"#00e5ff"},"customized":true},{"id":"big-digit","name":"Big Digit","style":"bigdigit","colors":{"face":"#0b0c0e","track":"#20242c","text":"#ffffff","muted":"#0b0c0e","vacuum":"#4dd2ff","boost":"#b8f35a","overboost":"#ff4f6d","zero":"#ffffff"},"customized":false},{"id":"neon","name":"Neon","style":"neon","colors":{"face":"#000000","track":"#241038","text":"#ffffff","muted":"#5a3a7a","vacuum":"#7b00ff","boost":"#ff2bd6","overboost":"#ff1500","zero":"#ffffff"},"customized":false}]}""")
                                "tpms/config" -> com.boostgauge.app.data.transport.Resp(200, """{"lowKpa":220.0,"lowPsi":31.9,"staleAfterMs":15000}""")
                                else -> com.boostgauge.app.data.transport.Resp(200, "{}")
                            }
                        }
                        override suspend fun send(method: String, path: String, bodyJson: String?) = get(path)
                        override suspend fun readLog(): String = throw com.boostgauge.app.data.transport.TransportException("not connected")
                        override suspend fun readStatus(): String = throw com.boostgauge.app.data.transport.TransportException("not connected")
                    }
                    container.transportController.debugSetForScreenshot(sel, dummy)
                    container.repository.debugSetReconnectState(connected = false, attempt = 3)
                }
                "disconnected" -> {
                    debugInitialSettingsPage = "Connection"
                    // Post-disconnect: the peer stays remembered, so the Connection
                    // page shows the "Saved gauge" row with a Connect action.
                    val sel = com.boostgauge.app.data.settings.TransportSelection(
                        com.boostgauge.app.data.settings.TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge",
                    )
                    container.transportController.debugSetForScreenshot(sel, null)
                    container.repository.onTransportDisconnected()
                }
                "fresh-no-peer" -> {
                    debugInitialSettingsPage = "Connection"
                    // No remembered peer at all: empty selection + null transport
                    // -> pill "Not connected", no saved row.
                    val sel = com.boostgauge.app.data.settings.TransportSelection(
                        com.boostgauge.app.data.settings.TransportType.BLE, "", "", "",
                    )
                    container.transportController.debugSetForScreenshot(sel, null)
                    container.repository.onTransportDisconnected()
                }
                "disconnected-status" -> {
                    // Status page after transport loss: cached payloads reset to
                    // the not-connected placeholders (--.- etc.).
                    val sel = com.boostgauge.app.data.settings.TransportSelection(
                        com.boostgauge.app.data.settings.TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge",
                    )
                    container.transportController.debugSetForScreenshot(sel, null)
                    container.repository.onTransportDisconnected()
                }
                else -> {
                    container.initialize(transportExtra == "simBle")
                    container.repository.start(lifecycleScope)
                    container.repository.refresh()
                }
            }
        }
        setContent {
            BoostGaugeApp(container)
        }
    }
}
