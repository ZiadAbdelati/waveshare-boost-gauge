package com.boostgauge.app.data.transport

import com.boostgauge.app.data.api.ApiFixtures
import com.boostgauge.app.data.api.ApiException
import com.boostgauge.app.data.api.GaugeApi
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Test

/** Verifies the fake BLE transport satisfies the GaugeTransport contract end to end. */
class FakeBleTransportTest {

    @Test
    fun getReturnsStateShape() = runBlocking {
        val transport = FakeBleTransport { method, path, _ ->
            assertEquals("GET", method)
            assertEquals("state", path)
            Resp(200, ApiFixtures.STATE)
        }

        val api = GaugeApi { transport }
        val status = api.getState()

        assertEquals(3.42, status.psi, 0.001)
        assertEquals("BOOST", status.zone)
        assertEquals(4, status.tpms.wheels.size)
        assertTrue(status.obd.valid)
        assertEquals("GET", transport.requests.single().method)
    }

    @Test
    fun apiMapsNon2xxToApiException() = runBlocking {
        val transport = FakeBleTransport { _, _, _ ->
            Resp(409, ApiFixtures.ERROR_CLOCK_REJECTED)
        }
        val api = GaugeApi { transport }

        try {
            api.syncTime(-240, "EST5EDT,M3.2.0/2,M11.1.0/2")
            fail("expected ApiException")
        } catch (e: ApiException) {
            assertEquals(409, e.status)
            assertEquals("clock_rejected", e.message)
        }
    }

    @Test
    fun bodyIsNullForGet() {
        runBlocking {
            val transport = FakeBleTransport { _, _, body ->
                assertNull(body)
                Resp(200, "{}")
            }
            transport.get("state")
        }
    }
}
