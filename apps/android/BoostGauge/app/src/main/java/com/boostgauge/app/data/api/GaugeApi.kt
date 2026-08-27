package com.boostgauge.app.data.api

import com.boostgauge.app.data.transport.GaugeTransport
import com.boostgauge.app.data.transport.BleGaugeTransport
import com.boostgauge.app.data.transport.Resp
import com.boostgauge.app.data.transport.TransportException
import kotlinx.serialization.json.JsonArray
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
    suspend fun getLogs(limit: Int): LogsPayload {
        val transport = transportProvider()
        if (transport is BleGaugeTransport) {
            // The graph window is the last 5 minutes (limit samples at the 5 Hz
            // log rate). BLE alone cannot carry it: the BGL1 Log characteristic
            // is an 8-sample diagnostic window. Prefer the gauge's LAN HTTP host
            // from device-info, then a transport that serves the window itself
            // (the emulator sim); only then surface an error — an 8-sample
            // "band" must never masquerade as history.
            runCatching {
                val info = json.parseToJsonElement(transport.readDeviceInfo()) as? JsonObject
                    ?: return@runCatching
                val ip = (info["ip"] as? kotlinx.serialization.json.JsonPrimitive)?.content
                if (ip.isNullOrBlank()) return@runCatching
                val resp = com.boostgauge.app.data.transport.HttpTransport(ip)
                    .get("logs?limit=$limit")
                if (resp.status == 200) {
                    parseLogsPayload(resp.body)?.let { return it }
                }
            }
            runCatching {
                val resp = transport.get("logs?limit=$limit")
                if (resp.status == 200) {
                    parseLogsPayload(resp.body)?.let { return it }
                }
            }
            throw TransportException(
                "5-minute log window unavailable over BLE — join the gauge's Wi-Fi to load logs",
            )
        }
        return parse(get("logs?limit=$limit"))
    }

    /**
     * A log payload is a JSON object carrying a `samples` array. The firmware
     * BLE Control `/logs` route only echoes `{"count":N}`, so that shape must
     * not parse into an (empty) LogsPayload and hide a missing window.
     */
    private fun parseLogsPayload(text: String): LogsPayload? {
        val obj = runCatching { json.parseToJsonElement(text) as? JsonObject }.getOrNull()
            ?: return null
        if (obj["samples"] !is JsonArray) return null
        return runCatching { json.decodeFromString(LogsPayload.serializer(), text) }.getOrNull()
    }

    suspend fun activateTheme(id: String): ThemesPayload =
        parse(send("PUT", "themes/active", buildJsonObject { put("id", id) }.toString()))

    suspend fun updateConfig(patch: JsonObject): Config =
        parse(send("PUT", "config", patch.toString()))

    suspend fun updateThemesConfig(patch: JsonObject): ThemesPayload =
        parse(send("PUT", "themes/config", patch.toString()))

    /** Clears the stored OBD peer (NVS obd_peer) and drops any live link. */
    suspend fun forgetObdPeer() {
        send("POST", "obd/forget", "{}")
    }

    suspend fun updateTpmsConfig(lowPsi: Double, staleAfterMs: Long): TpmsConfig =
        parse(send("PUT", "tpms/config", buildJsonObject {
            put("lowPsi", lowPsi)
            put("staleAfterMs", staleAfterMs)
        }.toString()))

    /** POST /sensors/calibration; body is ignored by the handler but must be valid JSON. */
    suspend fun calibrateAtmosphere(): Calibration =
        parse(send("POST", "sensors/calibration", "{}"))

    /**
     * POST /time with ONLY the timezone. The gauge's DS3231 RTC is the sole
     * time authority, so the phone epoch is never sent; the gauge rejects a
     * body that omits either timezone field with 400.
     */
    suspend fun syncTime(timezoneOffsetMinutes: Int, timezoneTz: String): Status =
        parse(send("POST", "time", buildJsonObject {
            put("timezoneOffsetMinutes", timezoneOffsetMinutes)
            put("timezoneTz", timezoneTz)
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
