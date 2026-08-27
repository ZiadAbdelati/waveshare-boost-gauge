package com.boostgauge.app.ui.viewmodels

import com.boostgauge.app.data.GaugeRepository
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test

/**
 * Per-window decode cache: switching 1m/5m/15m chips must never re-fetch a
 * window already decoded, and a device restart (uptime reset) must drop the
 * stale cache so a cached window never describes a previous boot.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class LogsViewModelCacheTest {

    private val dispatcher = StandardTestDispatcher()

    @Before
    fun setUp() {
        Dispatchers.setMain(dispatcher)
    }

    @After
    fun tearDown() {
        Dispatchers.resetMain()
    }

    private fun payload(count: Int, newestMs: Long): Resp {
        val samples = (0 until count).joinToString(",") { i ->
            """{"tMs":${newestMs - (count - 1 - i) * 200},"psi":$i,"peakPsi":10.0,"zone":"BOOST","demo":true}"""
        }
        return Resp(200, """{"samples":[$samples]}""")
    }

    private fun newViewModel(transport: FakeBleTransport): LogsViewModel {
        val api = GaugeApi { transport }
        val repository = GaugeRepository(api, MutableStateFlow<GaugeTransport?>(transport))
        // Inject the test dispatcher so the ioDispatcher hop stays deterministic.
        return LogsViewModel(api = api, repository = repository, ioDispatcher = dispatcher)
    }

    private fun FakeBleTransport.logFetches() = requests.count { it.path.startsWith("logs?") }

    @Test
    fun chipSwitchServesCachedWindowsWithoutRefetch() = runTest(dispatcher) {
        val transport = FakeBleTransport { _, path, _ ->
            when {
                path.startsWith("logs?") -> {
                    val limit = path.substringAfter("limit=").toIntOrNull() ?: 1500
                    payload(limit, newestMs = 1_000_000L)
                }
                else -> Resp(404, "{}")
            }
        }
        val vm = newViewModel(transport)
        runCurrent() // init loads 5m
        assertEquals(1, transport.logFetches())
        assertEquals(1500, vm.state.value.samples.size)

        vm.load(300) // 1m — first visit fetches
        runCurrent()
        vm.load(4500) // 15m — first visit fetches
        runCurrent()
        assertEquals(3, transport.logFetches())

        vm.load(1500) // back to 5m — served from cache, no fetch
        runCurrent()
        assertEquals(1500, vm.state.value.samples.size)
        assertEquals("Last 5 minutes · 1500 samples", vm.state.value.source)
        vm.load(300) // back to 1m — served from cache, no fetch
        runCurrent()
        assertEquals(300, vm.state.value.samples.size)
        assertEquals("Last 1 minute · 300 samples", vm.state.value.source)
        assertEquals(3, transport.logFetches())
    }

    @Test
    fun forceRefreshRefetchesOnlyTheCurrentWindow() = runTest(dispatcher) {
        val transport = FakeBleTransport { _, path, _ ->
            when {
                path.startsWith("logs?") -> {
                    val limit = path.substringAfter("limit=").toIntOrNull() ?: 1500
                    payload(limit, newestMs = 1_000_000L)
                }
                else -> Resp(404, "{}")
            }
        }
        val vm = newViewModel(transport)
        runCurrent()
        vm.load(300)
        runCurrent()
        vm.load(4500)
        runCurrent()
        val fetchesBefore = transport.logFetches()

        vm.load(force = true) // refresh current window (4500)
        runCurrent()
        assertEquals(fetchesBefore + 1, transport.logFetches())

        vm.load(300) // cached window still served
        runCurrent()
        assertEquals(fetchesBefore + 1, transport.logFetches())
    }

    @Test
    fun deviceRestartDropsCachedWindowsFromPreviousBoot() = runTest(dispatcher) {
        var newestMs = 1_000_000L
        val transport = FakeBleTransport { _, path, _ ->
            when {
                path.startsWith("logs?") -> {
                    val limit = path.substringAfter("limit=").toIntOrNull() ?: 1500
                    payload(limit, newestMs = newestMs)
                }
                else -> Resp(404, "{}")
            }
        }
        val vm = newViewModel(transport)
        runCurrent()
        vm.load(300)
        runCurrent()
        assertEquals(2, transport.logFetches())

        newestMs = 5_000L // device rebooted: ring uptime reset
        vm.load(4500)
        runCurrent()

        // 5m was cached from the previous boot — now it must re-fetch.
        val fetchesBefore = transport.logFetches()
        vm.load(1500)
        runCurrent()
        assertEquals(fetchesBefore + 1, transport.logFetches())
    }
}