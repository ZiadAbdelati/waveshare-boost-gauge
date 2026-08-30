package com.boostgauge.app.ui

import java.util.TimeZone
import org.junit.Assert.assertEquals
import org.junit.Test

class TimezonesTest {

    @Test
    fun posixFromOffsetMatchesIosFixedOffsetShape() {
        assertEquals("UTC0", Timezones.posixFromOffset(0))
        assertEquals("UTC5", Timezones.posixFromOffset(-300))
        assertEquals("UTC-5:30", Timezones.posixFromOffset(330))
        assertEquals("UTC-5:45", Timezones.posixFromOffset(345))
    }

    @Test
    fun forDefaultMatchesCuratedEntryByIanaId() {
        val entry = Timezones.forDefault(TimeZone.getTimeZone("America/New_York"))
        assertEquals("UTC-05:00 · Eastern Time", entry.label)
        assertEquals(-300, entry.offsetMinutes)
        assertEquals("EST5EDT,M3.2.0/2,M11.1.0/2", entry.posix)
    }

    @Test
    fun forDefaultFallsBackToFixedOffsetForUnknownZones() {
        val entry = Timezones.forDefault(TimeZone.getTimeZone("Asia/Kathmandu"))
        assertEquals(345, entry.offsetMinutes)
        assertEquals("UTC-5:45", entry.posix)
    }
}