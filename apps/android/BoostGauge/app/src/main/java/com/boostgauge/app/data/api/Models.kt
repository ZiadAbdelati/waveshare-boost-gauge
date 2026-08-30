package com.boostgauge.app.data.api

import kotlinx.serialization.Serializable

/** /api/v1/state payload (boost_web.c state_json()). */
@Serializable
data class Status(
    val psi: Double = 0.0,
    val peakPsi: Double = 0.0,
    val zone: String = "",
    val demo: Boolean = false,
    val firmwareVersion: String = "",
    val uptimeMs: Long = 0L,
    val epochMs: Long = 0L,
    val timezoneOffsetMinutes: Int = 0,
    val activeThemeId: String = "",
    val activePage: Int = 0,
    val sensors: Sensors = Sensors(),
    val tpms: Tpms = Tpms(),
    val obd: Obd = Obd(),
)

@Serializable
data class Sensors(
    val adsPresent: Boolean = false,
    val bmpPresent: Boolean = false,
    val fault: Boolean = false,
    val mapVolts: Double = 0.0,
    val mapAbsKpa: Double = 0.0,
    val ambientKpa: Double = 0.0,
)

@Serializable
data class Tpms(
    val status: Int = 0,
    val lowPsi: Double = 0.0,
    val wheels: List<Wheel> = emptyList(),
)

@Serializable
data class Wheel(
    val psi: Double = 0.0,
    val valid: Boolean = false,
)

@Serializable
data class Obd(
    val state: Int = 0,
    val lastError: Long = 0L,
    val peer: String = "",
    val peerAddr: String = "",
    val uptimeMs: Long = 0L,
    val ageMs: Long = 0L,
    val valid: Boolean = false,
    val rpm: Double = 0.0,
    val speedKph: Double = 0.0,
    val coolantC: Double = 0.0,
    val batteryV: Double = 0.0,
)

/** /api/v1/config payload (boost_web.c config_json()). */
@Serializable
data class Config(
    val brightnessHigh: Int = 92,
    val brightnessLow: Int = 10,
    val dimSchedule: DimSchedule = DimSchedule(),
    val timezoneOffsetMinutes: Int = 0,
    val timezoneTz: String = "",
    val activeThemeId: String = "dyno-cell",
    val psiMin: Double = -15.0,
    val psiMax: Double = 10.0,
    val psiOverboost: Double = 8.0,
    val zeroAngle: Double = 0.0,
    val appBle: Boolean = false,
)

@Serializable
data class DimSchedule(
    val enabled: Boolean = false,
    val startMinutes: Int = 0,
    val endMinutes: Int = 0,
)

/** /api/v1/themes payload (boost_web.c themes_get()). */
@Serializable
data class ThemesPayload(
    val activeThemeId: String = "",
    val bigDigitStaticBg: Boolean = false,
    val bigDigitColorText: Boolean = false,
    val bigDigitStaticColor: String = "",
    val bigDigitTextColor: String = "",
    val arcGradient: Boolean = false,
    val hudGradient: Boolean = false,
    val hudTrueBlack: Boolean = false,
    val neonMarqueeSpin: Boolean = false,
    val teSync: Boolean = false,
    val regionDBuf: Boolean = false,
    val teScanline: Boolean = false,
    val rotation: Int = 0,
    val vaultFace: String = "",
    val vaultVignette: Int = 0,
    val vaultNeedleRed: Boolean = false,
    val vaultNeedleTail: Boolean = false,
    val neonLayout: Int = 0,
    val neonFont: Int = 0,
    val neonPreset: Int = 0,
    val demoMode: Boolean = false,
    val demoFastSweep: Boolean = false,
    val tpmsBle: Boolean = false,
    val pixelShift: Boolean = false,
    val pixelShiftSec: Int = 90,
    val themes: List<ThemeInfo> = emptyList(),
)

@Serializable
data class ThemeInfo(
    val id: String = "",
    val name: String = "",
    val style: String = "",
    val colors: ThemeColors = ThemeColors(),
    val customized: Boolean = false,
)

@Serializable
data class ThemeColors(
    val face: String = "",
    val track: String = "",
    val text: String = "",
    val muted: String = "",
    val vacuum: String = "",
    val boost: String = "",
    val overboost: String = "",
    val zero: String = "",
)

/** /api/v1/sensors/calibration payload (boost_web.c sensors_calibration_json()). */
@Serializable
data class Calibration(
    val supplyVolts: Double = 0.0,
    val live: CalibrationLive = CalibrationLive(),
    val calibration: CalibrationValues = CalibrationValues(),
)

@Serializable
data class CalibrationLive(
    val adsPresent: Boolean = false,
    val bmpPresent: Boolean = false,
    val fault: Boolean = false,
    val mapVolts: Double = 0.0,
    val mapAgeMs: Long = 0L,
    val nominalKpa: Double = 0.0,
    val correctedKpa: Double = 0.0,
    val bmpKpa: Double = 0.0,
    val bmpAgeMs: Long = 0L,
    val bmpUpdates: Long = 0L,
    val ambientIsFallback: Boolean = false,
)

@Serializable
data class CalibrationValues(
    val valid: Boolean = false,
    val version: Int = 0,
    val offsetKpa: Double = 0.0,
    val offsetPsi: Double = 0.0,
    val supplyVolts: Double = 0.0,
    val refMapVolts: Double = 0.0,
    val refNominalKpa: Double = 0.0,
    val refBmpKpa: Double = 0.0,
    val samples: Int = 0,
    val epochMs: Long = 0L,
)

/** /api/v1/logs payload (boost_web.c logs_get()). */
@Serializable
data class LogsPayload(
    val samples: List<LogSample> = emptyList(),
)

@Serializable
data class LogSample(
    val tMs: Long = 0L,
    val psi: Double = 0.0,
    val peakPsi: Double = 0.0,
    val zone: String = "",
    val demo: Boolean = false,
)

/** /api/v1/tpms/config payload (boost_web.c tpms_config_json()). */
@Serializable
data class TpmsConfig(
    val lowKpa: Double = 0.0,
    val lowPsi: Double = 0.0,
    val staleAfterMs: Long = 15_000L,
)

/** /api/v1/network payload (boost_web.c network_status_json()). */
@Serializable
data class NetworkStatus(
    val mode: String = "ap",
    val staEnabled: Boolean = false,
    val staConnected: Boolean = false,
    val staSsid: String = "",
    val staIp: String = "",
    val apSsid: String = "",
    val rssi: Int = 0,
    val saved: List<SavedNetwork> = emptyList(),
)

@Serializable
data class SavedNetwork(val ssid: String = "")

@Serializable
data class WifiScanPayload(val networks: List<WifiNetwork> = emptyList())

@Serializable
data class WifiNetwork(val ssid: String = "", val rssi: Int = 0)

/** Error body shape returned by send_err(). */
@Serializable
data class ErrorBody(val error: String = "")
