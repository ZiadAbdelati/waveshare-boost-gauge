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
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ThemesViewModelTest {

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
    fun loadsThemeListFromFirmwareShape() = runTest(dispatcher) {
        val transport = FakeBleTransport { _, path, _ ->
            assertEquals("themes", path)
            Resp(200, ApiFixtures.THEMES)
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })

        viewModel.state.first { !it.loading }

        val state = viewModel.state.value
        assertEquals(5, state.themes.size)
        assertEquals("Dyno Cell", state.themes.first().name)
        assertEquals("Neon", state.themes.last().name)
        assertEquals("dyno-cell", state.activeThemeId)
    }

    @Test
    fun activatePutsActiveThemeAndRefreshesState() = runTest(dispatcher) {
        val transport = FakeBleTransport { method, path, body ->
            when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                "themes/active" -> {
                    assertEquals("PUT", method)
                    assertTrue(body!!.contains("\"id\":\"neon\""))
                    Resp(200, ApiFixtures.THEMES.replace("\"activeThemeId\": \"dyno-cell\"", "\"activeThemeId\": \"neon\""))
                }
                else -> Resp(404, "{}")
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.activate("neon")

        viewModel.state.first { it.activatingId == null && !it.loading && it.activeThemeId == "neon" }
        assertEquals("neon", viewModel.state.value.activeThemeId)
        assertTrue(transport.requests.any { it.path == "themes/active" && it.method == "PUT" })
    }

    @Test
    fun loadFailureSurfacesError() = runTest(dispatcher) {
        val transport = FakeBleTransport { _, _, _ ->
            Resp(500, """{"error":"themes_truncated"}""")
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })

        viewModel.state.first { !it.loading }

        assertTrue(viewModel.state.value.error != null)
        assertTrue(viewModel.state.value.themes.isEmpty())
    }
}
