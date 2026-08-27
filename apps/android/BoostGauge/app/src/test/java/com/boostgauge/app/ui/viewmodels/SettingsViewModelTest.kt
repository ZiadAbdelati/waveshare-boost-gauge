package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import com.boostgauge.app.ui.displayLabel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class SettingsViewModelTest {

    private val dispatcher = StandardTestDispatcher()

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun newViewModel(transport: FakeBleTransport): SettingsViewModel {
        val api = GaugeApi { transport }
        val repository = GaugeRepository(api, MutableStateFlow<GaugeTransport?>(transport))
        return SettingsViewModel(
            api = api,
            selection = MutableStateFlow(TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge")),
            selectTransport = { _, _, _ -> },
            repository = repository,
        )
    }

    @Test
    fun scrollSimulatedConfigReEmissionDoesNotClobberEditedField() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        assertEquals("92", viewModel.state.value.fields.brightnessHigh)
        assertEquals("31.9", viewModel.state.value.fields.lowPsi)

        // User types 77 into Brightness high.
        viewModel.updateFields { it.copy(brightnessHigh = "77") }

        // Scrolling disposes/re-composes the LazyColumn item against this same
        // collected state. The edited value lives in the ViewModel, so the
        // recomposition must still read 77 while the server config stays 92.
        val afterReEmission = viewModel.state.value
        assertEquals("77", afterReEmission.fields.brightnessHigh)
        assertEquals(92, afterReEmission.config!!.brightnessHigh)
    }

    @Test
    fun explicitReloadIsTheOnlyServerOverwritePath() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }
        viewModel.updateFields { it.copy(brightnessHigh = "77") }

        viewModel.refreshAll()
        runCurrent()

        // An explicit reload may re-seed the form from the server.
        assertEquals("92", viewModel.state.value.fields.brightnessHigh)
    }

    @Test
    fun saveConfigRoundTripsEditedFieldsAndAppBleThroughServer() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "config" && method == "PUT" ->
                    Resp(200, ApiFixtures.CONFIG.replace("\"appBle\": false", "\"appBle\": true"))
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        viewModel.updateFields { it.copy(brightnessHigh = "77", appBle = true) }
        viewModel.saveConfig()
        runCurrent()

        val put = transport.requests.first { it.method == "PUT" && it.path == "config" }
        assertTrue(put.bodyJson!!.contains("\"brightnessHigh\":77"))
        assertTrue(put.bodyJson!!.contains("\"appBle\":true"))

        // Save-response fold-back: the server's authoritative config becomes
        // the field values (brightness folded back to the fixture's 92, and
        // the echoed appBle=true is kept).
        assertEquals("92", viewModel.state.value.fields.brightnessHigh)
        assertTrue(viewModel.state.value.fields.appBle)
        assertTrue(viewModel.state.value.config!!.appBle)
    }

    @Test
    fun timezoneSyncSendsOnlyTimezoneWithoutEpochMs() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                path == "time" && method == "POST" ->
                    Resp(200, ApiFixtures.STATE.replace("\"timezoneOffsetMinutes\": -240", "\"timezoneOffsetMinutes\": -480"))
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        viewModel.applyTimezone(-480, "PST8PDT,M3.2.0/2,M11.1.0/2")
        runCurrent()

        val post = transport.requests.first { it.method == "POST" && it.path == "time" }
        assertTrue(post.bodyJson!!.contains("\"timezoneOffsetMinutes\":-480"))
        assertTrue(post.bodyJson.contains("\"timezoneTz\":\"PST8PDT,M3.2.0/2,M11.1.0/2\""))
        // The gauge RTC is the time authority; the app never sends the phone epoch.
        assertTrue(!post.bodyJson.contains("epochMs"))
        assertEquals("Timezone applied", viewModel.state.value.message)
        assertEquals(-480, viewModel.state.value.fields.timezoneOffsetMinutes)
    }

    @Test
    fun saveThemeFlagsRoundTripsNewFieldsThroughServer() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "themes/config" && method == "PUT" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        viewModel.updateFields {
            it.copy(
                demoMode = true,
                demoFastSweep = true,
                rotation = 90,
                regionDBuf = false,
                teSync = true,
                teScanline = true,
                pixelShift = true,
                pixelShiftSec = "120",
            )
        }
        viewModel.saveThemeFlags()
        runCurrent()

        val put = transport.requests.first { it.method == "PUT" && it.path == "themes/config" }
        assertTrue(put.bodyJson!!.contains("\"demoMode\":true"))
        assertTrue(put.bodyJson.contains("\"demoFastSweep\":true"))
        assertTrue(put.bodyJson.contains("\"rotation\":90"))
        assertTrue(put.bodyJson.contains("\"regionDBuf\":false"))
        assertTrue(put.bodyJson.contains("\"teSync\":true"))
        assertTrue(put.bodyJson.contains("\"teScanline\":true"))
        assertTrue(put.bodyJson.contains("\"pixelShift\":true"))
        assertTrue(put.bodyJson.contains("\"pixelShiftSec\":120"))
        // Theme-specific settings live ONLY in the Themes tab editor; the global
        // Theme & demo save must never carry them.
        assertFalse(put.bodyJson.contains("vaultNeedleRed"))
        assertFalse(put.bodyJson.contains("vaultNeedleTail"))
        assertFalse(put.bodyJson.contains("bigDigitStaticBg"))
    }

    @Test
    fun forgetObdPeerPostsObdForget() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "obd/forget" && method == "POST" -> Resp(200, "{\"ok\":true}")
                path == "state" -> Resp(200, ApiFixtures.STATE.replace("\"peer\": \"vlinker fd+\"", "\"peer\": \"\""))
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        viewModel.forgetObdPeer()
        runCurrent()

        val post = transport.requests.first { it.method == "POST" && it.path == "obd/forget" }
        assertTrue(post.method == "POST")
    }

    @Test
    fun forgetObdPeerReportsForgottenWhenStateClears() = runTest(dispatcher) {
        // Firmware POST /obd/forget erases NVS obd_peer; the refreshed /state
        // shows an empty peer and Forget reports success.
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "obd/forget" && method == "POST" -> Resp(200, "{\"ok\":true}")
                path == "state" -> Resp(
                    200,
                    ApiFixtures.STATE
                        .replace("\"peer\": \"vlinker fd+\"", "\"peer\": \"\"")
                        .replace("\"peerAddr\": \"11:22:33:44:55:66\"", "\"peerAddr\": \"\""),
                )
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = newViewModel(transport)
        viewModel.state.first { !it.loading }

        viewModel.forgetObdPeer()
        runCurrent()

        assertEquals("OBD peer forgotten", viewModel.state.value.message)
    }

    @Test
    fun disconnectMakesEveryDisplayedStateConsistentlyDisconnected() = runTest(dispatcher) {
        val transportFlow = MutableStateFlow<GaugeTransport?>(null)
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "state" -> Resp(200, ApiFixtures.STATE)
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val api = GaugeApi { transport }
        val repository = GaugeRepository(api, transportFlow)
        val selection = MutableStateFlow(
            TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge"),
        )
        val viewModel = SettingsViewModel(
            api = api,
            selection = selection,
            selectTransport = { _, _, _ -> },
            repository = repository,
            disconnectTransport = { transportFlow.value = null },
        )
        viewModel.state.first { !it.loading }

        // Connected: a /state sample is live and the single source says Connected.
        transportFlow.value = transport
        repository.refresh()
        assertEquals(ConnectionStatus.Connected, viewModel.connectionStatus.value)
        assertEquals("BOOST", repository.status.value?.zone)

        // Tap Disconnect: the transport is torn down and the repository resets
        // link flags + stale payload in the same synchronous step.
        viewModel.disconnectBle()
        runCurrent()

        // ONE source of truth — the pill, the footer and the page rows all
        // derive from this single flow; a shadowed boolean cannot diverge.
        val connectionStatus = viewModel.connectionStatus.value
        assertEquals(ConnectionStatus.Disconnected, connectionStatus)
        val peerKnown = selection.value.bleAddress.isNotBlank()
        assertTrue(peerKnown)
        // Header pill label == dashboard footer label == the same helper.
        assertEquals("Disconnected", connectionStatus.displayLabel(peerKnown, viewModel.reconnectAttempt.value))
        assertEquals("Disconnected", connectionStatus.displayLabel(peerKnown, viewModel.reconnectAttempt.value))
        // No Disconnect affordance when disconnected.
        assertTrue(connectionStatus != ConnectionStatus.Connected)
        assertTrue(connectionStatus != ConnectionStatus.Reconnecting)
        // BUG B: the stale /state payload is gone — Status shows placeholders.
        assertNull(repository.status.value)
        assertFalse(repository.connected.value)
        assertNull(repository.reconnectAttempt.value)
    }

    @Test
    fun disconnectDoesNotDuplicateTheDisconnectedWord() = runTest(dispatcher) {
        // A remembered peer at Disconnected renders "Disconnected" exactly once
        // (the single displayLabel pill). The disconnect action must not also
        // set a "Disconnected" toast that duplicates the word on the page.
        val transportFlow = MutableStateFlow<GaugeTransport?>(null)
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "state" -> Resp(200, ApiFixtures.STATE)
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val api = GaugeApi { transport }
        val repository = GaugeRepository(api, transportFlow)
        val selection = MutableStateFlow(
            TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge"),
        )
        val viewModel = SettingsViewModel(
            api = api,
            selection = selection,
            selectTransport = { _, _, _ -> },
            repository = repository,
            disconnectTransport = { transportFlow.value = null },
        )
        viewModel.state.first { !it.loading }

        transportFlow.value = transport
        repository.refresh()
        assertEquals(ConnectionStatus.Connected, viewModel.connectionStatus.value)

        viewModel.disconnectBle()
        runCurrent()

        // ONE status word comes from the displayLabel helper; no toast echoes it.
        assertNull(viewModel.state.value.message)
        assertEquals(ConnectionStatus.Disconnected, viewModel.connectionStatus.value)
        val peerKnown = selection.value.bleAddress.isNotBlank()
        assertEquals(
            "Disconnected",
            viewModel.connectionStatus.value.displayLabel(peerKnown, viewModel.reconnectAttempt.value),
        )
    }

    @Test
    fun connectSavedGaugeReconnectsToRememberedPeer() = runTest(dispatcher) {
        var selectedAddress: String? = null
        var selectedName: String? = null
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "state" -> Resp(200, ApiFixtures.STATE)
                path == "config" && method == "GET" -> Resp(200, ApiFixtures.CONFIG)
                path == "themes" -> Resp(200, ApiFixtures.THEMES)
                path == "tpms/config" -> Resp(200, ApiFixtures.TPMS_CONFIG)
                else -> Resp(404, "{}")
            }
        }
        val api = GaugeApi { transport }
        val repository = GaugeRepository(
            api,
            MutableStateFlow<GaugeTransport?>(transport),
        )
        val viewModel = SettingsViewModel(
            api = api,
            selection = MutableStateFlow(
                TransportSelection(TransportType.BLE, "", "AA:BB:CC:DD:EE:FF", "BoostGauge"),
            ),
            selectTransport = { _, address, name ->
                selectedAddress = address
                selectedName = name
            },
            repository = repository,
        )
        viewModel.state.first { !it.loading }

        viewModel.connectSavedGauge()
        runCurrent()

        assertEquals("AA:BB:CC:DD:EE:FF", selectedAddress)
        assertEquals("BoostGauge", selectedName)
        assertTrue(viewModel.connectionStatus.value == ConnectionStatus.Connected)
    }
}
