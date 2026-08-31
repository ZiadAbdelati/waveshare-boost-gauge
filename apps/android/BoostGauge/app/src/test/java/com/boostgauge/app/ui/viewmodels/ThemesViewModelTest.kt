package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import com.boostgauge.app.data.transport.TransportException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
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
    fun activateSucceedsWhileReconnectingWithoutBlocking() = runTest(dispatcher) {
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
        // The reconnect loop is actively retrying: the theme apply must still
        // complete promptly (bounded, never queued behind the reconnect path).
        val viewModel = ThemesViewModel(
            GaugeApi { transport },
            MutableStateFlow(ConnectionStatus.Reconnecting),
        )
        viewModel.state.first { !it.loading }

        viewModel.activate("neon")
        // Completes without blocking: spinner clears and the theme applies.
        viewModel.state.first { it.activatingId == null && it.activeThemeId == "neon" }
        assertEquals("neon", viewModel.state.value.activeThemeId)
        assertNull(viewModel.state.value.error)
        assertTrue(transport.requests.any { it.path == "themes/active" && it.method == "PUT" })
    }

    @Test
    fun activateDoesNotSpinForeverOnHangingLink() = runTest(dispatcher) {
        // A half-dead BLE link would otherwise hold the request for the
        // transport's full retry ladder (~20s × 5): the ViewModel must bound it.
        // Load paths answer fast; the activation PUT hangs indefinitely.
        val hanging = object : GaugeTransport {
            override suspend fun get(path: String): Resp = when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                else -> Resp(404, "{}")
            }

            override suspend fun send(method: String, path: String, bodyJson: String?): Resp =
                if (path == "themes/active") {
                    delay(100_000L)
                    Resp(404, "{}")
                } else {
                    get(path)
                }
        }
        val viewModel = ThemesViewModel(
            GaugeApi { hanging },
            MutableStateFlow(ConnectionStatus.Reconnecting),
        )
        viewModel.state.first { !it.loading }

        viewModel.activate("neon")
        runCurrent()
        assertTrue("spinner must be visible while the request is in flight", viewModel.state.value.activatingId == "neon")

        // The 10s bound fires and clears the spinner with an error — never 30s+.
        advanceTimeBy(ThemesViewModel.THEME_OP_TIMEOUT_MS + 1_000L)
        runCurrent()

        assertNull(viewModel.state.value.activatingId)
        assertTrue(viewModel.state.value.error != null)
    }

    @Test
    fun staleActivationResponseCannotOverwriteNewerSelection() = runTest(dispatcher) {
        // Tap big-digit then neon; the newer neon request answers first and its
        // selection is applied. The stale big-digit response lands late, after
        // activatingId has cleared — it must be dropped, never accepted through
        // the `activatingId == null` branch.
        val stale = ApiFixtures.THEMES.replace("\"activeThemeId\": \"dyno-cell\"", "\"activeThemeId\": \"big-digit\"")
        val newest = ApiFixtures.THEMES.replace("\"activeThemeId\": \"dyno-cell\"", "\"activeThemeId\": \"neon\"")
        val outOfOrder = object : GaugeTransport {
            override suspend fun get(path: String): Resp = when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                else -> Resp(404, "{}")
            }

            override suspend fun send(method: String, path: String, bodyJson: String?): Resp = when {
                path == "themes/active" && bodyJson?.contains("\"id\":\"big-digit\"") == true -> {
                    delay(1_000L)
                    Resp(200, stale)
                }
                path == "themes/active" && bodyJson?.contains("\"id\":\"neon\"") == true -> {
                    delay(100L)
                    Resp(200, newest)
                }
                else -> get(path)
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { outOfOrder })
        viewModel.activate("big-digit")
        viewModel.activate("neon")
        testScheduler.advanceUntilIdle()

        assertEquals("neon", viewModel.state.value.activeThemeId)
        assertNull(viewModel.state.value.activatingId)
    }

    @Test
    fun staleLoadCannotClobberNewerSelection() = runTest(dispatcher) {
        // Real-device probe: the initial load's GET /themes is held in flight
        // (a slow BLE read queued ahead of the user's tap). The tap activates
        // neon and its echo applies; when the stale GET finally lands it still
        // carries the PRE-switch activeThemeId (dyno-cell). load()'s apply must
        // not overwrite the newer selection.
        val gate = CompletableDeferred<Unit>()
        val newest = ApiFixtures.THEMES.replace("\"activeThemeId\": \"dyno-cell\"", "\"activeThemeId\": \"neon\"")
        val gated = object : GaugeTransport {
            override suspend fun get(path: String): Resp = when (path) {
                "themes" -> {
                    gate.await()
                    Resp(200, ApiFixtures.THEMES)
                }
                "config" -> Resp(200, ApiFixtures.CONFIG)
                "state" -> Resp(200, ApiFixtures.STATE)
                else -> Resp(404, "{}")
            }

            override suspend fun send(method: String, path: String, bodyJson: String?): Resp = when (path) {
                "themes/active" -> Resp(200, newest)
                else -> Resp(404, "{}")
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { gated })
        runCurrent()
        // load() is suspended on the gate; nothing has applied yet.
        viewModel.activate("neon")
        runCurrent()
        assertEquals("neon", viewModel.state.value.activeThemeId)

        // The stale GET (captured before the PUT) returns dyno-cell. The
        // seq-guarded load apply must drop it, keeping the newer selection.
        gate.complete(Unit)
        runCurrent()

        assertEquals("neon", viewModel.state.value.activeThemeId)
    }

    @Test
    fun lostActivationEchoReconcilesBoardState() = runTest(dispatcher) {
        // Real-device probe: the PUT always reaches the board, but the echoed
        // ThemeList response is lost (BLE response-notification path down). The
        // board applies neon; the app must reconcile with a fresh GET and adopt
        // the board's real state instead of freezing on the old theme.
        var putReachedBoard = false
        val neonActive = ApiFixtures.THEMES.replace("\"activeThemeId\": \"dyno-cell\"", "\"activeThemeId\": \"neon\"")
        val transport = object : GaugeTransport {
            override suspend fun get(path: String): Resp = when (path) {
                "themes" -> Resp(200, if (putReachedBoard) neonActive else ApiFixtures.THEMES)
                else -> Resp(404, "{}")
            }

            override suspend fun send(method: String, path: String, bodyJson: String?): Resp {
                if (path == "themes/active") {
                    putReachedBoard = true
                    throw TransportException("response lost")
                }
                return get(path)
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.activate("neon")
        testScheduler.advanceUntilIdle()

        assertEquals("neon", viewModel.state.value.activeThemeId)
        assertNull(viewModel.state.value.error)
        assertNull(viewModel.state.value.activatingId)
    }

    @Test
    fun failedActivationKeepsErrorWhenBoardDidNotSwitch() = runTest(dispatcher) {
        // The PUT never reached the board (link dropped). The reconcile GET
        // confirms the board is still on the old theme: the failure stands and
        // the row reflects the board's real state.
        val transport = object : GaugeTransport {
            override suspend fun get(path: String): Resp = when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                else -> Resp(404, "{}")
            }

            override suspend fun send(method: String, path: String, bodyJson: String?): Resp =
                throw TransportException("not connected")
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.activate("neon")
        testScheduler.advanceUntilIdle()

        assertEquals("dyno-cell", viewModel.state.value.activeThemeId)
        assertNull(viewModel.state.value.activatingId)
        assertTrue(viewModel.state.value.error != null)
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

    @Test
    fun updateThemeOptionsPutsConfigAndRefreshesState() = runTest(dispatcher) {
        var updatedConfigJson = ""
        val transport = FakeBleTransport { method, path, body ->
            when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                "themes/config" -> {
                    assertEquals("PUT", method)
                    updatedConfigJson = body ?: ""
                    Resp(200, ApiFixtures.THEMES)
                }
                else -> Resp(404, "{}")
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.updateVaultNeedleTail(true)
        viewModel.updateVaultFace("green")
        viewModel.saveOptions("vault-tec")

        testScheduler.advanceUntilIdle()

        assertTrue(updatedConfigJson.contains("\"id\":\"vault-tec\""))
        assertTrue(updatedConfigJson.contains("\"vaultNeedleTail\":true"))
        assertTrue(updatedConfigJson.contains("\"vaultFace\":\"green\""))
    }

    @Test
    fun resetColorsCallsThemesConfigWithReset() = runTest(dispatcher) {
        var resetBody = ""
        val transport = FakeBleTransport { method, path, body ->
            when (path) {
                "themes" -> Resp(200, ApiFixtures.THEMES)
                "themes/config" -> {
                    assertEquals("PUT", method)
                    resetBody = body ?: ""
                    Resp(200, ApiFixtures.THEMES)
                }
                else -> Resp(404, "{}")
            }
        }
        val viewModel = ThemesViewModel(GaugeApi { transport })
        viewModel.state.first { !it.loading }

        viewModel.resetColors("dyno-cell")

        testScheduler.advanceUntilIdle()

        assertTrue(resetBody.contains("\"id\":\"dyno-cell\""))
        assertTrue(resetBody.contains("\"reset\":true"))
    }
}
