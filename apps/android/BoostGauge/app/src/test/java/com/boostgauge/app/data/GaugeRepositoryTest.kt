package com.boostgauge.app.data

import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class GaugeRepositoryTest {

    @Test
    fun pollLoopRestartsInNewScopeAfterActivityRecreation() = runTest {
        val transport = FakeBleTransport { _, path, _ ->
            Resp(200, ApiFixtures.STATE)
        }
        val repository = GaugeRepository(
            GaugeApi { transport },
            MutableStateFlow<GaugeTransport?>(transport),
        )

        // First Activity instance: onCreate starts the loop in its lifecycleScope.
        val firstScope = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())
        repository.start(firstScope)
        runCurrent()
        advanceTimeBy(1_100L)
        runCurrent()
        val pollsBeforeRecreation = transport.requests.count { it.path == "state" }
        assertTrue("first lifecycle scope should poll", pollsBeforeRecreation >= 2)

        // Recreation: the old lifecycleScope is cancelled, then the new
        // onCreate calls start() again with a fresh lifecycleScope. A
        // one-shot guard would swallow this second start and freeze the
        // dashboard forever; start() must cancel-and-restart.
        firstScope.cancel()
        runCurrent()

        val secondScope = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())
        repository.start(secondScope)
        runCurrent()
        advanceTimeBy(1_100L)
        runCurrent()

        val totalPolls = transport.requests.count { it.path == "state" }
        assertTrue(
            "polling must resume after recreation (before=$pollsBeforeRecreation total=$totalPolls)",
            totalPolls >= pollsBeforeRecreation + 2,
        )

        secondScope.cancel()
    }

    @Test
    fun pollingPublishesStatusAndReflectsFailures() = runTest {
        var failing = false
        val transport = FakeBleTransport { _, _, _ ->
            if (failing) {
                Resp(503, """{"error":"state unavailable"}""")
            } else {
                Resp(200, ApiFixtures.STATE)
            }
        }
        val repository = GaugeRepository(
            GaugeApi { transport },
            MutableStateFlow<GaugeTransport?>(transport),
        )
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())

        repository.start(scope)
        runCurrent()

        assertEquals("BOOST", repository.status.value?.zone)
        assertTrue(repository.connected.value)

        failing = true
        advanceTimeBy(1_100L)
        runCurrent()

        assertFalse(repository.connected.value)
        assertTrue(repository.lastError.value != null)

        scope.cancel()
    }

    @Test
    fun explicitTransportLossResetsStatusAndConnectionState() = runTest {
        val transportFlow = MutableStateFlow<GaugeTransport?>(null)
        val transport = FakeBleTransport { _, path, _ ->
            when (path) {
                "state" -> Resp(200, ApiFixtures.STATE)
                else -> Resp(404, "{}")
            }
        }
        val repository = GaugeRepository(GaugeApi { transport }, transportFlow)
        transportFlow.value = transport

        assertTrue(repository.refresh())
        assertEquals("BOOST", repository.status.value?.zone)
        assertTrue(repository.connected.value)
        assertEquals(ConnectionStatus.Connected, repository.connectionStatus.value)

        // Explicit Disconnect: transport torn down, repository resets the link
        // flags AND the last-known payload so screens show placeholders.
        transportFlow.value = null
        repository.onTransportDisconnected()

        assertFalse(repository.connected.value)
        assertNull(repository.reconnectAttempt.value)
        assertNull(repository.status.value)
        assertNull(repository.lastError.value)
        assertEquals(ConnectionStatus.Disconnected, repository.connectionStatus.value)
    }

    @Test
    fun statusLoopClearsStalePayloadWhenTransportGoesNull() = runTest {
        val transportFlow = MutableStateFlow<GaugeTransport?>(null)
        val transport = FakeBleTransport { _, _, _ -> Resp(200, ApiFixtures.STATE) }
        val repository = GaugeRepository(GaugeApi { transport }, transportFlow)
        val scope = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())
        repository.start(scope)
        runCurrent()
        assertFalse(repository.connected.value)

        transportFlow.value = transport
        advanceTimeBy(1_100L)
        runCurrent()
        assertEquals("BOOST", repository.status.value?.zone)
        assertTrue(repository.connected.value)

        // The transport disappears (disconnect or radio loss): the loop's
        // no-peer branch must also drop the stale payload, never hold it.
        transportFlow.value = null
        advanceTimeBy(1_100L)
        runCurrent()

        assertFalse(repository.connected.value)
        assertNull(repository.status.value)
        assertEquals(ConnectionStatus.Disconnected, repository.connectionStatus.value)

        scope.cancel()
    }
}
