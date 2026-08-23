package com.boostgauge.app.data.api

import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.Resp
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

class ApiException(val status: Int, message: String) : Exception(message)

/** Typed access to the gauge's /api/v1 surface. */
class GaugeApi(private val transportProvider: () -> GaugeTransport) {

    private val json = ApiJson.json

    suspend fun getState(): Status = parse(get("state"))
    suspend fun getConfig(): Config = parse(get("config"))
    suspend fun getThemes(): ThemesPayload = parse(get("themes"))
    suspend fun getCalibration(): Calibration = parse(get("sensors/calibration"))
    suspend fun getTpmsConfig(): TpmsConfig = parse(get("tpms/config"))
    suspend fun getLogs(limit: Int): LogsPayload = parse(get("logs?limit=$limit"))

    suspend fun activateTheme(id: String): ThemesPayload =
        parse(send("PUT", "themes/active", buildJsonObject { put("id", id) }.toString()))

    suspend fun updateConfig(patch: JsonObject): Config =
        parse(send("PUT", "config", patch.toString()))

    suspend fun updateThemesConfig(patch: JsonObject): ThemesPayload =
        parse(send("PUT", "themes/config", patch.toString()))

    suspend fun updateTpmsConfig(lowPsi: Double, staleAfterMs: Long): TpmsConfig =
        parse(send("PUT", "tpms/config", buildJsonObject {
            put("lowPsi", lowPsi)
            put("staleAfterMs", staleAfterMs)
        }.toString()))

    /** POST /sensors/calibration; body is ignored by the handler but must be valid JSON. */
    suspend fun calibrateAtmosphere(): Calibration =
        parse(send("POST", "sensors/calibration", "{}"))

    /** POST /time with the device epoch; the gauge rejects a clock >5 min from its RTC. */
    suspend fun syncTime(epochMs: Long, timezoneOffsetMinutes: Int): Status =
        parse(send("POST", "time", buildJsonObject {
            put("epochMs", epochMs)
            put("timezoneOffsetMinutes", timezoneOffsetMinutes)
        }.toString()))

    suspend fun clearLogs(): Resp = send("DELETE", "logs", null)

    private suspend fun get(path: String): Resp = check(transportProvider().get(path))

    private suspend fun send(method: String, path: String, bodyJson: String?): Resp =
        check(transportProvider().send(method, path, bodyJson))

    private fun check(resp: Resp): Resp {
        if (resp.status !in 200..299) {
            val error = runCatching { json.decodeFromString<ErrorBody>(resp.body).error }
                .getOrNull()
                ?.takeIf { it.isNotBlank() }
                ?: "HTTP ${resp.status}"
            throw ApiException(resp.status, error)
        }
        return resp
    }

    private inline fun <reified T> parse(resp: Resp): T = json.decodeFromString(resp.body)
}

