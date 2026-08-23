package com.boostgauge.app.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.CalibrationLive
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.Format
import com.boostgauge.app.ui.components.GroupedSection
import com.boostgauge.app.ui.components.MetricRow
import com.boostgauge.app.ui.viewmodels.CalibrationViewModel
import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter

@Composable
fun CalibrationScreen(container: AppContainer) {
    val viewModel: CalibrationViewModel = viewModel(
        factory = viewModelFactory {
            initializer { CalibrationViewModel(container.api) }
        },
    )
    val state by viewModel.state.collectAsState()

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "Calibration",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                IconButton(onClick = { viewModel.load() }) {
                    Icon(
                        Icons.Filled.Refresh,
                        contentDescription = "Refresh calibration",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        state.message?.let {
            item {
                Text(
                    text = it,
                    style = BoostFootnote,
                    color = BoostColors.success,
                )
            }
        }
        when (state.mode()) {
            CalibrationViewModel.UiMode.LOADING -> item {
                Box(
                    modifier = Modifier.fillMaxWidth().padding(40.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator()
                }
            }
            CalibrationViewModel.UiMode.CONTENT -> {
                val calibration = checkNotNull(state.calibration)
                state.error?.let {
                    item {
                        Text(
                            text = it,
                            style = BoostFootnote,
                            color = BoostColors.warning,
                        )
                    }
                }
                item { LiveSensorsSection(calibration.live) }
                item { SavedCalibrationSection(calibration.calibration) }
                item {
                    GroupedSection {
                        Button(
                            onClick = { viewModel.calibrate() },
                            enabled = !state.calibrating,
                            modifier = Modifier.fillMaxWidth(),
                        ) {
                            if (state.calibrating) {
                                CircularProgressIndicator(
                                    modifier = Modifier.padding(end = 8.dp).size(16.dp),
                                    strokeWidth = 2.dp,
                                )
                            }
                            Text(if (state.calibrating) "Calibrating…" else "Calibrate to atmosphere")
                        }
                        Text(
                            text = "Takes about 2 seconds while the device observes the atmosphere.",
                            style = BoostFootnote,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            CalibrationViewModel.UiMode.ERROR -> item {
                GroupedSection {
                    Text(
                        text = state.error ?: "Failed to load calibration",
                        style = BoostFootnote,
                        color = BoostColors.warning,
                    )
                    Text(
                        text = "Check the transport and try again.",
                        style = BoostFootnote,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Button(
                        onClick = { viewModel.load() },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text("Retry")
                    }
                }
            }
            CalibrationViewModel.UiMode.EMPTY -> item {
                GroupedSection {
                    Text(
                        text = "No calibration data loaded.",
                        style = BoostFootnote,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Button(
                        onClick = { viewModel.load() },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text("Refresh")
                    }
                }
            }
        }
    }
}

@Composable
private fun LiveSensorsSection(live: CalibrationLive) {
    GroupedSection(title = "Live sensors") {
        PresenceRow("ADS1115", present = live.adsPresent)
        PresenceRow("BMP280", present = live.bmpPresent)
        if (live.fault) {
            Text(
                text = "Sensor fault",
                style = BoostFootnote,
                color = MaterialTheme.colorScheme.error,
            )
        }
        MetricRow("MAP volts", Format.fmt(live.mapVolts, 4))
        MetricRow("Nominal kPa", Format.fmt(live.nominalKpa, 2))
        MetricRow("Corrected kPa", Format.fmt(live.correctedKpa, 2))
        MetricRow("BMP kPa", Format.fmt(live.bmpKpa, 2))
        MetricRow("MAP age", ageText(live.mapAgeMs))
        MetricRow("BMP age", ageText(live.bmpAgeMs))
        MetricRow("Ambient", if (live.ambientIsFallback) "fallback" else "live", valueColor = if (live.ambientIsFallback) BoostColors.warning else MaterialTheme.colorScheme.onSurface)
    }
}

@Composable
private fun SavedCalibrationSection(cal: com.boostgauge.app.data.api.CalibrationValues) {
    GroupedSection(title = "Saved calibration") {
        if (!cal.valid) {
            Text(
                text = "Not calibrated",
                style = BoostMetric,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        } else {
            MetricRow("Valid", "yes")
            MetricRow("Version", cal.version.toString())
            MetricRow("Offset", "${Format.fmt(cal.offsetPsi, 2)} psi / ${Format.fmt(cal.offsetKpa, 2)} kPa")
            MetricRow("Supply", "${Format.fmt(cal.supplyVolts, 2)} V")
            MetricRow("Ref MAP volts", Format.fmt(cal.refMapVolts, 4))
            MetricRow("Samples", cal.samples.toString())
            if (cal.epochMs > 0L) {
                MetricRow("Calibrated", CALIBRATED_FORMAT.format(Instant.ofEpochMilli(cal.epochMs).atZone(ZoneId.systemDefault())))
            }
        }
    }
}

@Composable
private fun PresenceRow(label: String, present: Boolean) {
    MetricRow(
        label = label,
        value = if (present) "present" else "absent",
        valueColor = if (present) BoostColors.success else MaterialTheme.colorScheme.error,
        valueWeight = androidx.compose.ui.text.font.FontWeight.SemiBold,
    )
}

private fun ageText(ageMs: Long): String =
    if (ageMs < 0) "never read" else Format.formatUptime(ageMs)

private val CALIBRATED_FORMAT: DateTimeFormatter = DateTimeFormatter.ofPattern("yyyy-MM-dd HH:mm")
