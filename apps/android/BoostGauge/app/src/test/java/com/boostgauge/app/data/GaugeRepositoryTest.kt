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
}
