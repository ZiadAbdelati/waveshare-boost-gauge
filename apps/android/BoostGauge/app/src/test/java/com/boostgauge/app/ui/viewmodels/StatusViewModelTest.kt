package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import com.boostgauge.app.ui.PressureReferenceState
import com.boostgauge.app.ui.PressureUnitState
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class StatusViewModelTest {

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
    fun stateSampleFlipsReferenceEvenWhenThemesSaidAtmospheric() = runTest(dispatcher) {
        // /themes is the initial-config source and says atmospheric; a live
        // /state sample carrying true must still flip the mirror (the physical
        // panel can change the reference without any /themes refetch).
        val transport = FakeBleTransport { _, path, _ ->
            when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                "config" -> Resp(200, ApiFixtures.CONFIG)
                "state" -> Resp(
                    200,
                    ApiFixtures.STATE.replace("\"pressureAbsolute\": false", "\"pressureAbsolute\": true"),
                )
                else -> Resp(404, "{}")
            }
        }
        val api = GaugeApi { transport }
        val repository = GaugeRepository(api, MutableStateFlow<GaugeTransport?>(transport))
        val reference = PressureReferenceState()
        val viewModel = StatusViewModel(repository, api, PressureUnitState(), reference)
        runCurrent()
        assertFalse("themes fixture is atmospheric", reference.absolute.value)

        repository.refresh()
        runCurrent()

        assertTrue(reference.absolute.value)
        assertTrue(viewModel.pressureAbsolute.value)
    }
}
