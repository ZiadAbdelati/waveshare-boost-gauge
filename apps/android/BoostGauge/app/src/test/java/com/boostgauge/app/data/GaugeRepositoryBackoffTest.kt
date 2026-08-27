package com.boostgauge.app.data

import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.GaugeApi
import com.boostgauge.app.data.transport.FakeBleGaugeTransport
import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class GaugeRepositoryBackoffTest {

    @Test
    fun backoffDelayProgression() {
        assertEquals(1_000L, GaugeRepository.backoffDelayMs(1))
        assertEquals(2_000L, GaugeRepository.backoffDelayMs(2))
        assertEquals(5_000L, GaugeRepository.backoffDelayMs(3))
        assertEquals(10_000L, GaugeRepository.backoffDelayMs(4))
        assertEquals(30_000L, GaugeRepository.backoffDelayMs(5))
        assertEquals(60_000L, GaugeRepository.backoffDelayMs(6))
        assertEquals(60_000L, GaugeRepository.backoffDelayMs(7))
        assertEquals(60_000L, GaugeRepository.backoffDelayMs(100))
        // Attempt 0 and negative clamp to 1s
        assertEquals(1_000L, GaugeRepository.backoffDelayMs(0))
        assertEquals(1_000L, GaugeRepository.backoffDelayMs(-1))
    }

    @Test
    fun reconnectAttemptResetsOnSuccessfulConnect() = runTest {
        // Fake that fails twice then succeeds
        var connectCalls = 0
        val transport = FakeBleGaugeTransport(
            connectBehavior = {
                connectCalls++
                if (connectCalls <= 2) throw RuntimeException("BLE not ready")
            },
            handler = { _, path, _ ->
                if (path == "state") Resp(200, ApiFixtures.STATE) else Resp(404, "{}")
            },
        )
        val api = GaugeApi { transport }
        val transportFlow = MutableStateFlow<GaugeTransport?>(transport)
        val repo = GaugeRepository(api, transportFlow)
        val dispatcher = StandardTestDispatcher(testScheduler)
        val scope = CoroutineScope(dispatcher + Job())
        repo.start(scope)
        runCurrent()

        // Initially disconnected, should enter reconnect loop with attempt 1 after first delay
        advanceTimeBy(1_100L)
        runCurrent()
        // After first failed connect (attempt 1), reconnectAttempt should be 2 (next delay 2s)
        // The loop: first delay 1s, attempt1 fails -> set attempt 2
        assertEquals(2, repo.reconnectAttempt.value)

        advanceTimeBy(2_100L)
        runCurrent()
        // Second failure -> attempt 3
        assertEquals(3, repo.reconnectAttempt.value)

        advanceTimeBy(5_100L)
        runCurrent()
        // Third attempt succeeds -> reset to null and connected true
        assertNull(repo.reconnectAttempt.value)
        assertTrue(repo.connected.value)

        scope.coroutineContext[Job]?.cancel()
    }

    @Test
    fun retryIsIndefiniteNotCappedAtFive() = runTest {
        var connectCalls = 0
        val transport = FakeBleGaugeTransport(
            connectBehavior = {
                connectCalls++
                throw RuntimeException("always fails")
            },
            handler = { _, path, _ -> Resp(200, ApiFixtures.STATE) },
        )
        val api = GaugeApi { transport }
        val repo = GaugeRepository(api, MutableStateFlow<GaugeTransport?>(transport))
        val dispatcher = StandardTestDispatcher(testScheduler)
        val scope = CoroutineScope(dispatcher + Job())
        repo.start(scope)
        runCurrent()

        // Drive through 10 attempts — must not stop at 5
        val expectedDelays = listOf(1_000L, 2_000L, 5_000L, 10_000L, 30_000L, 60_000L)
        var attempt = 1
        repeat(10) { idx ->
            val delay = expectedDelays.getOrElse(idx) { 60_000L }
            advanceTimeBy(delay + 100L)
            runCurrent()
            attempt++
            assertEquals(attempt, repo.reconnectAttempt.value)
            // Must still be retrying, not stuck at 5
            assertTrue("attempt $attempt should be >5 after many failures", repo.reconnectAttempt.value!! > 5 || idx < 5)
        }
        // After 10 failures, still retrying (indefinite) — attempt should be 11
        assertEquals(11, repo.reconnectAttempt.value)

        scope.coroutineContext[Job]?.cancel()
    }

    @Test
    fun disconnectedOnlyForPreFirstConnection() = runTest {
        // No BLE peer (transport null) => pre-first-connection, should be Not connected, not Reconnecting
        val api = GaugeApi { FakeBleGaugeTransport() }
        val repo = GaugeRepository(api, MutableStateFlow<GaugeTransport?>(null))
        val dispatcher = StandardTestDispatcher(testScheduler)
        val scope = CoroutineScope(dispatcher + Job())
        repo.start(scope)
        runCurrent()
        advanceTimeBy(1_100L)
        runCurrent()
        assertEquals(false, repo.connected.value)
        assertNull(repo.reconnectAttempt.value)

        // With BLE peer but failing, should be Reconnecting
        val failingTransport = FakeBleGaugeTransport(
            connectBehavior = { throw RuntimeException("fail") },
            handler = { _, _, _ -> Resp(200, ApiFixtures.STATE) },
        )
        val repo2 = GaugeRepository(GaugeApi { failingTransport }, MutableStateFlow<GaugeTransport?>(failingTransport))
        val scope2 = CoroutineScope(StandardTestDispatcher(testScheduler) + Job())
        repo2.start(scope2)
        runCurrent()
        advanceTimeBy(1_100L)
        runCurrent()
        assertEquals(false, repo2.connected.value)
        assertTrue(repo2.reconnectAttempt.value != null)

        scope.coroutineContext[Job]?.cancel()
        scope2.coroutineContext[Job]?.cancel()
    }
}
