package com.boostgauge.app.ui.screens

import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
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
            initializer { CalibrationViewModel(container.api, container.repository.connectionStatus) }
        },
    )
    val state by viewModel.state.collectAsState()
    val focusManager = LocalFocusManager.current

    Box(modifier = Modifier.fillMaxSize()) {
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .pointerInput(Unit) { detectTapGestures { focusManager.clearFocus() } },
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
                    item { LiveSensorsSection(calibration.live) }
                    item {
                        SavedCalibrationSection(
                            cal = calibration.calibration,
                            savingSupply = state.savingSupply,
                            onSaveSupply = viewModel::setSupplyVolts,
                        )
                    }
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
                                Text(if (state.calibrating) "Calibrating…" else "Calibrate to Atmosphere")
                            }
                            Text(
                                text = "Takes about 2 seconds while the device observes the atmosphere.",
                                style = BoostFootnote,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                modifier = Modifier.padding(top = 4.dp, bottom = 4.dp),
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

        // Native M3 snackbar: floats at the bottom, auto-expires, never inserts
        // rows (same pattern as SettingsScreen).
        val snackbarHostState = remember { SnackbarHostState() }
        val toastText = state.toast
        LaunchedEffect(toastText) {
            if (toastText != null) {
                snackbarHostState.showSnackbar(toastText, withDismissAction = false)
                viewModel.clearToast()
            }
        }
        SnackbarHost(
            hostState = snackbarHostState,
            modifier = Modifier.align(Alignment.BottomCenter),
        )
    }
}

@Composable
private fun LiveSensorsSection(live: CalibrationLive) {
    GroupedSection(title = "Live sensors") {
        PresenceRow("ADS1115", present = live.adsPresent)
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        PresenceRow("BMP280", present = live.bmpPresent)
        if (live.fault) {
            HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
            Text(
                text = "Sensor fault",
                style = BoostFootnote,
                color = MaterialTheme.colorScheme.error,
                modifier = Modifier.padding(vertical = 12.dp),
            )
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("MAP volts", Format.fmt(live.mapVolts, 4))
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("Nominal kPa", Format.fmt(live.nominalKpa, 2))
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("Corrected kPa", Format.fmt(live.correctedKpa, 2))
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("BMP kPa", Format.fmt(live.bmpKpa, 2))
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("MAP age", ageText(live.mapAgeMs))
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        MetricRow("BMP age", ageText(live.bmpAgeMs))
        if (live.bmpUpdates != null) {
            HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
            MetricRow("BMP updates", live.bmpUpdates.toString())
        }
        if (live.ambientIsFallback) {
            HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
            MetricRow("Ambient", "fallback", valueColor = BoostColors.warning)
        }
    }
}

@Composable
private fun SavedCalibrationSection(
    cal: com.boostgauge.app.data.api.CalibrationValues,
    savingSupply: Boolean,
    onSaveSupply: (Double) -> Unit,
) {
    GroupedSection(title = "Saved calibration") {
        if (!cal.valid) {
            Text(
                text = "Not calibrated",
                style = BoostMetric,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 12.dp),
            )
        } else {
            MetricRow("Offset", "${Format.fmt(cal.offsetPsi, 2)} psi")
            if (cal.offsetKpa != null) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
                MetricRow("Offset kPa", Format.fmt(cal.offsetKpa, 2))
            }
            if (cal.version != null) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
                MetricRow("Version", cal.version.toString())
            }
            if (cal.refMapVolts != null) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
                MetricRow("Ref MAP volts", Format.fmt(cal.refMapVolts, 4))
            }
            if (cal.samples != null) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
                MetricRow("Samples", cal.samples.toString())
            }
            if (cal.epochMs > 0L) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
                MetricRow("Calibrated", CALIBRATED_FORMAT.format(Instant.ofEpochMilli(cal.epochMs).atZone(ZoneId.systemDefault())))
            }
        }
    }
    GroupedSection(title = "MAP supply voltage") {
        val focusManager = LocalFocusManager.current
        var supplyText by remember(cal.supplyVolts) { mutableStateOf(Format.fmt(cal.supplyVolts, 4)) }
        OutlinedTextField(
            value = supplyText,
            onValueChange = { supplyText = it },
            label = { Text("Supply (V)") },
            singleLine = true,
            keyboardOptions = KeyboardOptions(
                keyboardType = KeyboardType.Decimal,
                imeAction = ImeAction.Done,
            ),
            keyboardActions = KeyboardActions(onDone = { focusManager.clearFocus() }),
            modifier = Modifier.fillMaxWidth(),
        )
        Button(
            onClick = { supplyText.toDoubleOrNull()?.let { onSaveSupply(it) } },
            enabled = !savingSupply,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text(if (savingSupply) "Saving…" else "Save supply voltage")
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
