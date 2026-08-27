package com.boostgauge.app.ui

import java.util.TimeZone
import kotlin.math.abs

/**
 * Curated POSIX TZ table for the gauge's clock & timezone settings. Each entry
 * carries the standard (raw) offset and the POSIX TZ string the firmware
 * applies via setenv("TZ")+tzset(). The IANA ids let the app pick the entry
 * that matches the phone's default zone; unknown zones fall back to a
 * fixed-offset POSIX string.
 */
data class TimezoneEntry(
    val label: String,
    val ids: Set<String>,
    val offsetMinutes: Int,
    val posix: String,
)

object Timezones {

    val curated: List<TimezoneEntry> = listOf(
        TimezoneEntry("UTC", setOf("UTC", "Etc/UTC", "Etc/GMT", "GMT"), 0, "UTC0"),
        TimezoneEntry(
            "US Eastern",
            setOf("America/New_York", "US/Eastern"),
            -300,
            "EST5EDT,M3.2.0/2,M11.1.0/2",
        ),
        TimezoneEntry(
            "US Central",
            setOf("America/Chicago", "US/Central"),
            -360,
            "CST6CDT,M3.2.0/2,M11.1.0/2",
        ),
        TimezoneEntry(
            "US Mountain",
            setOf("America/Denver", "US/Mountain"),
            -420,
            "MST7MDT,M3.2.0/2,M11.1.0/2",
        ),
        TimezoneEntry(
            "US Pacific",
            setOf("America/Los_Angeles", "US/Pacific"),
            -480,
            "PST8PDT,M3.2.0/2,M11.1.0/2",
        ),
        TimezoneEntry(
            "US Alaska",
            setOf("America/Anchorage", "US/Alaska"),
            -540,
            "AKST9AKDT,M3.2.0/2,M11.1.0/2",
        ),
        TimezoneEntry("US Hawaii", setOf("Pacific/Honolulu", "US/Hawaii"), -600, "HST10"),
        TimezoneEntry(
            "UK",
            setOf("Europe/London", "GMT0BST", "Europe/Dublin"),
            0,
            "GMT0BST,M3.5.0/1,M10.5.0/2",
        ),
        TimezoneEntry(
            "Central Europe",
            setOf("Europe/Berlin", "Europe/Paris", "Europe/Madrid", "Europe/Rome", "Europe/Amsterdam"),
            60,
            "CET-1CEST,M3.5.0/2,M10.5.0/3",
        ),
        TimezoneEntry("India", setOf("Asia/Kolkata", "Asia/Calcutta"), 330, "IST-5:30"),
        TimezoneEntry("Japan", setOf("Asia/Tokyo"), 540, "JST-9"),
        TimezoneEntry(
            "Australia East",
            setOf("Australia/Sydney", "Australia/Melbourne"),
            600,
            "AEST-10AEDT,M10.1.0/2,M4.1.0/3",
        ),
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