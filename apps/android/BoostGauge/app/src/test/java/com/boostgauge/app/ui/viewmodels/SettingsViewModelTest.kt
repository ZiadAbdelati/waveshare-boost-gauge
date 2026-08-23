package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
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
            selection = MutableStateFlow(TransportSelection(TransportType.HTTP, "192.168.4.1", "")),
            selectTransport = { _, _ -> },
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
}
