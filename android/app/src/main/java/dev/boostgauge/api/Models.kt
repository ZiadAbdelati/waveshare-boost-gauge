package dev.boostgauge.api

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class GaugeState(
    val psi: Double = 0.0,
    val peakPsi: Double = 0.0,
    val zone: String = "vacuum",
    val demo: Boolean = false,
    val brightness: Int = 0,
    val firmwareVersion: String = "",
    val uptimeMs: Long = 0,
    val epochMs: Long = 0,
    val timezoneOffsetMinutes: Int = 0,
    val activeThemeId: String = "pit-lane"
)

@Serializable
data class GaugeConfig(
    val brightnessHigh: Int = 100,
    val brightnessLow: Int = 20,
    val dimSchedule: DimSchedule = DimSchedule(),
    val timezoneOffsetMinutes: Int = 0,
    val activeThemeId: String = "pit-lane"
)

@Serializable
data class DimSchedule(
    val enabled: Boolean = false,
    val startMinutes: Int = 21 * 60,
    val endMinutes: Int = 7 * 60
)

@Serializable
data class TimeSyncRequest(
    val epochMs: Long,
    val timezoneOffsetMinutes: Int
)

@Serializable
data class ThemeCatalog(
    @SerialName("themes") val themes: List<GaugeTheme> = emptyList(),
    @SerialName("activeThemeId") val activeThemeId: String = ""
)

@Serializable
data class GaugeTheme(
    val id: String,
    val name: String = id,
    val colors: ThemeColors = ThemeColors(),
    val brightnessHigh: Int? = null,
    val brightnessLow: Int? = null
)

@Serializable
data class ThemeColors(
    val face: String = "#090A0D",
    val track: String = "#20242C",
    val text: String = "#E8ECF2",
    val muted: String = "#808792",
    val vacuum: String = "#2EE6C5",
    val boost: String = "#FFB020",
    val overboost: String = "#FF3B30",
    val zero: String = "#E8ECF2"
)

@Serializable
data class ActiveThemeRequest(val id: String)

@Serializable
data class LogResponse(
    val samples: List<LogSample> = emptyList(),
    val count: Int = samples.size,
    val limit: Int = samples.size
)

@Serializable
data class LogSample(
    val epochMs: Long = 0,
    val uptimeMs: Long = 0,
    val psi: Double = 0.0,
    val peakPsi: Double = 0.0,
    val zone: String = ""
)
