package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class CalibrationViewModelTest {

    private val dispatcher = StandardTestDispatcher()

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    @Test
    fun loadSuccessAlwaysMapsToContentState() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            assertEquals("GET", method)
            assertEquals("sensors/calibration", path)
            Resp(200, ApiFixtures.CALIBRATION)
        }
        val viewModel = CalibrationViewModel(GaugeApi { transport })

        viewModel.state.first { !it.loading }

        val state = viewModel.state.value
        // The screen's exhaustive when() renders the full content branch:
        // live diagnostics + saved calibration + Calibrate button.
        assertEquals(CalibrationViewModel.UiMode.CONTENT, state.mode())
        assertNotNull(state.calibration)
        assertEquals(1.1821, state.calibration!!.live.mapVolts, 0.0001)
        assertTrue(state.calibration!!.calibration.valid)
        assertNull(state.error)
    }

    @Test
    fun loadFailureMapsToErrorState() = runTest(dispatcher) {
        val transport = FakeBleTransport { _, _, _ ->
            Resp(500, """{"error":"sensor_bus_down"}""")
        }
        val viewModel = CalibrationViewModel(GaugeApi { transport })

        viewModel.state.first { !it.loading }

        val state = viewModel.state.value
        assertEquals(CalibrationViewModel.UiMode.ERROR, state.mode())
        assertEquals("sensor_bus_down", state.error)
        assertNull(state.calibration)
    }

    @Test
    fun calibrateSuccessFoldsUpdatedCalibrationBackIntoState() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, _ ->
            when {
                path == "sensors/calibration" && method == "GET" -> Resp(200, ApiFixtures.CALIBRATION)
                path == "sensors/calibration" && method == "POST" -> {
                    Resp(200, ApiFixtures.CALIBRATION.replace("\"version\": 3", "\"version\": 4"))
                }
                else -> Resp(404, "{}")
            }
        }
        val viewModel = CalibrationViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.calibrate()
        runCurrent()

        assertEquals("Calibration stored", viewModel.state.value.message)
        assertEquals(4, viewModel.state.value.calibration!!.calibration.version)
        assertEquals(CalibrationViewModel.UiMode.CONTENT, viewModel.state.value.mode())
    }
}
