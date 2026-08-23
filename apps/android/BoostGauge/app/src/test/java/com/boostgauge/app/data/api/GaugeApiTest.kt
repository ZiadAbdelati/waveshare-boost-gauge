package com.boostgauge.app.data.api

import com.boostgauge.app.data.transport.FakeBleTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Config round-trip plus payload parsing against real firmware output shapes. */
class GaugeApiTest {

    @Test
    fun configRoundTrip() = runBlocking {
        val transport = FakeBleTransport { method, path, _ ->
            assertEquals("PUT", method)
            assertEquals("config", path)
            Resp(200, ApiFixtures.CONFIG)
        }
        val api = GaugeApi { transport }

        val patch = buildJsonObject {
            put("brightnessHigh", 92)
            put("dimSchedule", buildJsonObject {
                put("enabled", true)
                put("startMinutes", 1380)
                put("endMinutes", 360)
            })
            put("psiMin", -15.0)
            put("psiMax", 10.0)
        }
        val config = api.updateConfig(patch)

        assertEquals(92, config.brightnessHigh)
        assertEquals(10, config.brightnessLow)
        assertTrue(config.dimSchedule.enabled)
        assertEquals(-15.0, config.psiMin, 0.001)
        assertEquals("EST5EDT,M3.2.0/2,M11.1.0/2", config.timezoneTz)
        assertFalse(config.appBle)
        val request = transport.requests.single()
        assertTrue(request.bodyJson!!.contains("\"brightnessHigh\":92"))
        assertTrue(request.bodyJson.contains("\"dimSchedule\""))
        assertFalse(request.bodyJson.contains("zeroAngle"))
    }

    @Test
    fun configRoundTripIncludesAppBle() = runBlocking {
        val transport = FakeBleTransport { method, path, _ ->
            assertEquals("PUT", method)
            assertEquals("config", path)
            Resp(200, ApiFixtures.CONFIG.replace("\"appBle\": false", "\"appBle\": true"))
        }
        val api = GaugeApi { transport }

        val config = api.updateConfig(buildJsonObject { put("appBle", true) })

        assertTrue(config.appBle)
        assertTrue(transport.requests.single().bodyJson!!.contains("\"appBle\":true"))
    }

    @Test
    fun themesPayloadParsesAllFiveThemesInOrder() = runBlocking {
        val transport = FakeBleTransport { _, _, _ -> Resp(200, ApiFixtures.THEMES) }
        val api = GaugeApi { transport }

        val payload = api.getThemes()

        assertEquals(listOf("Dyno Cell", "Vault-Tec", "Night City", "Big Digit", "Neon"),
            payload.themes.map { it.name })
        assertEquals("dyno-cell", payload.activeThemeId)
        assertTrue(payload.themes[2].customized)
        assertTrue(payload.regionDBuf)
    }

    @Test
    fun activateThemeSendsIdBodyAndReturnsUpdatedThemes() = runBlocking {
        val transport = FakeBleTransport { method, path, body ->
            assertEquals("PUT", method)
            assertEquals("themes/active", path)
            assertTrue(body!!.contains("\"id\":\"neon\""))
            Resp(200, ApiFixtures.THEMES)
        }
        val api = GaugeApi { transport }

        val payload = api.activateTheme("neon")

        // The fake echoes the unmodified themes fixture back; the request is the
        // interesting assertion here.
        assertEquals("dyno-cell", payload.activeThemeId)
    }

    @Test
    fun updateThemesConfigSendsDemoPatch() = runBlocking {
        val transport = FakeBleTransport { method, path, body ->
            assertEquals("PUT", method)
            assertEquals("themes/config", path)
            assertTrue(body!!.contains("\"demoMode\":true"))
            Resp(200, ApiFixtures.THEMES)
        }
        val api = GaugeApi { transport }

        api.updateThemesConfig(
            buildJsonObject { put("demoMode", true) },
        )

        assertEquals("themes/config", transport.requests.single().path)
    }

    @Test
    fun logsAndCalibrationParse() = runBlocking {
        val transport = FakeBleTransport { _, path, _ ->
            when (path) {
                "logs?limit=50" -> Resp(200, ApiFixtures.LOGS)
                "sensors/calibration" -> Resp(200, ApiFixtures.CALIBRATION)
                else -> Resp(404, "{}")
            }
        }
        val api = GaugeApi { transport }

        val logs = api.getLogs(50)
        assertEquals(2, logs.samples.size)
        assertEquals("ATMO", logs.samples[0].zone)
        assertEquals(1000L, logs.samples[0].tMs)

        val calibration = api.getCalibration()
        assertTrue(calibration.calibration.valid)
        assertEquals(3, calibration.calibration.version)
        assertEquals(1.1821, calibration.live.mapVolts, 0.0001)
    }
}
