package com.boostgauge.app.data.transport

import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.api.LogsPayload
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.ThemeColors
import com.boostgauge.app.data.api.ThemesPayload
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The sim transport must serve the same parseable payloads as the real
 * firmware, so the BLE-mode UI can be iterated against it in the emulator.
 */
class SimBleTransportTest {

    @Test
    fun themesPayloadHasAllEightPaletteColorRoles() = runBlocking {
        val transport = SimBleTransport()
        try {
            val resp = transport.get("themes")
            assertEquals(200, resp.status)
            val payload = ApiJson.json.decodeFromString<ThemesPayload>(resp.body)
            assertEquals(5, payload.themes.size)
            payload.themes.forEach { theme -> assertAllEightColorRoles(theme.colors) }
        } finally {
            transport.close()
        }
    }

    @Test
    fun statePayloadHasFourTpmsWheels() = runBlocking {
        val transport = SimBleTransport()
        try {
            val resp = transport.get("state")
            assertEquals(200, resp.status)
            val status = ApiJson.json.decodeFromString<Status>(resp.body)
            assertEquals(4, status.tpms.wheels.size)
        } finally {
            transport.close()
        }
    }

    @Test
    fun logsRingIsDenseAndReadLogServesBgl1() = runBlocking {
        val transport = SimBleTransport()
        try {
            val resp = transport.get("logs?limit=3000")
            assertEquals(200, resp.status)
            val payload = ApiJson.json.decodeFromString<LogsPayload>(resp.body)
            assertEquals(3000, payload.samples.size)

            val bgl1 = transport.readLog()
            assertTrue(bgl1.startsWith("BGL1\n"))
            assertTrue(BleLogParser.parse(bgl1).size >= 8)
        } finally {
            transport.close()
        }
    }

    @Test
    fun putThemesActiveMutatesPayload() = runBlocking {
        val transport = SimBleTransport()
        try {
            val resp = transport.send("PUT", "themes/active", """{"id":"vault-tec"}""")
            assertEquals(200, resp.status)
            val payload = ApiJson.json.decodeFromString<ThemesPayload>(resp.body)
            assertEquals("vault-tec", payload.activeThemeId)
            assertEquals("vault-tec", transport.readStatus().let {
                ApiJson.json.decodeFromString<Status>(it).activeThemeId
            })
        } finally {
            transport.close()
        }
    }

    private fun assertAllEightColorRoles(colors: ThemeColors) {
        listOf(
            colors.face, colors.track, colors.text, colors.muted,
            colors.vacuum, colors.boost, colors.overboost, colors.zero,
        ).forEach { hex ->
            assertTrue("palette role must be a 6-digit hex color: $hex", HEX_COLOR.matches(hex))
        }
    }

    private companion object {
        val HEX_COLOR = Regex("^#[0-9a-fA-F]{6}$")
    }
}