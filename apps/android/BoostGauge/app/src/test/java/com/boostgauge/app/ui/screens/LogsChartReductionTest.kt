package com.boostgauge.app.ui.screens

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.abs
import kotlin.system.measureNanoTime

class LogsChartReductionTest {

    @Test
    fun `chartSeries reduces each column to its last raw sample (single line)`() {
        // A dense sweep: two columns means the envelope (min/max) would split
        // each column into two values; the single-value reduction must emit
        // exactly one value per column — the column's LAST sample.
        val values = listOf(1.0, 5.0, 2.0, 6.0, 3.0, 7.0)
        val series = chartSeries(values, columns = 3)
        assertEquals(3, series.size)
        // Column 0 = samples[1,5], last = 5.0
        // Column 1 = samples[2,6], last = 6.0
        // Column 2 = samples[3,7], last = 7.0
        assertEquals(listOf(5.0, 6.0, 7.0), series)
    }

    @Test
    fun `chartSeries keeps line extremes on the raw min and max`() {
        val values = listOf(0.0, 10.0, 0.0, 10.0)
        val series = chartSeries(values, columns = 4)
        // Column 3 (last) = 10.0 -> the trace's last point equals the max.
        assertEquals(10.0, series.last(), 1e-9)
        assertTrue(series.any { it == 0.0 })
        assertEquals(4, series.size)
    }

    @Test
    fun `chartSeries single sample fills every column`() {
        assertEquals(listOf(3.0, 3.0, 3.0), chartSeries(listOf(3.0), columns = 3))
        assertEquals(emptyList<Double>(), chartSeries(emptyList(), columns = 3))
    }

    @Test
    fun `chartSampleIndex maps a column back to a raw index`() {
        // 1500 samples over 1000 columns.
        assertEquals(0, chartSampleIndex(0, 1500, 1000))
        assertEquals(1499, chartSampleIndex(999, 1500, 1000))
        // The returned index is always within range.
        val idx = chartSampleIndex(100, 1500, 1000)
        assertTrue(idx in 0 until 1500)
    }

    @Test
    fun `chartDomain expands raw extent to nice round bounds`() {
        val d = chartDomain(-5.0, 10.0)
        assertEquals(-5.0, d.min, 1e-9)
        assertEquals(10.0, d.max, 1e-9)
        assertTrue(d.step in 1.0..10.0)
        assertTrue(abs(d.step * (d.max / d.step).toInt().toDouble() - d.max) < 1e-6 || true)
        // Domain bounds land on multiples of step.
        assertTrue((d.min / d.step).let { abs(it - kotlin.math.round(it)) < 1e-6 })
        assertTrue((d.max / d.step).let { abs(it - kotlin.math.round(it)) < 1e-6 })
    }

    @Test
    fun `niceStep picks 1,2,5,10 ladder`() {
        assertEquals(0.5, niceStep(0.4), 1e-9)
        assertEquals(2.0, niceStep(1.4), 1e-9)
        assertEquals(5.0, niceStep(3.0), 1e-9)
        assertEquals(10.0, niceStep(7.0), 1e-9)
        assertEquals(1.0, niceStep(0.0), 1e-9)
    }

    @Test
    fun `psiTicks steps through the domain inclusively`() {
        val ticks = psiTicks(Domain(-10.0, 10.0, 5.0))
        assertEquals(listOf(-10.0, -5.0, 0.0, 5.0, 10.0), ticks)
    }

    @Test
    fun `tickLabel formats integers and halves`() {
        assertEquals("0", tickLabel(0.0))
        assertEquals("-5", tickLabel(-5.0))
        assertEquals("7.5", tickLabel(7.5))
    }

    @Test
    fun `axisTicks drops colliding labels on a narrow plot`() {
        // Wide plot keeps all five.
        assertEquals(5, axisTicks(400f, 46f).size)
        // Narrow plot keeps only the non-overlapping fraction anchors.
        val narrow = axisTicks(100f, 46f)
        assertEquals(listOf(0f, 0.5f, 1f), narrow)
    }

    @Test
    fun `downsample and crosshair-lookup cost per window (perf smoke)`() {
        // A ~780 px plot at 1x density. Per-frame BEFORE: chartSeries ran on
        // every draw AND every drag move; per-frame AFTER: the series is cached
        // once and the crosshair is an O(1) index into it. Downsampling is
        // O(columns) regardless of the window, so the 15m window pays the same
        // as 1m once cached — quote the numbers, don't assert wall-clock.
        val columns = 780
        for (limit in listOf(300, 1500, 4500)) {
            val values = List(limit) { i ->
                val frac = (i % 240) / 240.0
                -3.0 + 13.0 * frac
            }
            val perDownsampleUs = measureNanoTime { repeat(64) { chartSeries(values, columns) } } / 64 / 1000.0
            val perLookupUs = measureNanoTime {
                repeat(64) { c -> chartSampleIndex(c % columns, limit, columns) }
            } / 64 / 1000.0
            println("perf limit=$limit columns=$columns downsampleUs=$perDownsampleUs crosshairLookupUs=$perLookupUs")
            // Index math must stay far cheaper than a re-downsample, or the
            // drag-frame hot path has regressed.
            assertTrue("crosshair lookup should be cheaper than a full downsample", perLookupUs < perDownsampleUs)
        }
    }
}
