package com.boostgauge.app.ui.screens

import android.Manifest
import android.app.Application
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
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
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.data.transport.BleScanResult
import com.boostgauge.app.data.transport.BleScanner
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.components.GroupedSection
import com.boostgauge.app.ui.viewmodels.SettingsViewModel

@Composable
fun SettingsScreen(container: AppContainer) {
    val app = LocalContext.current.applicationContext as Application
    val viewModel: SettingsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                SettingsViewModel(
                    api = container.api,
                    selection = container.transportController.selection,
                    selectTransport = { type, address -> container.transportController.select(type, address) },
                    repository = container.repository,
                    scanDevices = { BleScanner(app).scan() },
                )
            }
        },
    )
    val state by viewModel.state.collectAsState()
    val selection by viewModel.transportSelection.collectAsState()
    val connected by viewModel.connected.collectAsState()
    val fields = state.fields

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
                    text = "Settings",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                IconButton(onClick = { viewModel.refreshAll() }) {
                    Icon(
                        Icons.Filled.Refresh,
                        contentDescription = "Refresh settings",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
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
        state.message?.let {
            item {
                Text(
                    text = it,
                    style = BoostFootnote,
                    color = BoostColors.success,
                )
            }
        }
        item {
            TransportSection(
                selection = selection,
                connected = connected,
                scanning = state.scanning,
                scannedDevices = state.scannedDevices,
                httpAddress = fields.httpAddress,
                onHttpAddressChange = { address -> viewModel.updateFields { it.copy(httpAddress = address) } },
                onHttpSelected = viewModel::saveHttpAddress,
                onBleSelected = viewModel::connectBle,
                onScan = viewModel::scanForDevices,
            )
        }
        item {
            ConfigSection(
                fields = fields,
                saving = state.saving,
                onFieldChange = viewModel::updateFields,
                onSave = viewModel::saveConfig,
            )
        }
        item {
            ThemeFlagsSection(
                fields = fields,
                saving = state.saving,
                onFieldChange = viewModel::updateFields,
                onSave = viewModel::saveThemeFlags,
            )
        }
        item {
            TpmsSection(
                fields = fields,
                saving = state.saving,
                onFieldChange = viewModel::updateFields,
                onSave = viewModel::saveTpms,
            )
        }
        item {
            ClockSection(
                saving = state.saving,
                onSync = viewModel::syncTime,
            )
        }
    }
}

@Composable
private fun TransportSection(
    selection: TransportSelection,
    connected: Boolean,
    scanning: Boolean,
    scannedDevices: List<BleScanResult>,
    httpAddress: String,
    onHttpAddressChange: (String) -> Unit,
    onHttpSelected: () -> Unit,
    onBleSelected: (String) -> Unit,
    onScan: () -> Unit,
) {
    GroupedSection(title = "Transport") {
        val statusColor = when {
            connected -> BoostColors.success
            else -> MaterialTheme.colorScheme.error
        }
        Text(
            text = if (connected) "Live · ${selection.type.name}" else "Disconnected",
            style = BoostCaptionSemibold,
            color = statusColor,
            modifier = Modifier.padding(vertical = 4.dp),
        )
        SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
            SegmentedButton(
                selected = selection.type == TransportType.HTTP,
                onClick = {
                    if (selection.type != TransportType.HTTP) {
                        onHttpSelected()
                    }
                },
                shape = SegmentedButtonDefaults.itemShape(index = 0, count = 2),
            ) {
                Text("HTTP (Wi-Fi)")
            }
            SegmentedButton(
                selected = selection.type == TransportType.BLE,
                onClick = { /* BLE panel below */ },
                shape = SegmentedButtonDefaults.itemShape(index = 1, count = 2),
            ) {
                Text("BLE")
            }
        }
        when (selection.type) {
            TransportType.HTTP -> {
                OutlinedTextField(
                    value = httpAddress,
                    onValueChange = onHttpAddressChange,
                    label = { Text("Gauge IP / host") },
                    singleLine = true,
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(top = 4.dp),
                )
                Button(
                    onClick = onHttpSelected,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text("Save & connect HTTP")
                }
            }
            TransportType.BLE -> {
                val context = LocalContext.current
                val permissionLauncher = rememberLauncherForActivityResult(
                    ActivityResultContracts.RequestMultiplePermissions(),
                ) { grants ->
                    if (grants.values.all { it }) onScan() else Unit
                }
                Button(
                    onClick = {
                        val missing = listOf(
                            Manifest.permission.BLUETOOTH_SCAN,
                            Manifest.permission.BLUETOOTH_CONNECT,
                        ).filter {
                            ContextCompat.checkSelfPermission(context, it) != PackageManager.PERMISSION_GRANTED
                        }
                        if (missing.isEmpty()) onScan() else permissionLauncher.launch(missing.toTypedArray())
                    },
                    enabled = !scanning,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    if (scanning) {
                        CircularProgressIndicator(modifier = Modifier.padding(end = 8.dp), strokeWidth = 2.dp)
                    }
                    Text(if (scanning) "Scanning…" else "Scan for BoostGauge")
                }
                if (scannedDevices.isEmpty() && !scanning) {
                    Text(
                        text = "No gauge found. Make sure the gauge is advertising.",
                        style = BoostFootnote,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.padding(vertical = 2.dp),
                    )
                }
                scannedDevices.forEach { device ->
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Column {
                            Text(device.name, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
                            Text(
                                device.address,
                                style = BoostCaption,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        TextButton(onClick = { onBleSelected(device.address) }) {
                            Text("Connect")
                        }
                    }
                }
            }
        }
    }
}

@Composable
private fun ConfigSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    GroupedSection(title = "Display & range") {
        NumberField("Brightness high (%)", fields.brightnessHigh) { value ->
            onFieldChange { it.copy(brightnessHigh = value) }
        }
        NumberField("Brightness low (%)", fields.brightnessLow) { value ->
            onFieldChange { it.copy(brightnessLow = value) }
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Dim schedule", style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
            Switch(checked = fields.dimEnabled, onCheckedChange = { enabled ->
                onFieldChange { it.copy(dimEnabled = enabled) }
            })
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("Start (min of day)", fields.dimStart, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(dimStart = value) }
            }
            NumberField("End (min of day)", fields.dimEnd, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(dimEnd = value) }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("psiMin", fields.psiMin, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(psiMin = value) }
            }
            NumberField("psiMax", fields.psiMax, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(psiMax = value) }
            }
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            NumberField("psiOverboost", fields.psiOverboost, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(psiOverboost = value) }
            }
            NumberField("zeroAngle °", fields.zeroAngle, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(zeroAngle = value) }
            }
        }
        ToggleRow("Companion BLE advertising (appBle)", fields.appBle) { value ->
            onFieldChange { it.copy(appBle = value) }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save config")
        }
    }
}

@Composable
private fun ThemeFlagsSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    GroupedSection(title = "Theme & demo") {
        ToggleRow("Demo mode", fields.demoMode) { value ->
            onFieldChange { it.copy(demoMode = value) }
        }
        ToggleRow("Demo fast sweep (9.789 psi/s)", fields.demoFastSweep) { value ->
            onFieldChange { it.copy(demoFastSweep = value) }
        }
        ToggleRow("TPMS BLE link (gauge OBD2 central)", fields.tpmsBle) { value ->
            onFieldChange { it.copy(tpmsBle = value) }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save flags")
        }
    }
}

@Composable
private fun TpmsSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    GroupedSection(title = "TPMS") {
        NumberField("Low pressure (psi)", fields.lowPsi) { value ->
            onFieldChange { it.copy(lowPsi = value) }
        }
        NumberField("Stale after (ms)", fields.staleAfterMs) { value ->
            onFieldChange { it.copy(staleAfterMs = value) }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save TPMS config")
        }
    }
}

@Composable
private fun ClockSection(saving: Boolean, onSync: () -> Unit) {
    GroupedSection(title = "Clock") {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Sync device clock to phone",
                style = BoostMetric,
                color = MaterialTheme.colorScheme.onSurface,
            )
            Button(onClick = onSync, enabled = !saving) {
                Text("Sync now")
            }
        }
        Text(
            text = "Sends the phone epoch and timezone to the device.",
            style = BoostFootnote,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        if (saving) {
            CircularProgressIndicator(modifier = Modifier.size(18.dp), strokeWidth = 2.dp)
        }
    }
}

@Composable
private fun NumberField(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    onValueChange: (String) -> Unit,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        label = { Text(label) },
        singleLine = true,
        modifier = modifier.fillMaxWidth(),
    )
}

@Composable
private fun ToggleRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}
