package com.boostgauge.app.data.transport

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.assertThrows
import org.junit.Test

class BleLogParserTest {

    @Test
    fun parsesBgl1HeaderAndCsvRows() {
        val payload = """
            BGL1
            t_ms,psi,peak_psi,zone,demo
            1000,0.12,0.34,ATMO,1
            1200,1.02,1.02,BOOST,1
        """.trimIndent()

        val rows = BleLogParser.parse(payload)

        assertEquals(2, rows.size)
        assertEquals(1000L, rows[0].tMs)
        assertEquals(0.12, rows[0].psi, 0.001)
        assertEquals("ATMO", rows[0].zone)
        assertTrue(rows[0].demo)
    }

    @Test
    fun requiresBgl1Header() {
        assertThrows(TransportException::class.java) { BleLogParser.parse("") }
        assertThrows(TransportException::class.java) { BleLogParser.parse("not-a-log") }
        assertTrue(BleLogParser.parse("BGL1\n").isEmpty())
    }

    @Test
    fun malformedRowsAreSkipped() {
        val payload = "BGL1\n1000,0.12,0.34,ATMO,1\nbad-line\n"
        val rows = BleLogParser.parse(payload)
        assertEquals(1, rows.size)
    }
}
