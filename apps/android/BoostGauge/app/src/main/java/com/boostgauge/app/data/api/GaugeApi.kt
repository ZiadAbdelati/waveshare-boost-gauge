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

/**
 * Outcome of POST /time: the parsed gauge state plus whether the gauge's RTC
 * refused the phone clock (409 `clock_rejected`) and the request fell back to
 * a timezone-only retry. `clockRejected` is always false for a forced set — a
 * forced set that is still rejected is an error, not a fallback.
 */
data class TimeSyncResult(val status: Status, val clockRejected: Boolean)

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

    /** PUT /sensors/supply; firmware range-checks against BOOST_MAP_SUPPLY_MIN/MAX. */
    suspend fun setSupplyVolts(volts: Double): Calibration =
        parse(send("PUT", "sensors/supply", buildJsonObject { put("supplyVolts", volts) }.toString()))

    /**
     * POST /time with the phone epoch for clock calibration plus the timezone.
     * The gauge's DS3231 RTC stays authoritative: if it disagrees with the
     * phone by more than BOOST_RTC_SYNC_TOLERANCE_MS the firmware answers 409
     * `clock_rejected`, and the request is retried WITHOUT `epochMs` so the
     * timezone still lands (the retry's outcome is reported via
     * [TimeSyncResult.clockRejected]). A body omitting either timezone field is
     * rejected with 400 regardless of `epochMs`; `epochMs` itself is optional.
     *
     * `force = true` is the deliberate recovery for an RTC that is genuinely
     * wrong: the body carries `"force":true`, the disagreement guard is skipped
     * by the firmware, and a 409 is NOT retried — a forced set that is still
     * refused surfaces as [ApiException] (as does one below the plausibility
     * floor, 400 `time_not_set`).
     */
    suspend fun syncTime(
        timezoneOffsetMinutes: Int,
        timezoneTz: String,
        force: Boolean = false,
    ): TimeSyncResult {
        val transport = transportProvider()
        val calibration = transport.send("POST", "time", buildJsonObject {
            put("epochMs", System.currentTimeMillis())
            put("timezoneOffsetMinutes", timezoneOffsetMinutes)
            put("timezoneTz", timezoneTz)
            if (force) put("force", true)
        }.toString())
        if (force) {
            // No fallback: the user explicitly asked to overwrite the clock, so
            // a refused forced set is an error the caller must show.
            return TimeSyncResult(parse(check(calibration)), clockRejected = false)
        }
        // The raw status is inspected before check(): 409 is the calibration
        // refusal, not a fatal error, so the timezone-only retry must happen
        // instead of throwing.
        val rejected = calibration.status == 409
        val resp = if (rejected) {
            transport.send("POST", "time", buildJsonObject {
                put("timezoneOffsetMinutes", timezoneOffsetMinutes)
                put("timezoneTz", timezoneTz)
            }.toString())
        } else {
            calibration
        }
        return TimeSyncResult(parse(check(resp)), clockRejected = rejected)
    }

    suspend fun getNetworkStatus(): NetworkStatus = parse(get("network"))
    suspend fun scanWifi(): WifiScanPayload = parse(get("network/scan"))
    suspend fun updateNetwork(ssid: String, password: String?): NetworkStatus =
        parse(send("PUT", "network", buildJsonObject {
            put("ssid", ssid)
            if (!password.isNullOrBlank()) put("password", password)
            put("mode", "apsta")
        }.toString()))
    suspend fun deleteSavedNetwork(ssid: String): NetworkStatus =
        parse(send("DELETE", "network", buildJsonObject { put("ssid", ssid) }.toString()))
    suspend fun reconnectNetwork(): NetworkStatus =
        parse(send("POST", "network/reconnect", "{}"))

    /** Password-less PUT with keepPassword=true: the gauge reuses the stored
     *  PSK for this SSID if it has one (phone-network handoff). */
    suspend fun usePhoneWifi(ssid: String): NetworkStatus =
        parse(send("PUT", "network", buildJsonObject {
            put("ssid", ssid)
            put("mode", "apsta")
            put("keepPassword", true)
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
