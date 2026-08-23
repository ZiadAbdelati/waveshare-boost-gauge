package com.boostgauge.app.data.transport

import com.boostgauge.app.data.api.ApiFixtures
import kotlinx.coroutines.runBlocking
import okhttp3.mockwebserver.MockResponse
import okhttp3.mockwebserver.MockWebServer
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test

class HttpTransportTest {

    private lateinit var server: MockWebServer

    @Before
    fun setUp() {
        server = MockWebServer()
        server.start()
    }

    @After
    fun tearDown() {
        server.shutdown()
    }

    @Test
    fun getHitsApiV1PathWithMethodAndBody() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody(ApiFixtures.STATE))
        val transport = HttpTransport(server.url("/").toString())

        val resp = transport.get("state")

        assertEquals(200, resp.status)
        assertTrue(resp.body.contains("\"psi\""))
        val recorded = server.takeRequest()
        assertEquals("GET", recorded.method)
        assertEquals("/api/v1/state", recorded.path)
        assertEquals("", recorded.body.readUtf8())
    }

    @Test
    fun putSendsJsonBody() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody(ApiFixtures.CONFIG))
        val transport = HttpTransport(server.url("/").toString())
        val patch = """{"brightnessHigh":80,"psiMax":12.0}"""

        val resp = transport.send("PUT", "config", patch)

        assertEquals(200, resp.status)
        val recorded = server.takeRequest()
        assertEquals("PUT", recorded.method)
        assertEquals("/api/v1/config", recorded.path)
        assertEquals(patch, recorded.body.readUtf8())
    }

    @Test
    fun postTimeBodyAndMethod() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody(ApiFixtures.STATE))
        val transport = HttpTransport(server.url("/").toString())
        val body = """{"epochMs":1780000000000,"timezoneOffsetMinutes":-240}"""

        transport.send("POST", "time", body)

        val recorded = server.takeRequest()
        assertEquals("POST", recorded.method)
        assertEquals("/api/v1/time", recorded.path)
        assertTrue(recorded.body.readUtf8().contains("\"epochMs\":1780000000000"))
    }

    @Test
    fun deleteWithoutBody() = runBlocking {
        server.enqueue(MockResponse().setResponseCode(200).setBody("""{"ok":true}"""))
        val transport = HttpTransport(server.url("/").toString())

        transport.send("DELETE", "logs", null)

        val recorded = server.takeRequest()
        assertEquals("DELETE", recorded.method)
        assertEquals("/api/v1/logs", recorded.path)
        assertEquals("", recorded.body.readUtf8())
    }

    @Test
    fun non2xxReturnsStatusAndBodyWithoutThrowing() = runBlocking {
        server.enqueue(
            MockResponse().setResponseCode(409).setBody("""{"error":"clock_rejected"}"""),
        )
        val transport = HttpTransport(server.url("/").toString())

        val resp = transport.send("POST", "time", """{"epochMs":1,"timezoneOffsetMinutes":0}""")

        assertEquals(409, resp.status)
        assertTrue(resp.body.contains("clock_rejected"))
    }

    @Test
    fun queryStringIsPreserved() = runBlocking {
        server.enqueue(
            MockResponse().setResponseCode(200).setBody(ApiFixtures.LOGS),
        )
        val transport = HttpTransport(server.url("/").toString())

        transport.get("logs?limit=50")

        val recorded = server.takeRequest()
        assertEquals("/api/v1/logs?limit=50", recorded.path)
    }
}
