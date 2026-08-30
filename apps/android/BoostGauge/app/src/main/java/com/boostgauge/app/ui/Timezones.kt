package com.boostgauge.app.ui

import java.util.TimeZone
import kotlin.math.abs

/**
 * Curated POSIX TZ table for the gauge's clock & timezone settings. Each entry
 * carries the standard (raw) offset and the POSIX TZ string the firmware
 * applies via setenv("TZ")+tzset(). The IANA ids let the app pick the entry
 * that matches the phone's default zone; unknown zones fall back to a
 * fixed-offset POSIX string.
 *
 * Generated from the canonical zone list in web/tz.json (the phone-relevant
 * entries that carry IANA ids); the JSON posix strings are copied verbatim.
 */
data class TimezoneEntry(
    val label: String,
    val ids: Set<String>,
    val offsetMinutes: Int,
    val posix: String,
)

object Timezones {

    val curated: List<TimezoneEntry> = listOf(
        TimezoneEntry("UTC-10:00 · Hawaii", setOf("Pacific/Honolulu", "US/Hawaii"), -600, "HST10"),
        TimezoneEntry("UTC-09:00 · Alaska", setOf("America/Anchorage", "US/Alaska"), -540, "AKST9AKDT,M3.2.0/2,M11.1.0/2"),
        TimezoneEntry("UTC-08:00 · Pacific Time", setOf("America/Los_Angeles", "US/Pacific"), -480, "PST8PDT,M3.2.0/2,M11.1.0/2"),
        TimezoneEntry("UTC-07:00 · Mountain Time", setOf("America/Denver", "US/Mountain"), -420, "MST7MDT,M3.2.0/2,M11.1.0/2"),
        TimezoneEntry("UTC-06:00 · Central Time", setOf("America/Chicago", "US/Central"), -360, "CST6CDT,M3.2.0/2,M11.1.0/2"),
        TimezoneEntry("UTC-05:00 · Eastern Time", setOf("America/New_York", "US/Eastern"), -300, "EST5EDT,M3.2.0/2,M11.1.0/2"),
        TimezoneEntry("UTC+00:00 · London / UTC", setOf("UTC", "Etc/UTC", "Etc/GMT", "GMT", "Europe/London", "Europe/Dublin"), 0, "GMT0BST,M3.5.0/1,M10.5.0/2"),
        TimezoneEntry("UTC+01:00 · Central Europe / West Africa", setOf("Europe/Berlin", "Europe/Paris", "Europe/Madrid", "Europe/Rome", "Europe/Amsterdam"), 60, "CET-1CEST,M3.5.0/2,M10.5.0/3"),
        TimezoneEntry("UTC+05:30 · Mumbai / Colombo", setOf("Asia/Kolkata", "Asia/Calcutta"), 330, "IST-5:30"),
        TimezoneEntry("UTC+09:00 · Tokyo / Seoul / Yakutsk", setOf("Asia/Tokyo"), 540, "JST-9"),
        TimezoneEntry("UTC+10:00 · Sydney / Brisbane / Vladivostok", setOf("Australia/Sydney", "Australia/Melbourne"), 600, "AEST-10AEDT,M10.1.0/2,M4.1.0/3"),
    )

    /**
     * Standard offset + POSIX TZ for the phone's default zone. Matched against
     * the curated table by IANA id; anything else degrades to a fixed-offset
     * POSIX string (the firmware derives DST from the string when present).
     */
    fun forDefault(tz: TimeZone = TimeZone.getDefault()): TimezoneEntry {
        curated.firstOrNull { tz.id in it.ids }?.let { return it }
        val offsetMinutes = tz.rawOffset / 60_000
        val label = tz.getDisplayName(false, TimeZone.SHORT).takeIf { it.isNotBlank() } ?: tz.id
        return TimezoneEntry(label, emptySet(), offsetMinutes, posixFromOffset(offsetMinutes))
    }

    /** Fixed-offset POSIX TZ string ("UTC0", "UTC-5", "IST-5:30") from a standard offset. */
    fun posixFromOffset(offsetMinutes: Int): String {
        val posix = -offsetMinutes
        val hours = abs(posix) / 60
        val minutes = abs(posix) % 60
        return if (minutes == 0) {
            if (posix < 0) "UTC-$hours" else "UTC$hours"
        } else {
            "UTC${if (posix < 0) "-" else "+"}$hours:%02d".format(minutes)
        }
    }
}