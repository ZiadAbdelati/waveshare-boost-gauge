package dev.boostgauge

import android.content.Intent
import android.net.Uri
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Checkbox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import dev.boostgauge.api.GaugeState
import dev.boostgauge.api.GaugeTheme
import dev.boostgauge.api.LogSample
import kotlin.math.max
import kotlin.math.min

class MainActivity : ComponentActivity() {
    private val viewModel: BoostGaugeViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            val state by viewModel.uiState.collectAsStateWithLifecycle()
            BoostGaugeApp(state = state, viewModel = viewModel)
        }
    }
}

@Composable
private fun BoostGaugeApp(state: BoostGaugeUiState, viewModel: BoostGaugeViewModel) {
    val context = LocalContext.current
    MaterialTheme {
        Surface(
            color = Asphalt,
            contentColor = Ice,
            modifier = Modifier.fillMaxSize()
        ) {
            Column(
                modifier = Modifier
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(14.dp)
            ) {
                Text(
                    text = "Boost Gauge",
                    fontSize = 30.sp,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 0.sp
                )
                ConnectionPanel(state, viewModel)
                GaugePanel(state.state, state.liveMode)
                StatusPanel(state)
                ConfigPanel(state, viewModel)
                ThemePanel(state.themes, state.activeThemeId, viewModel::selectTheme)
                LogsPanel(state.logs, viewModel)
                Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
                    Button(onClick = {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(viewModel.dashboardUrl())))
                    }) {
                        Text("Open web dashboard")
                    }
                    OutlinedButton(onClick = {
                        context.startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(viewModel.logsCsvUrl())))
                    }) {
                        Text("Export CSV")
                    }
                }
                if (state.error.isNotBlank()) {
                    Text(text = state.error, color = Flare, fontSize = 13.sp)
                }
            }
        }
    }
}

@Composable
private fun ConnectionPanel(state: BoostGaugeUiState, viewModel: BoostGaugeViewModel) {
    Section {
        OutlinedTextField(
            value = state.baseUrlInput,
            onValueChange = viewModel::setBaseUrl,
            label = { Text("Device URL") },
            singleLine = true,
            modifier = Modifier.fillMaxWidth()
        )
        Row(
            horizontalArrangement = Arrangement.spacedBy(10.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Button(onClick = viewModel::connect, enabled = !state.busy) {
                Text("Connect")
            }
            OutlinedButton(onClick = { viewModel.setBaseUrl("http://boostgauge.local") }) {
                Text("Use boostgauge.local")
            }
            if (state.busy) CircularProgressIndicator(modifier = Modifier.size(22.dp), strokeWidth = 2.dp)
        }
        Text(
            text = if (state.connected) "Connected to ${state.connectedBaseUrl}" else "Manual Wi-Fi URL required",
            color = if (state.connected) Vacuum else Muted,
            fontSize = 13.sp
        )
    }
}

@Composable
private fun GaugePanel(state: GaugeState, liveMode: String) {
    Section {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(1f),
            contentAlignment = Alignment.Center
        ) {
            GaugeCanvas(state)
            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                Text(
                    text = "%+.1f".format(state.psi),
                    fontSize = 56.sp,
                    fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold,
                    letterSpacing = 0.sp
                )
                Text(text = "PSI", color = Muted, fontSize = 14.sp, letterSpacing = 0.sp)
                Text(text = "peak %+.1f".format(state.peakPsi), color = Boost, fontSize = 15.sp)
            }
        }
        Row(
            horizontalArrangement = Arrangement.SpaceBetween,
            modifier = Modifier.fillMaxWidth()
        ) {
            Text(text = state.zone.uppercase(), color = zoneColor(state.zone), fontWeight = FontWeight.Bold)
            Text(text = liveMode.uppercase(), color = Muted)
        }
    }
}

@Composable
private fun GaugeCanvas(state: GaugeState) {
    Canvas(modifier = Modifier.fillMaxSize()) {
        val stroke = Stroke(width = 22.dp.toPx(), cap = StrokeCap.Round)
        val inset = 26.dp.toPx()
        val arcSize = Size(size.width - inset * 2, size.height - inset * 2)
        val topLeft = Offset(inset, inset)
        drawArc(Graphite, 140f, 260f, false, topLeft, arcSize, style = stroke)
        drawArc(Vacuum, 140f, 118f, false, topLeft, arcSize, style = stroke)
        drawArc(Boost, 258f, 112f, false, topLeft, arcSize, style = stroke)
        drawArc(Flare, 370f, 30f, false, topLeft, arcSize, style = stroke)

        val normalized = ((state.psi + 30.0) / 60.0).coerceIn(0.0, 1.0).toFloat()
        val sweep = 260f * normalized
        drawArc(Ice, 140f, sweep, false, topLeft, arcSize, style = Stroke(8.dp.toPx(), cap = StrokeCap.Round))
    }
}

@Composable
private fun StatusPanel(state: BoostGaugeUiState) {
    Section {
        Text("Status", fontWeight = FontWeight.Bold, fontSize = 20.sp)
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Stat("Firmware", state.state.firmwareVersion.ifBlank { "unknown" })
            Stat("Uptime", state.state.uptimeMs.uptimeLabel())
        }
        Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
            Stat("Brightness", state.state.brightness.toString())
            Stat("Clock", state.state.epochMs.wallClockLabel())
        }
    }
}

@Composable
private fun ConfigPanel(state: BoostGaugeUiState, viewModel: BoostGaugeViewModel) {
    Section {
        Text("Config", fontWeight = FontWeight.Bold, fontSize = 20.sp)
        Text("High ${state.config.brightnessHigh}", color = Muted)
        Slider(
            value = state.config.brightnessHigh.toFloat(),
            onValueChange = { viewModel.setBrightnessHigh(it.toInt()) },
            valueRange = 0f..100f
        )
        Text("Low ${state.config.brightnessLow}", color = Muted)
        Slider(
            value = state.config.brightnessLow.toFloat(),
            onValueChange = { viewModel.setBrightnessLow(it.toInt()) },
            valueRange = 0f..100f
        )
        Row(verticalAlignment = Alignment.CenterVertically) {
            Checkbox(
                checked = state.config.dimSchedule.enabled,
                onCheckedChange = viewModel::setScheduleEnabled
            )
            Text("Dim schedule")
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            TimeField(
                label = "Start",
                minutes = state.config.dimSchedule.startMinutes,
                onChanged = viewModel::setScheduleStart,
                modifier = Modifier.weight(1f)
            )
            TimeField(
                label = "End",
                minutes = state.config.dimSchedule.endMinutes,
                onChanged = viewModel::setScheduleEnd,
                modifier = Modifier.weight(1f)
            )
        }
        Row(horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            Button(onClick = viewModel::saveConfig, enabled = state.connected) {
                Text("Save config")
            }
            OutlinedButton(onClick = viewModel::syncDeviceTime, enabled = state.connected) {
                Text("Sync time")
            }
        }
    }
}

@Composable
private fun TimeField(label: String, minutes: Int, onChanged: (String) -> Unit, modifier: Modifier = Modifier) {
    OutlinedTextField(
        value = minutesLabel(minutes),
        onValueChange = onChanged,
        label = { Text(label) },
        singleLine = true,
        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
        modifier = modifier
    )
}

@Composable
private fun ThemePanel(themes: List<GaugeTheme>, activeThemeId: String, onTheme: (String) -> Unit) {
    Section {
        Text("Themes", fontWeight = FontWeight.Bold, fontSize = 20.sp)
        if (themes.isEmpty()) {
            Text("No catalogue loaded", color = Muted)
        } else {
            themes.forEach { theme ->
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .clickable { onTheme(theme.id) }
                        .padding(vertical = 6.dp),
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Swatch(theme.colors.vacuum)
                    Spacer(Modifier.width(6.dp))
                    Swatch(theme.colors.boost)
                    Spacer(Modifier.width(10.dp))
                    Text(theme.name, modifier = Modifier.weight(1f))
                    if (theme.id == activeThemeId) Text("ACTIVE", color = Vacuum, fontSize = 12.sp)
                }
            }
        }
    }
}

@Composable
private fun LogsPanel(logs: List<LogSample>, viewModel: BoostGaugeViewModel) {
    Section {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("Logs", fontWeight = FontWeight.Bold, fontSize = 20.sp)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick = viewModel::refreshLogs) { Text("Refresh") }
                OutlinedButton(onClick = viewModel::clearLogs) { Text("Clear") }
            }
        }
        LogChart(logs)
        LazyColumn(
            modifier = Modifier
                .fillMaxWidth()
                .height(180.dp),
            verticalArrangement = Arrangement.spacedBy(4.dp)
        ) {
            items(logs.takeLast(40).reversed()) { sample ->
                Row(modifier = Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween) {
                    Text("%+.1f psi".format(sample.psi), fontFamily = FontFamily.Monospace)
                    Text(sample.zone.ifBlank { sample.uptimeMs.uptimeLabel() }, color = Muted)
                }
            }
        }
    }
}

@Composable
private fun LogChart(logs: List<LogSample>) {
    Canvas(
        modifier = Modifier
            .fillMaxWidth()
            .height(96.dp)
            .background(Graphite, RoundedCornerShape(6.dp))
            .padding(8.dp)
    ) {
        if (logs.size < 2) return@Canvas
        val visible = logs.takeLast(120)
        val minPsi = min(-30.0, visible.minOf { it.psi })
        val maxPsi = max(30.0, visible.maxOf { it.psi })
        val range = (maxPsi - minPsi).coerceAtLeast(1.0)
        val stepX = size.width / (visible.size - 1).coerceAtLeast(1)
        for (index in 0 until visible.lastIndex) {
            val a = visible[index]
            val b = visible[index + 1]
            val start = Offset(index * stepX, yFor(a.psi, minPsi, range, size.height))
            val end = Offset((index + 1) * stepX, yFor(b.psi, minPsi, range, size.height))
            drawLine(if (b.psi >= 0.0) Boost else Vacuum, start, end, strokeWidth = 3.dp.toPx())
        }
    }
}

private fun yFor(psi: Double, minPsi: Double, range: Double, height: Float): Float {
    val normalized = ((psi - minPsi) / range).coerceIn(0.0, 1.0)
    return (height - height * normalized).toFloat()
}

@Composable
private fun Section(content: @Composable ColumnScope.() -> Unit) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .border(1.dp, Graphite, RoundedCornerShape(8.dp))
            .padding(12.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp),
        content = content
    )
}

@Composable
private fun Stat(label: String, value: String) {
    Column(modifier = Modifier.width(160.dp)) {
        Text(label, color = Muted, fontSize = 12.sp)
        Text(value, fontSize = 15.sp, maxLines = 1)
    }
}

@Composable
private fun Swatch(hex: String) {
    Box(
        modifier = Modifier
            .size(22.dp)
            .background(parseColor(hex), CircleShape)
            .border(1.dp, Ice.copy(alpha = 0.25f), CircleShape)
    )
}

private fun zoneColor(zone: String): Color = when (zone.lowercase()) {
    "boost" -> Boost
    "overboost" -> Flare
    else -> Vacuum
}

private fun parseColor(hex: String): Color = runCatching {
    Color(android.graphics.Color.parseColor(hex))
}.getOrDefault(Muted)

private val Asphalt = Color(0xFF090A0D)
private val Graphite = Color(0xFF20242C)
private val Ice = Color(0xFFE8ECF2)
private val Muted = Color(0xFF8C94A3)
private val Vacuum = Color(0xFF2EE6C5)
private val Boost = Color(0xFFFFB020)
private val Flare = Color(0xFFFF3B30)
