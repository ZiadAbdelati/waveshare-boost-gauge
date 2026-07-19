package dev.boostgauge.api

import kotlinx.coroutines.test.runTest
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class BoostGaugeApiTest {
    @Test
    fun normalizesManualDeviceUrl() {
        assertEquals("http://192.168.4.1", BoostGaugeApi.normalizeBaseUrl("192.168.4.1/"))
        assertEquals("http://boostgauge.local", BoostGaugeApi.normalizeBaseUrl(""))
        assertEquals("https://gauge.local", BoostGaugeApi.normalizeBaseUrl("https://gauge.local/"))
    }

    @Test
    fun parsesStateWithUnknownFields() {
        val state = BoostGaugeApi.apiJson.decodeFromString<GaugeState>(
            """
            {
              "psi": 8.5,
              "peakPsi": 12.25,
              "zone": "boost",
              "demo": false,
              "brightness": 72,
              "firmwareVersion": "0.1.15",
              "uptimeMs": 60000,
              "epochMs": 1720000000000,
              "timezoneOffsetMinutes": -300,
              "activeThemeId": "pit-lane",
              "futureField": "ignored"
            }
            """.trimIndent()
        )

        assertEquals(8.5, state.psi, 0.001)
        assertEquals("pit-lane", state.activeThemeId)
    }

    @Test
    fun usesExactApiV1Paths() = runTest {
        MockWebServer().use { server ->
            server.enqueue(MockResponse().setBody("""{"psi":1.0,"peakPsi":2.0,"zone":"boost"}"""))
            server.enqueue(MockResponse().setBody("""{"brightnessHigh":90,"brightnessLow":20,"dimSchedule":{"enabled":true,"startMinutes":1260,"endMinutes":420},"timezoneOffsetMinutes":-300,"activeThemeId":"pit-lane"}"""))
            server.enqueue(MockResponse().setBody("""{"themes":[],"activeThemeId":"pit-lane"}"""))
            server.enqueue(MockResponse().setBody("""{"samples":[{"uptimeMs":10,"psi":1.5}],"count":1,"limit":120}"""))
            val api = BoostGaugeApi(server.url("/").toString())

            assertEquals(1.0, api.state().psi, 0.001)
            assertTrue(api.config().dimSchedule.enabled)
            assertEquals("pit-lane", api.themes().activeThemeId)
            assertEquals(1, api.logs().samples.size)

            assertEquals("/api/v1/state", server.takeRequest().path)
            assertEquals("/api/v1/config", server.takeRequest().path)
            assertEquals("/api/v1/themes", server.takeRequest().path)
            assertEquals("/api/v1/logs?limit=120", server.takeRequest().path)
        }
    }
}
