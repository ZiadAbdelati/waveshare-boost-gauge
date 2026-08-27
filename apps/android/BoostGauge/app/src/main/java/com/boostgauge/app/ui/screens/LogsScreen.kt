package com.boostgauge.app.ui.screens

import android.app.Activity
import android.graphics.Paint
import android.graphics.Typeface
import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.drag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowDownward
import androidx.compose.material.icons.filled.ArrowUpward
import androidx.compose.material.icons.filled.Inbox
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Share
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.LogSample
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.ui.BoostCardShape
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostMonoCaption
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.BoostSectionHeader
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.Format
import com.boostgauge.app.ui.viewmodels.LogsViewModel
import com.boostgauge.app.ui.viewmodels.LogWindow
import kotlinx.coroutines.launch
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter
import java.util.Locale
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt

private val CHART_CYAN = Color(0xFF00E5FF)
private const val TAG = "LogsChart"

@Composable
fun LogsScreen(container: AppContainer) {
    val viewModel: LogsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                LogsViewModel(container.api, container.repository)
            }
        },
    )
    val state by viewModel.state.collectAsState()
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var exportDone by remember { mutableStateOf(false) }
    val exportLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.CreateDocument("text/csv"),
    ) { uri ->
        if (uri != null) {
            scope.launch {
                runCatching {
                    context.contentResolver.openOutputStream(uri)?.use { out ->
                        out.write(viewModel.exportCsv().toByteArray(Charsets.UTF_8))
                    }
                }.onSuccess { exportDone = true }
            }
        }
    }
    // Screenshot/e2e fixture only: mirror of iOS -e2eCrosshair. Shows the
    // crosshair at the newest sample when no finger is down so a capture is
    // deterministic. A real touch always owns the state; release clears it.
    val fixtureCrosshair = remember {
        (context as? Activity)?.intent?.getBooleanExtra("e2eCrosshair", false) == true
    }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 24.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "Logs",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Row(verticalAlignment = Alignment.CenterVertically) {
                    IconButton(onClick = { viewModel.load(force = true) }) {
                        Icon(
                            Icons.Filled.Refresh,
                            contentDescription = "Refresh logs",
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    IconButton(
                        onClick = { exportLauncher.launch("boost-gauge-log.csv") },
                        enabled = state.samples.isNotEmpty(),
                    ) {
                        Icon(
                            Icons.Filled.Share,
                            contentDescription = "Export CSV",
                            tint = if (state.samples.isNotEmpty()) MaterialTheme.colorScheme.onSurfaceVariant else MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.38f),
                        )
                    }
                }
            }
        }

        if (exportDone) {
            item {
                Text(
                    text = "CSV exported",
                    style = BoostFootnote,
                    color = BoostColors.success,
                )
            }
        }
        state.error?.let {
            item {
                Text(
                    text = it,
                    style = BoostFootnote,
                    color = BoostColors.warning,
                )
            }
        }

        if (state.loading && state.samples.isEmpty()) {
            item {
                Box(
                    modifier = Modifier.fillMaxWidth().padding(40.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator()
                }
            }
        } else if (state.samples.isEmpty()) {
            item {
                EmptyLogs(
                    error = state.error,
                    onLoad = { viewModel.load() },
                )
            }
        } else {
            item {
                Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    Text(
                        text = "Pressure history",
                        style = BoostSectionHeader,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Card(
                        modifier = Modifier.fillMaxWidth(),
                        shape = BoostCardShape,
                        colors = CardDefaults.cardColors(containerColor = MaterialTheme.colorScheme.surface),
                    ) {
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(16.dp),
                            verticalArrangement = Arrangement.spacedBy(12.dp),
                        ) {
                            Row(
                                modifier = Modifier.fillMaxWidth(),
                                verticalAlignment = Alignment.CenterVertically,
                                horizontalArrangement = Arrangement.spacedBy(8.dp),
                            ) {
                                LogWindow.all.forEach { window ->
                                    WindowChip(
                                        title = window.title,
                                        selected = state.window == window,
                                        enabled = !state.loading,
                                        onClick = { viewModel.load(window.limit) },
                                    )
                                }
                                Spacer(Modifier.weight(1f))
                                Text(
                                    text = state.source,
                                    style = BoostMonoCaption,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                    modifier = Modifier.testTag("logsSampleCount"),
                                )
                            }

                            HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))

                            LogPressureChart(
                                samples = state.samples,
                                anchor = viewModel.anchor(),
                                fixtureCrosshair = fixtureCrosshair,
                            )

                            val min = state.samples.minOfOrNull { it.psi }
                            val max = state.samples.maxOfOrNull { it.psi }
                            if (min != null && max != null) {
                                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
                                Row(
                                    modifier = Modifier.fillMaxWidth(),
                                    horizontalArrangement = Arrangement.SpaceBetween,
                                    verticalAlignment = Alignment.CenterVertically,
                                ) {
                                    Row(
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                                    ) {
                                        Icon(
                                            Icons.Filled.ArrowDownward,
                                            contentDescription = null,
                                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                            modifier = Modifier.size(14.dp),
                                        )
                                        Text(
                                            text = "Min ${Format.fmt(min, 2)} psi",
                                            style = BoostMonoCaption,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                    }
                                    Row(
                                        verticalAlignment = Alignment.CenterVertically,
                                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                                    ) {
                                        Icon(
                                            Icons.Filled.ArrowUpward,
                                            contentDescription = null,
                                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                                            modifier = Modifier.size(14.dp),
                                        )
                                        Text(
                                            text = "Max ${Format.fmt(max, 2)} psi",
                                            style = BoostMonoCaption,
                                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                                        )
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun WindowChip(title: String, selected: Boolean, enabled: Boolean, onClick: () -> Unit) {
    val bg = if (selected) {
        CHART_CYAN.copy(alpha = 0.22f)
    } else {
        MaterialTheme.colorScheme.surfaceVariant
    }
    val fg = if (selected) CHART_CYAN else MaterialTheme.colorScheme.onSurfaceVariant
    Box(
        modifier = Modifier
            .clip(CircleShape)
            .background(bg)
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 5.dp),
    ) {
        Text(
            text = title,
            style = BoostCaptionSemibold,
            color = fg,
            modifier = Modifier.testTag("logsWindow.$title"),
        )
    }
}

data class Domain(val min: Double, val max: Double, val step: Double)

private data class Crosshair(val x: Float, val y: Float, val value: Double, val sampleIndex: Int)

@Composable
private fun LogPressureChart(
    samples: List<LogSample>,
    anchor: Status?,
    fixtureCrosshair: Boolean = false,
) {
    val values = remember(samples) { samples.map { it.psi } }
    val domain = remember(values) {
        chartDomain(min(values.minOrNull() ?: 0.0, 0.0), max(values.maxOrNull() ?: 1.0, 0.0))
    }
    val gridColor = MaterialTheme.colorScheme.onSurfaceVariant
    val zeroColor = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.45f)
    val density = LocalDensity.current
    var boxSize by remember { mutableStateOf(IntSize.Zero) }
    val plot = remember(boxSize, density) {
        if (boxSize == IntSize.Zero) null else with(density) { plotRectPx(boxSize) }
    }
    val columns = remember(plot) { if (plot == null) 0 else max(plot.width.roundToInt(), 1) }
    // Downsample ONCE per (samples identity, columns): the path and the
    // crosshair both build from this single cached series — never per frame.
    val series = remember(values, columns) {
        val t0 = System.nanoTime()
        chartSeries(values, columns).also {
            Log.d(TAG, "downsample samples=${values.size} columns=$columns ${(System.nanoTime() - t0) / 1000}us")
        }
    }
    val span = max(domain.max - domain.min, 1.0)
    val tracePath = remember(series, plot, domain) {
        if (plot == null || series.isEmpty()) null else Path().also { path ->
            val t0 = System.nanoTime()
            traceSeries(series, plot, domain, span, path)
            Log.d(TAG, "path samples=${values.size} columns=${series.size} ${(System.nanoTime() - t0) / 1000}us")
        }
    }
    var userCrosshair by remember { mutableStateOf<Crosshair?>(null) }
    var userTouched by remember { mutableStateOf(false) }

    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(200.dp)
            .clip(RoundedCornerShape(10.dp))
            .background(MaterialTheme.colorScheme.surfaceVariant)
            .onSizeChanged { boxSize = it }
            .pointerInput(plot, series) {
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    userTouched = true
                    userCrosshair = crosshairFor(down.position, plot, series, values.size, domain)
                    drag(down.id) { change ->
                        userCrosshair = crosshairFor(change.position, plot, series, values.size, domain)
                        change.consume()
                    }
                    userCrosshair = null
                }
            },
    ) {
        Canvas(modifier = Modifier.fillMaxSize()) {
            drawChart(values, domain, samples, anchor, plot, series, tracePath, userCrosshair, userTouched, fixtureCrosshair, gridColor, zeroColor)
        }
    }
}

private fun DrawScope.drawChart(
    values: List<Double>,
    domain: Domain,
    samples: List<LogSample>,
    anchor: Status?,
    plot: Rect?,
    series: List<Double>,
    tracePath: Path?,
    userCrosshair: Crosshair?,
    userTouched: Boolean,
    fixtureCrosshair: Boolean,
    gridColor: Color,
    zeroColor: Color,
) {
    if (plot == null) return
    val span = max(domain.max - domain.min, 1.0)

    // Horizontal psi gridlines + value labels (left edge).
    psiTicks(domain).forEach { tick ->
        val y = plot.top + plot.height * ((domain.max - tick) / span).toFloat()
        drawLine(
            color = gridColor.copy(alpha = 0.15f),
            start = Offset(plot.left, y),
            end = Offset(plot.right, y),
            strokeWidth = 1f,
        )
        drawRightText(tickLabel(tick), rightX = plot.left - 6.dp.toPx(), centerY = y, color = gridColor)
    }

    // Zero line (dashed, slightly stronger).
    val zeroY = plot.top + plot.height * ((domain.max - 0.0) / span).toFloat()
    drawLine(
        color = zeroColor,
        start = Offset(plot.left, zeroY),
        end = Offset(plot.right, zeroY),
        strokeWidth = 1f,
        pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f, 8f)),
    )

    // ONE series: a single representative psi value per display column (the
    // column's last sample). The old min/max envelope stroked BOTH the
    // per-column maxima and minima as separate cyan lines, which render as two
    // distinct squiggles on a dense sweep. The path was already built from the
    // cached downsampled series before draw — never rebuild it per frame.
    tracePath?.let {
        drawPath(
            it,
            color = CHART_CYAN,
            style = Stroke(width = 2.dp.toPx(), cap = StrokeCap.Round, join = StrokeJoin.Round),
        )
    }

    // Time ticks along the bottom (HH:mm), collision-guarded so labels never overlap.
    val minSpacing = 46.dp.toPx()
    val margin = 30.dp.toPx()
    axisTicks(plot.width, minSpacing).forEach { fraction ->
        val x = min(max(plot.left + plot.width * fraction, plot.left + margin), plot.right - margin)
        val sampleIndex = min((fraction * (values.size - 1).toDouble()).roundToInt(), max(values.size - 1, 0))
        drawCenteredText(
            axisTimeFor(samples, sampleIndex, anchor),
            centerX = x,
            centerY = plot.bottom + 10.dp.toPx(),
            color = gridColor,
        )
    }

    // Crosshair: a real finger owns it; the -e2eCrosshair fixture fills in
    // before the first touch so a capture is deterministic.
    val fixturePoint = if (!userTouched && fixtureCrosshair) {
        val last = series.lastOrNull()
        if (last != null) {
            Crosshair(
                x = plot.right,
                y = plot.top + plot.height * ((domain.max - last) / span).toFloat(),
                value = last,
                sampleIndex = chartSampleIndex(max(series.size - 1, 0), values.size, series.size),
            )
        } else null
    } else null
    val effective = userCrosshair ?: fixturePoint
    if (effective != null) {
        drawLine(
            color = CHART_CYAN.copy(alpha = 0.6f),
            start = Offset(effective.x, plot.top),
            end = Offset(effective.x, plot.bottom),
            strokeWidth = 1f,
            pathEffect = PathEffect.dashPathEffect(floatArrayOf(3f, 3f)),
        )
        drawCircle(
            color = CHART_CYAN,
            radius = 4.dp.toPx(),
            center = Offset(effective.x, effective.y),
        )
        val text = "${psiLabel(effective.value)} psi · ${timeTextFor(samples, effective.sampleIndex, anchor)}"
        drawPill(text, plot)
    }
}

/** O(1) per drag frame: index the cached downsampled series, never rebuild it. */
private fun crosshairFor(
    location: Offset,
    plot: Rect?,
    series: List<Double>,
    count: Int,
    domain: Domain,
): Crosshair? {
    if (plot == null || series.isEmpty()) return null
    val span = max(domain.max - domain.min, 1.0)
    val columns = series.size
    val x = min(max(location.x, plot.left), plot.right)
    val column = min(max(((x - plot.left) / plot.width * columns).toInt(), 0), columns - 1)
    val psiValue = series[column]
    val y = plot.top + plot.height * ((domain.max - psiValue) / span).toFloat()
    return Crosshair(
        x = x,
        y = y,
        value = psiValue,
        sampleIndex = chartSampleIndex(column, count, columns),
    )
}

private fun Density.plotRectPx(size: IntSize): Rect {
    val yAxisWidth = 38.dp.toPx()
    val xAxisHeight = 20.dp.toPx()
    val topInset = 10.dp.toPx()
    val rightInset = 10.dp.toPx()
    return Rect(
        left = yAxisWidth,
        top = topInset,
        right = max(size.width - rightInset, yAxisWidth + 1),
        bottom = max(size.height - xAxisHeight, topInset + 1),
    )
}

private fun DrawScope.drawRightText(text: String, rightX: Float, centerY: Float, color: Color) {
    val paint = axisPaint(color)
    val x = rightX - paint.measureText(text)
    drawContext.canvas.nativeCanvas.drawText(text, x, centerY - (paint.ascent() + paint.descent()) / 2, paint)
}

private fun DrawScope.drawCenteredText(text: String, centerX: Float, centerY: Float, color: Color) {
    val paint = axisPaint(color)
    drawContext.canvas.nativeCanvas.drawText(
        text,
        centerX - paint.measureText(text) / 2,
        centerY - (paint.ascent() + paint.descent()) / 2,
        paint,
    )
}

private fun DrawScope.drawPill(text: String, plot: Rect) {
    val paint = axisPaint(Color.White)
    val textW = paint.measureText(text)
    val h = 20.dp.toPx()
    val w = textW + 16.dp.toPx()
    val left = plot.left + (plot.width - w) / 2
    val top = plot.top + 4.dp.toPx()
    val r = h / 2
    drawRoundRect(
        color = CHART_CYAN,
        topLeft = Offset(left, top),
        size = androidx.compose.ui.geometry.Size(w, h),
        cornerRadius = androidx.compose.ui.geometry.CornerRadius(r, r),
    )
    drawContext.canvas.nativeCanvas.drawText(
        text,
        left + (w - textW) / 2,
        top + h / 2 - (paint.ascent() + paint.descent()) / 2,
        paint,
    )
}

private fun DrawScope.axisPaint(color: Color): Paint = Paint().apply {
    isAntiAlias = true
    textSize = 11.dp.toPx()
    typeface = Typeface.create(Typeface.MONOSPACE, Typeface.NORMAL)
    this.color = color.toArgb()
}

// ---------------------------------------------------------------------------
// Pure reduction / geometry logic (mirrors iOS LogsView.swift statics).
// ---------------------------------------------------------------------------

/** ONE value per display column: the column's last raw sample. */
fun chartSeries(values: List<Double>, columns: Int): List<Double> {
    if (values.isEmpty()) return emptyList()
    if (values.size == 1) return List(columns) { values[0] }
    return (0 until columns).map { column ->
        val start = column * values.size / columns
        val end = max(start + 1, (column + 1) * values.size / columns)
        values.subList(start, min(end, values.size)).last()
    }
}

/** Which raw sample produced a given display column (the column's last). */
fun chartSampleIndex(column: Int, count: Int, columns: Int): Int {
    if (count <= 0 || columns <= 0) return 0
    val start = column * count / columns
    val end = max(start + 1, (column + 1) * count / columns)
    return min(end - 1, count - 1)
}

fun chartDomain(minimum: Double, maximum: Double): Domain {
    val rawStep = (maximum - minimum) / 5
    val step = niceStep(rawStep)
    return Domain(
        min = Math.floor(minimum / step) * step,
        max = Math.ceil(maximum / step) * step,
        step = step,
    )
}

fun niceStep(raw: Double): Double {
    if (raw <= 0 || !raw.isFinite()) return 1.0
    val exponent = Math.floor(Math.log10(raw))
    val fraction = raw / 10.0.pow(exponent)
    val nice = when {
        fraction <= 1 -> 1
        fraction <= 2 -> 2
        fraction <= 5 -> 5
        else -> 10
    }
    return nice * 10.0.pow(exponent)
}

fun psiTicks(domain: Domain): List<Double> {
    val ticks = mutableListOf<Double>()
    var value = domain.min
    while (value <= domain.max + 1e-9) {
        ticks.add(value)
        value += domain.step
    }
    return ticks
}

fun tickLabel(value: Double): String = when {
    value == 0.0 -> "0"
    value == value.roundToInt().toDouble() -> String.format(Locale.US, "%.0f", value)
    else -> String.format(Locale.US, "%.1f", value)
}

/** Bottom time ticks at [0,.25,.5,.75,1], dropping any that would overlap. */
fun axisTicks(plotWidth: Float, minSpacing: Float): List<Float> {
    val fractions = listOf(0f, 0.25f, 0.5f, 0.75f, 1f)
    val kept = mutableListOf<Float>()
    for (fraction in fractions) {
        val x = plotWidth * fraction
        if (kept.isNotEmpty() && x - kept.last() * plotWidth < minSpacing) continue
        kept.add(fraction)
    }
    return kept
}

private fun traceSeries(ys: List<Double>, plot: Rect, domain: Domain, span: Double, path: Path) {
    if (ys.isEmpty()) return
    val denom = max(ys.size - 1, 1)
    ys.forEachIndexed { index, value ->
        val x = plot.left + plot.width * index.toFloat() / denom.toFloat()
        val y = plot.top + plot.height * ((domain.max - value) / span).toFloat()
        if (index == 0) path.moveTo(x, y) else path.lineTo(x, y)
    }
}

private fun psiLabel(value: Double): String = String.format(Locale.US, "%.1f", value)

private fun epochMsFor(sample: LogSample, anchor: Status?): Long? {
    val a = anchor ?: return null
    if (a.epochMs <= 0 || a.uptimeMs <= 0) return null
    return a.epochMs - a.uptimeMs + sample.tMs
}

private fun zoneFor(anchor: Status?): ZoneId =
    anchor?.timezoneOffsetMinutes?.let { ZoneOffset.ofTotalSeconds(it * 60) } ?: ZoneId.systemDefault()

private val TIME_FMT: DateTimeFormatter = DateTimeFormatter.ofPattern("HH:mm:ss")

private fun timeTextFor(samples: List<LogSample>, index: Int, anchor: Status?): String {
    if (index < 0 || index >= samples.size) return ""
    val sample = samples[index]
    val epoch = epochMsFor(sample, anchor)
    if (epoch != null) return Instant.ofEpochMilli(epoch).atZone(zoneFor(anchor)).format(TIME_FMT)
    return relativeTime(sample.tMs, samples.lastOrNull()?.tMs)
}

/** Compact axis tick ("HH:mm") so five labels fit across a narrow plot. */
private fun axisTimeFor(samples: List<LogSample>, index: Int, anchor: Status?): String {
    if (index < 0 || index >= samples.size) return ""
    val sample = samples[index]
    val epoch = epochMsFor(sample, anchor)
    if (epoch != null) {
        val full = Instant.ofEpochMilli(epoch).atZone(zoneFor(anchor)).format(TIME_FMT)
        return full.take(5)
    }
    return relativeTime(sample.tMs, samples.lastOrNull()?.tMs)
}

private fun relativeTime(tMs: Long?, newestMs: Long?): String {
    if (tMs == null || newestMs == null) return ""
    val elapsed = newestMs - tMs
    if (elapsed < 0) return "now"
    if (elapsed < 2_000) return "now"
    val seconds = elapsed / 1000
    return "-${seconds / 60}:${String.format(Locale.US, "%02d", seconds % 60)}"
}

@Composable
private fun EmptyLogs(error: String?, onLoad: () -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 32.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Icon(
            Icons.Filled.Inbox,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(36.dp),
        )
        Text(
            text = "No log samples yet",
            style = BoostFootnote,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (error != null) {
            Text(
                text = error,
                style = BoostFootnote,
                color = BoostColors.warning,
            )
        }
        Button(onClick = onLoad) {
            Text("Load logs")
        }
    }
}
