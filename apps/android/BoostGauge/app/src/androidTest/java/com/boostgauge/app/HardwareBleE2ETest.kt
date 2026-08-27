package com.boostgauge.app

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.test.core.app.ApplicationProvider
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.uiautomator.By
import androidx.test.uiautomator.UiDevice
import androidx.test.uiautomator.Until
import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.transport.BleScanner
import com.boostgauge.app.data.transport.BleTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.util.regex.Pattern

@RunWith(AndroidJUnit4::class)
class HardwareBleE2ETest {
    private val context: Context = ApplicationProvider.getApplicationContext()

    @Test
    fun physicalGaugeCompletesFullRequestMatrix() = runBlocking {
        requirePermission(Manifest.permission.BLUETOOTH_SCAN)
        requirePermission(Manifest.permission.BLUETOOTH_CONNECT)

        withTimeout(180_000) {
            val scanResults = BleScanner(context).scan(timeoutMs = 20_000)
            assertTrue("FAIL: no BoostGauge advertisement found", scanResults.isNotEmpty())
            val gauge = scanResults.first()
            println("BLE_E2E found ${gauge.name} ${gauge.address}")

            val transport = BleTransport(context, gauge.address)
            try {
                val pairWatcher = launch(Dispatchers.Default) { acceptSystemPairingPrompt() }
                try {
                    transport.connect()
                } finally {
                    pairWatcher.cancelAndJoin()
                }
                println("BLE_E2E connected")

                val state = expectJson(transport.get("state"), "GET /state")
                val originalTheme = state.string("activeThemeId")

                expectJson(transport.get("config"), "GET /config")
                expectJson(transport.get("themes"), "GET /themes")

                expectJson(transport.send("PUT", "page", "{\"page\":0}"), "PUT /page 0")
                expectJson(transport.send("PUT", "page", "{\"page\":1}"), "PUT /page 1")
                val pageRestored = expectJson(
                    transport.send("PUT", "page", "{\"page\":0}"),
                    "PUT /page restore 0",
                )
                assertEquals("PUT /page restore did not select page 0", 0, pageRestored.int("activePage"))

                val testTheme = if (originalTheme == "neon") "dyno-cell" else "neon"
                expectJson(
                    transport.send("PUT", "themes/active", "{\"id\":\"$testTheme\"}"),
                    "PUT /themes/active $testTheme",
                )
                val themeRestored = expectJson(
                    transport.send("PUT", "themes/active", "{\"id\":\"$originalTheme\"}"),
                    "PUT /themes/active restore $originalTheme",
                )
                assertEquals(
                    "PUT /themes/active did not restore the original theme",
                    originalTheme,
                    themeRestored.string("activeThemeId"),
                )

                val deviceInfo = parseObject(transport.readDeviceInfo(), "READ DeviceInfo")
                assertEquals("BoostGauge", deviceInfo.string("name"))
                assertEquals(1, deviceInfo.int("api"))
                println("BLE_E2E PASS READ DeviceInfo")

                val forcedStatus = parseObject(transport.readStatus(), "READ Status")
                assertNotNull(
                    "READ Status did not return the full /state mirror (missing display)",
                    forcedStatus["display"],
                )
                assertNotNull("READ Status missing uptimeMs", forcedStatus["uptimeMs"])
                println("BLE_E2E PASS READ Status")

                val log = transport.readLog()
                assertTrue("READ Log did not return BGL1 framing", log.startsWith("BGL1\n"))
                println("BLE_E2E PASS READ Log BGL1")
                println("BLE_E2E PASS full matrix")
            } finally {
                transport.close()
            }
        }
    }

    /**
     * Android shows an OS-owned confirmation dialog for the gauge's encrypted
     * characteristics. The test runner cannot bond until that positive button
     * is accepted, so watch the system UI while connect() waits for bonding.
     * Existing bonds produce no dialog and this coroutine is cancelled as soon
     * as connect completes.
     */
    private suspend fun acceptSystemPairingPrompt() {
        val device = UiDevice.getInstance(InstrumentationRegistry.getInstrumentation())
        val positiveText = Pattern.compile("^(PAIR|Pair|OK|Ok)$")
        repeat(30) {
            var button = device.wait(Until.findObject(By.text(positiveText)), 500)
                ?: device.findObject(By.res("android:id/button1"))
            if (button != null && button.isEnabled) {
                println("BLE_E2E accepting system pairing prompt: ${button.text}")
                button.click()
                device.waitForIdle()
                return
            }
            // This Android 12 build initially posts the pairing request as a
            // notification instead of launching its confirmation activity.
            // Open the shade, select the BoostGauge request, then accept the
            // positive button on the resulting system dialog.
            device.openNotification()
            device.waitForIdle()
            device.findObject(By.textContains("BoostGauge"))?.click()
            button = device.wait(Until.findObject(By.text(positiveText)), 500)
                ?: device.findObject(By.res("android:id/button1"))
            if (button != null && button.isEnabled) {
                println("BLE_E2E accepting pairing notification/dialog: ${button.text}")
                button.click()
                device.waitForIdle()
                return
            }
            delay(100)
        }
    }

    private fun requirePermission(permission: String) {
        assertEquals(
            "FAIL: $permission not granted; run through tools/check_ble_android_e2e.sh",
            PackageManager.PERMISSION_GRANTED,
            context.checkSelfPermission(permission),
        )
    }

    private fun expectJson(response: Resp, label: String): JsonObject {
        assertTrue("$label returned ${response.status}: ${response.body}", response.status in 200..299)
        val body = parseObject(response.body, label)
        println("BLE_E2E PASS $label")
        return body
    }

    private fun parseObject(value: String, label: String): JsonObject = runCatching {
        ApiJson.json.parseToJsonElement(value).jsonObject
    }.getOrElse { throw AssertionError("$label returned invalid JSON: $value", it) }

    private fun JsonObject.string(key: String): String {
        val value = this[key]?.jsonPrimitive?.content
        assertNotNull("missing '$key' in $this", value)
        return value!!
    }

    private fun JsonObject.int(key: String): Int {
        val value = runCatching { this[key]?.jsonPrimitive?.int }.getOrNull()
        assertNotNull("missing integer '$key' in $this", value)
        return value!!
    }
}
