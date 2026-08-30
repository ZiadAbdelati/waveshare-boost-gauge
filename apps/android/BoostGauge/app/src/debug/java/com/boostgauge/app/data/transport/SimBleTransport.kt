package com.boostgauge.app.data.transport

import com.boostgauge.app.data.api.ApiJson
import com.boostgauge.app.data.api.Calibration
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.DimSchedule
import com.boostgauge.app.data.api.LogSample
import com.boostgauge.app.data.api.LogsPayload
import com.boostgauge.app.data.api.ThemesPayload
import com.boostgauge.app.data.api.TpmsConfig
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.doubleOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.longOrNull
import kotlinx.serialization.json.put

/**
 * In-process stand-in for the firmware's BLE transport so the BLE-mode UI can
 * be iterated in the Android emulator, which cannot do reliable real BLE.
 * Serves the same /api/v1 JSON shapes captured from the board and pushes a
 * live demo waveform on the Status-characteristic contract (~8 Hz) via
 * [statusLine].
 *
 * Launch with:
 *   adb shell am start -n com.boostgauge.app/.MainActivity --es transport simBle
 */
class SimBleTransport : BleGaugeTransport {

    override val transportKind: String = "BLE"

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val _statusLine = MutableStateFlow<String?>(null)
    override val statusLine: StateFlow<String?> = _statusLine.asStateFlow()

    private val _linkUp = MutableStateFlow(true)
    override val linkUp: StateFlow<Boolean> = _linkUp.asStateFlow()

    private var themes: ThemesPayload = ApiJson.json.decodeFromString(FIXTURE_THEMES)
    private var config: Config = ApiJson.json.decodeFromString(FIXTURE_CONFIG)
    private var tpms: TpmsConfig = ApiJson.json.decodeFromString(FIXTURE_TPMS)
    private val calibration: Calibration = ApiJson.json.decodeFromString(FIXTURE_CALIBRATION)
    private val baseState: JsonObject = ApiJson.json.parseToJsonElement(FIXTURE_STATE).jsonObject

    private var activeThemeId: String = themes.activeThemeId

    // Log ring mirroring the firmware (18,000 samples, 200 ms spacing -> 1 hour).
    private val ring = ArrayDeque<LogSample>()
    private var uptimeMs = baseState["uptimeMs"]?.jsonPrimitive?.longOrNull ?: 0L
    private var peakPsi = baseState["peakPsi"]?.jsonPrimitive?.doubleOrNull ?: PSI_MAX
    private var lastLogUptime = uptimeMs

    init {
        var t = uptimeMs - (LOG_RING_CAPACITY - 1) * LOG_INTERVAL_MS
        repeat(LOG_RING_CAPACITY) {
            ring.addLast(sampleAt(t))
            t += LOG_INTERVAL_MS
        }
        _statusLine.value = stateJson()
        scope.launch {
            while (isActive) {
                delay(TICK_MS)
                tick()
            }
        }
    }

    private fun tick() {
        uptimeMs += TICK_MS
        if (uptimeMs - lastLogUptime >= LOG_INTERVAL_MS) {
            ring.addLast(sampleAt(uptimeMs))
            while (ring.size > LOG_RING_CAPACITY) ring.removeFirst()
            lastLogUptime = uptimeMs
        }
        _statusLine.value = stateJson()
    }

    private fun psiAt(tMs: Long): Double {
        val t = tMs / 1000.0
        val period = TRIANGLE_PERIOD_MS / 1000.0
        val phase = (t % period + period) % period
        val half = period / 2.0
        val frac = if (phase < half) phase / half else 2.0 - phase / half
        return PSI_MIN + (PSI_MAX - PSI_MIN) * frac
    }

    private fun zoneFor(psi: Double): String = when {
        psi >= config.psiOverboost -> "OVER"
        psi > ZONE_BAND -> "BOOST"
        psi < -ZONE_BAND -> "VAC"
        else -> "ATMO"
    }

    private fun sampleAt(tMs: Long): LogSample {
        val psi = psiAt(tMs)
        return LogSample(
            tMs = tMs,
            psi = psi,
            peakPsi = PSI_MAX,
            zone = zoneFor(psi),
            demo = true,
        )
    }

    private fun stateJson(): String {
        // Peak latches upward only — matching the real firmware (a tap on
        // the gauge face is the only reset). No decay: peak is not an average.
        peakPsi = maxOf(psiAt(uptimeMs), peakPsi)
        return buildJsonObject {
            put("psi", psiAt(uptimeMs))
            put("peakPsi", peakPsi)
            put("zone", zoneFor(psiAt(uptimeMs)))
            put("demo", true)
            put("uptimeMs", uptimeMs)
            put("epochMs", System.currentTimeMillis())
            put("activeThemeId", activeThemeId)
            baseState.forEach { (key, value) ->
                if (key !in LIVE_STATE_KEYS) put(key, value)
            }
        }.toString()
    }

    private fun encodeThemes(): String = ApiJson.json.encodeToString(themes)

    override suspend fun get(path: String): Resp = handle("GET", path, null)

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp =
        handle(method, path, bodyJson)

    private fun handle(method: String, path: String, bodyJson: String?): Resp {
        val clean = path.trimStart('/')
        val route = clean.substringBefore('?')
        val query = clean.substringAfter('?', "")
        val body = bodyJson?.let {
            runCatching { ApiJson.json.parseToJsonElement(it).jsonObject }.getOrNull()
        }
        return when (route) {
            "state" -> ok(stateJson())
            "themes" -> ok(encodeThemes())
            "themes/active" -> {
                if (method == "PUT") {
                    body?.get("id")?.jsonPrimitive?.content
                        ?.takeIf { it.isNotBlank() }
                        ?.let { id ->
                            activeThemeId = id
                            themes = themes.copy(activeThemeId = id)
                            config = config.copy(activeThemeId = id)
                        }
                    _statusLine.value = stateJson()
                }
                ok(encodeThemes())
            }
            "themes/config" -> {
                if (method == "PUT") {
                    themes = applyThemePatch(body)
                    _statusLine.value = stateJson()
                }
                ok(encodeThemes())
            }
            "config" -> {
                if (method == "PUT") {
                    config = applyConfigPatch(body)
                    _statusLine.value = stateJson()
                }
                ok(ApiJson.json.encodeToString(config))
            }
            "tpms/config" -> {
                if (method == "PUT") tpms = applyTpmsPatch(body)
                ok(ApiJson.json.encodeToString(tpms))
            }
            "sensors/calibration" -> ok(ApiJson.json.encodeToString(calibration))
            "logs" -> when (method) {
                "DELETE" -> {
                    ring.clear()
                    ok("{}")
                }
                else -> {
                    val limit = query.removePrefix("limit=").toIntOrNull() ?: DEFAULT_LOG_LIMIT
                    ok(ApiJson.json.encodeToString(LogsPayload(ring.toList().takeLast(limit))))
                }
            }
            "time" -> {
                if (method == "POST") {
                    body?.let { config = applyConfigPatch(it) }
                    _statusLine.value = stateJson()
                }
                ok(stateJson())
            }
            else -> Resp(404, """{"error":"not found"}""")
        }
    }

    private fun applyThemePatch(body: JsonObject?): ThemesPayload {
        var t = themes
        body ?: return t
        for ((key, value) in body) {
            val prim = value.jsonPrimitive
            when (key) {
                "bigDigitStaticBg" -> prim.booleanOrNull?.let { t = t.copy(bigDigitStaticBg = it) }
                "bigDigitColorText" -> prim.booleanOrNull?.let { t = t.copy(bigDigitColorText = it) }
                "arcGradient" -> prim.booleanOrNull?.let { t = t.copy(arcGradient = it) }
                "hudGradient" -> prim.booleanOrNull?.let { t = t.copy(hudGradient = it) }
                "hudTrueBlack" -> prim.booleanOrNull?.let { t = t.copy(hudTrueBlack = it) }
                "neonMarqueeSpin" -> prim.booleanOrNull?.let { t = t.copy(neonMarqueeSpin = it) }
                "teSync" -> prim.booleanOrNull?.let { t = t.copy(teSync = it) }
                "regionDBuf" -> prim.booleanOrNull?.let { t = t.copy(regionDBuf = it) }
                "teScanline" -> prim.booleanOrNull?.let { t = t.copy(teScanline = it) }
                "vaultNeedleRed" -> prim.booleanOrNull?.let { t = t.copy(vaultNeedleRed = it) }
                "vaultNeedleTail" -> prim.booleanOrNull?.let { t = t.copy(vaultNeedleTail = it) }
                "demoMode" -> prim.booleanOrNull?.let { t = t.copy(demoMode = it) }
                "demoFastSweep" -> prim.booleanOrNull?.let { t = t.copy(demoFastSweep = it) }
                "tpmsBle" -> prim.booleanOrNull?.let { t = t.copy(tpmsBle = it) }
                "pixelShift" -> prim.booleanOrNull?.let { t = t.copy(pixelShift = it) }
                "vaultVignette" -> prim.intOrNull?.let { t = t.copy(vaultVignette = it) }
                "neonLayout" -> prim.intOrNull?.let { t = t.copy(neonLayout = it) }
                "neonFont" -> prim.intOrNull?.let { t = t.copy(neonFont = it) }
                "neonPreset" -> prim.intOrNull?.let { t = t.copy(neonPreset = it) }
                "rotation" -> prim.intOrNull?.let { t = t.copy(rotation = it) }
                "pixelShiftSec" -> prim.intOrNull?.let { t = t.copy(pixelShiftSec = it) }
                "bigDigitStaticColor" -> prim.content.takeIf { it.isNotBlank() }?.let { t = t.copy(bigDigitStaticColor = it) }
                "bigDigitTextColor" -> prim.content.takeIf { it.isNotBlank() }?.let { t = t.copy(bigDigitTextColor = it) }
                "vaultFace" -> prim.content.takeIf { it.isNotBlank() }?.let { t = t.copy(vaultFace = it) }
                else -> Unit
            }
        }
        return t
    }

    private fun applyConfigPatch(body: JsonObject?): Config {
        var c = config
        body ?: return c
        for ((key, value) in body) {
            val prim = value.jsonPrimitive
            when (key) {
                "brightnessHigh" -> prim.intOrNull?.let { c = c.copy(brightnessHigh = it) }
                "brightnessLow" -> prim.intOrNull?.let { c = c.copy(brightnessLow = it) }
                "dimSchedule" -> {
                    val d = value.jsonObject
                    c = c.copy(
                        dimSchedule = DimSchedule(
                            enabled = d["enabled"]?.jsonPrimitive?.booleanOrNull ?: c.dimSchedule.enabled,
                            startMinutes = d["startMinutes"]?.jsonPrimitive?.intOrNull ?: c.dimSchedule.startMinutes,
                            endMinutes = d["endMinutes"]?.jsonPrimitive?.intOrNull ?: c.dimSchedule.endMinutes,
                        ),
                    )
                }
                "timezoneOffsetMinutes" -> prim.intOrNull?.let { c = c.copy(timezoneOffsetMinutes = it) }
                "timezoneTz" -> prim.content.takeIf { it.isNotBlank() }?.let { c = c.copy(timezoneTz = it) }
                "activeThemeId" -> prim.content.takeIf { it.isNotBlank() }?.let { id ->
                    activeThemeId = id
                    themes = themes.copy(activeThemeId = id)
                    c = c.copy(activeThemeId = id)
                }
                "psiMin" -> prim.doubleOrNull?.let { c = c.copy(psiMin = it) }
                "psiMax" -> prim.doubleOrNull?.let { c = c.copy(psiMax = it) }
                "psiOverboost" -> prim.doubleOrNull?.let { c = c.copy(psiOverboost = it) }
                "zeroAngle" -> prim.doubleOrNull?.let { c = c.copy(zeroAngle = it) }
                "appBle" -> prim.booleanOrNull?.let { c = c.copy(appBle = it) }
                else -> Unit
            }
        }
        return c
    }

    private fun applyTpmsPatch(body: JsonObject?): TpmsConfig {
        var t = tpms
        body ?: return t
        body["lowPsi"]?.jsonPrimitive?.doubleOrNull?.let { t = t.copy(lowPsi = it) }
        body["staleAfterMs"]?.jsonPrimitive?.longOrNull?.let { t = t.copy(staleAfterMs = it) }
        return t
    }

    override suspend fun readLog(): String {
        val rows = ring.toList().takeLast(LOG_READ_LIMIT)
        return buildString {
            append("BGL1\n")
            rows.forEach { row ->
                append(row.tMs)
                    .append(',')
                    .append(row.psi)
                    .append(',')
                    .append(row.peakPsi)
                    .append(',')
                    .append(row.zone)
                    .append(',')
                    .append(if (row.demo) 1 else 0)
                    .append('\n')
            }
        }
    }

    override suspend fun readStatus(): String = _statusLine.value ?: stateJson()

    override suspend fun close() {
        _linkUp.value = false
        scope.cancel()
    }

    private fun ok(body: String) = Resp(200, body)

    companion object {
        const val LOG_RING_CAPACITY = 18_000
        const val LOG_READ_LIMIT = 3_000
        const val DEFAULT_LOG_LIMIT = 300

        private const val TICK_MS = 125L
        private const val LOG_INTERVAL_MS = 200L
        private const val TRIANGLE_PERIOD_MS = 24_000L
        private const val PSI_MIN = -3.0
        private const val PSI_MAX = 10.0
        private const val ZONE_BAND = 0.25

        private val LIVE_STATE_KEYS = setOf(
            "psi", "peakPsi", "zone", "demo", "uptimeMs", "epochMs", "activeThemeId",
        )

        // Byte-for-byte board captures (real fixtures).
        private const val FIXTURE_STATE = """{"psi":3.29,"peakPsi":10.00,"zone":"BOOST","demo":true,"brightness":100,"firmwareVersion":"v0.8.1-7-g1669ccc-dirty","uptimeMs":117589,"epochMs":1787532979588,"timezoneOffsetMinutes":-240,"activeThemeId":"neon","activePage":0,"display":{"renderFps":54,"gaugeDemandPerSecond":55,"flushesPerSecond":392,"pixelsPerSecond":1110788,"worstRenderUs":34037,"renderGapP50Us":16000,"renderGapMaxUs":35355,"framesOverBudget":7,"tePeriodUs":16728,"teWaits":54,"teTimeouts":0,"teSkips":44,"teScanlineWaits":0},"sensors":{"adsPresent":false,"bmpPresent":false,"fault":false,"mapVolts":0.0000,"mapAbsKpa":0.00,"ambientKpa":0.00},"tpms":{"status":2,"lowPsi":29.0,"wheels":[{"psi":32.5,"valid":true},{"psi":28.4,"valid":true},{"psi":0.0,"valid":false},{"psi":33.1,"valid":true}]},"obd":{"state":1,"lastError":13,"peer":"","peerAddr":"dd:0d:30:79:3d:a7","uptimeMs":0,"ageMs":0,"valid":false,"lastReply":"","protocol":"","rpm":0.0,"speedKph":0.0,"coolantC":0.0,"mapKpa":0.0,"iatC":0.0,"throttlePct":0.0,"mafGps":0.0,"fuelPct":0.0,"batteryV":0.0}}"""

        private const val FIXTURE_THEMES = """{"activeThemeId":"neon","bigDigitStaticBg":true,"bigDigitColorText":false,"bigDigitStaticColor":"#000000","bigDigitTextColor":"#ffffff","arcGradient":false,"hudGradient":false,"hudTrueBlack":true,"neonMarqueeSpin":false,"teSync":true,"regionDBuf":true,"teScanline":false,"rotation":0,"vaultFace":"#041c11","vaultVignette":45,"vaultNeedleRed":true,"vaultNeedleTail":false,"neonLayout":1,"neonFont":0,"neonPreset":3,"demoMode":true,"demoFastSweep":true,"tpmsBle":true,"pixelShift":false,"pixelShiftSec":90,"themes":[{"id":"dyno-cell","name":"Dyno Cell","style":"arc","colors":{"face":"#090a0d","track":"#20242c","text":"#f5f7fa","muted":"#8c95a3","vacuum":"#4dd2ff","boost":"#b8f35a","overboost":"#ff4f6d","zero":"#ffffff"},"customized":false},{"id":"vault-tec","name":"Vault-Tec","style":"vault","colors":{"face":"#05281a","track":"#0c3d24","text":"#38f08a","muted":"#1f7a4d","vacuum":"#38f08a","boost":"#38f08a","overboost":"#eafc50","zero":"#38f08a"},"customized":false},{"id":"night-city","name":"Night City","style":"hud","colors":{"face":"#080a08","track":"#1a1c0a","text":"#fcee0a","muted":"#5a7a0a","vacuum":"#00e5ff","boost":"#fcee0a","overboost":"#ff003c","zero":"#00e5ff"},"customized":false},{"id":"big-digit","name":"Big Digit","style":"bigdigit","colors":{"face":"#0b0c0e","track":"#20242c","text":"#ffffff","muted":"#0b0c0e","vacuum":"#4dd2ff","boost":"#b8f35a","overboost":"#ff4f6d","zero":"#ffffff"},"customized":false},{"id":"neon","name":"Neon","style":"neon","colors":{"face":"#000000","track":"#0c1440","text":"#ffffff","muted":"#35509e","vacuum":"#0064ff","boost":"#c4172e","overboost":"#ff6a00","zero":"#ffffff"},"customized":false}]}"""

        private const val FIXTURE_CONFIG = """{"brightnessHigh":100,"brightnessLow":25,"dimSchedule":{"enabled":true,"startMinutes":1230,"endMinutes":420},"timezoneOffsetMinutes":-300,"timezoneTz":"EST5EDT,M3.2.0/2,M11.1.0/2","activeThemeId":"neon","psiMin":-15.00,"psiMax":10.00,"psiOverboost":8.00,"zeroAngle":220.00,"appBle":true}"""

        private const val FIXTURE_TPMS = """{"lowKpa":200.0,"lowPsi":29.0,"staleAfterMs":15000}"""

        // Calibration shape from boost_web.c sensors_calibration_json().
        private const val FIXTURE_CALIBRATION = """{"supplyVolts":4.9860,"live":{"adsPresent":true,"bmpPresent":true,"fault":false,"mapVolts":1.1821,"mapAgeMs":42,"nominalKpa":101.32,"correctedKpa":101.32,"bmpKpa":101.30,"bmpAgeMs":41,"bmpUpdates":12345,"ambientIsFallback":false},"calibration":{"valid":true,"version":3,"offsetKpa":0.02,"offsetPsi":0.003,"supplyVolts":4.9860,"refMapVolts":1.1818,"refNominalKpa":101.28,"refBmpKpa":101.30,"samples":60,"epochMs":1780000000000}}"""
    }
}