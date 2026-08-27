package com.boostgauge.app.ui.screens

import android.Manifest
import android.app.Application
import android.content.pm.PackageManager
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.ArrowDropDown
import androidx.compose.material.icons.filled.Build
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Schedule
import androidx.compose.material.icons.filled.SwapHoriz
import androidx.compose.material.icons.filled.TireRepair
import androidx.compose.material.icons.filled.Tv
import androidx.compose.material.icons.filled.Wifi
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.ui.graphics.Color
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.api.Obd
import com.boostgauge.app.data.settings.TransportSelection
import com.boostgauge.app.data.transport.BleScanResult
import com.boostgauge.app.data.transport.BleScanner
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostMetricValue
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.BoostSubheadline
import com.boostgauge.app.ui.Timezones
import com.boostgauge.app.ui.components.CaptionText
import com.boostgauge.app.ui.components.GroupedSection
import com.boostgauge.app.ui.components.MetricRow
import com.boostgauge.app.ui.components.Pill
import com.boostgauge.app.ui.displayLabel
import com.boostgauge.app.ui.viewmodels.SettingsViewModel

internal enum class SettingsPage(val title: String) {
    Connection("Connection"),
    Display("Display"),
    Range("Range"),
    DemoMode("Demo mode"),
    ClockTimezone("Clock & Timezone"),
    Tpms("TPMS"),
    ObdScanner("OBD2 Scanner"),
}

@Composable
fun SettingsScreen(container: AppContainer) {
    val app = LocalContext.current.applicationContext as Application
    val viewModel: SettingsViewModel = viewModel(
        factory = viewModelFactory {
            initializer {
                SettingsViewModel(
                    api = container.api,
                    selection = container.transportController.selection,
                    selectTransport = { type, address, name ->
                        container.transportController.select(type, address, name)
                    },
                    repository = container.repository,
                    scanDevices = { BleScanner(app).scan() },
                    disconnectTransport = { container.transportController.disconnect() },
                )
            }
        },
    )
    val state by viewModel.state.collectAsState()
    val selection by viewModel.transportSelection.collectAsState()
    val connectionStatus by viewModel.connectionStatus.collectAsState()
    val status by viewModel.status.collectAsState()
    val fields = state.fields
    LaunchedEffect(Unit) { viewModel.refreshAll() }
    LaunchedEffect(connectionStatus) {
        if (connectionStatus == ConnectionStatus.Connected) viewModel.refreshAll()
    }
    var page by rememberSaveable {
        mutableStateOf<SettingsPage?>(
            com.boostgauge.app.MainActivity.debugInitialSettingsPage?.let { name ->
                val aliased = if (name == "ThemeDemo" || name == "Theme & demo") "DemoMode" else name
                SettingsPage.entries.firstOrNull { it.name == aliased }
                    ?: SettingsPage.entries.firstOrNull { it.title == name }
                    ?: SettingsPage.entries.firstOrNull { it.title == aliased }
            },
        )
    }
    BackHandler(enabled = page != null) { page = null }
    // Success toasts ("Range saved") belong to the sub-page that raised them;
    // leaving the page clears them so they can't leak onto other pages.
    LaunchedEffect(page) { viewModel.clearMessage() }

    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (page != null) {
                    IconButton(onClick = { page = null }) {
                        Icon(
                            Icons.AutoMirrored.Filled.ArrowBack,
                            contentDescription = "Back",
                            tint = MaterialTheme.colorScheme.onBackground,
                        )
                    }
                }
                Text(
                    text = page?.title ?: "Settings",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                Spacer(modifier = Modifier.weight(1f))
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
        when (page) {
            null -> {
                item {
                    SettingsMenu(onNavigate = { page = it })
                }
                item {
                    AboutSection(status = status)
                }
            }
            SettingsPage.Connection -> item {
                val reconnectAttempt by viewModel.reconnectAttempt.collectAsState()
                TransportSection(
                    selection = selection,
                    connectionStatus = connectionStatus,
                    reconnectAttempt = reconnectAttempt,
                    scanning = state.scanning,
                    scanCompleted = state.scanCompleted,
                    scannedDevices = state.scannedDevices,
                    onBleSelected = viewModel::connectBle,
                    onScan = viewModel::scanForDevices,
                    onDisconnect = viewModel::disconnectBle,
                    onConnectSaved = viewModel::connectSavedGauge,
                )
            }
            SettingsPage.Display -> item {
                DisplaySection(
                    fields = fields,
                    saving = state.saving,
                    onFieldChange = viewModel::updateFields,
                    onSave = viewModel::saveDisplay,
                )
            }
            SettingsPage.Range -> item {
                RangeSection(
                    fields = fields,
                    saving = state.saving,
                    onFieldChange = viewModel::updateFields,
                    onSave = viewModel::saveRange,
                )
            }
            SettingsPage.DemoMode -> item {
                DemoModeSection(
                    fields = fields,
                    saving = state.saving,
                    onFieldChange = viewModel::updateFields,
                    onSave = viewModel::saveDemoMode,
                )
            }
            SettingsPage.ClockTimezone -> item {
                ClockTimezoneSection(
                    fields = fields,
                    saving = state.saving,
                    onFieldChange = viewModel::updateFields,
                    onApply = viewModel::applyTimezone,
                    onSync = viewModel::syncTime,
                )
            }
            SettingsPage.Tpms -> item {
                TpmsSection(
                    fields = fields,
                    saving = state.saving,
                    onFieldChange = viewModel::updateFields,
                    onSave = viewModel::saveTpms,
                    onBleSave = viewModel::saveTpmsBle,
                )
            }
            SettingsPage.ObdScanner -> item {
                ObdScannerSection(
                    obd = status?.obd,
                    saving = state.saving,
                    onForget = viewModel::forgetObdPeer,
                )
            }
        }
    }
}

@Composable
private fun SettingsMenu(onNavigate: (SettingsPage) -> Unit) {
    GroupedSection {
        SettingsPage.entries.forEachIndexed { index, entry ->
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .clickable { onNavigate(entry) }
                    .padding(vertical = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    Icon(
                        imageVector = when (entry) {
                            SettingsPage.Connection -> Icons.Default.Wifi
                            SettingsPage.Display -> Icons.Default.Tv
                            SettingsPage.Range -> Icons.Default.SwapHoriz
                            SettingsPage.DemoMode -> Icons.Default.Palette
                            SettingsPage.ClockTimezone -> Icons.Default.Schedule
                            SettingsPage.Tpms -> Icons.Default.TireRepair
                            SettingsPage.ObdScanner -> Icons.Default.Build
                        },
                        contentDescription = null,
                        tint = BoostColors.navBlue,
                        modifier = Modifier.size(22.dp),
                    )
                    Text(
                        text = entry.title,
                        style = BoostMetric,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                }
                Icon(
                    Icons.AutoMirrored.Filled.KeyboardArrowRight,
                    contentDescription = null,
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            if (index < SettingsPage.entries.lastIndex) {
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
            }
        }
    }
}

@Composable
private fun TransportSection(
    selection: TransportSelection,
    connectionStatus: ConnectionStatus,
    reconnectAttempt: Int?,
    scanning: Boolean,
    scanCompleted: Boolean,
    scannedDevices: List<BleScanResult>,
    onBleSelected: (BleScanResult) -> Unit,
    onScan: () -> Unit,
    onDisconnect: () -> Unit,
    onConnectSaved: () -> Unit,
) {
    GroupedSection {
        val peerKnown = selection.bleAddress.isNotBlank()
        val statusColor = if (connectionStatus == ConnectionStatus.Connected) {
            BoostColors.success
        } else {
            MaterialTheme.colorScheme.error
        }
        Text(
            text = connectionStatus.displayLabel(peerKnown, reconnectAttempt),
            style = BoostCaptionSemibold,
            color = statusColor,
            modifier = Modifier.padding(vertical = 4.dp),
        )
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
        // Saved gauge row: shown whenever a peer is remembered AND the link is
        // not connected (incl. auto-reconnect + fresh launch). Hidden entirely
        // while connected — a Connect action against a live link is meaningless.
        when (savedRowAction(connectionStatus, peerKnown)) {
            SavedRowAction.Connected -> {
                // Live identity while connected (mirrors iOS): the gauge this
                // session is talking to, no Connect action.
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column {
                        Text(selection.bleName.ifBlank { "BoostGauge" }, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
                        Text(
                            selection.bleAddress,
                            style = BoostCaption,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Text(
                        text = "Connected",
                        style = BoostFootnote,
                        color = BoostColors.success,
                    )
                }
            }
            SavedRowAction.Connect -> {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column {
                        Text(selection.bleName.ifBlank { "BoostGauge" }, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
                        Text(
                            selection.bleAddress,
                            style = BoostCaption,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    TextButton(onClick = onConnectSaved) {
                        Text("Connect")
                    }
                }
                CaptionText("Saved gauge — reconnect to ${selection.bleName.ifBlank { "this gauge" }}")
            }
            SavedRowAction.Reconnecting -> {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Column {
                        Text(selection.bleName.ifBlank { "BoostGauge" }, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
                        Text(
                            selection.bleAddress,
                            style = BoostCaption,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    // No Connect button while the loop is retrying — the pill
                    // above carries the "Reconnecting… (attempt N)" banner.
                    Text(
                        text = "Auto-reconnecting…",
                        style = BoostFootnote,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                CaptionText("Saved gauge — ${selection.bleName.ifBlank { "this gauge" }}")
            }
            null -> Unit
        }
        if (connectionStatus != ConnectionStatus.Disconnected) {
            TextButton(
                onClick = onDisconnect,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("Disconnect")
            }
        }
        // "No gauge found" is ONLY the empty result of a user-initiated scan —
        // never while a peer is remembered (it would contradict the saved row).
        if (scanCompleted && !scanning && scannedDevices.isEmpty() && !peerKnown) {
            Text(
                text = "No gauge found. Make sure the gauge is advertising.",
                style = BoostFootnote,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(vertical = 2.dp),
            )
        }
        // The saved gauge is already shown as the dedicated Saved row — don't
        // repeat it in the scan list (same hardware would appear twice).
        scannedDevices.filterNot { it.address.equals(selection.bleAddress, ignoreCase = true) }.forEach { device ->
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
                TextButton(onClick = { onBleSelected(device) }) {
                    Text("Connect")
                }
            }
        }
    }
}

/** Round-8 saved-row visibility rule (see PARITY.md): the row is visible
 *  whenever a peer is remembered AND the link is not connected, and is hidden
 *  entirely while connected. Returns the action area to render, or null. */
internal enum class SavedRowAction { Connected, Connect, Reconnecting }

internal fun savedRowAction(connectionStatus: ConnectionStatus, peerKnown: Boolean): SavedRowAction? = when {
    !peerKnown -> null
    connectionStatus == ConnectionStatus.Connected -> SavedRowAction.Connected
    connectionStatus == ConnectionStatus.Disconnected -> SavedRowAction.Connect
    else -> SavedRowAction.Reconnecting
}

@Composable
private fun DisplaySection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    androidx.compose.foundation.layout.Column(verticalArrangement = Arrangement.spacedBy(16.dp)) {
        GroupedSection(title = "Brightness") {
            StepperRow(
                label = "Brightness high",
                value = fields.brightnessHigh,
                suffix = "%",
                min = 1,
                max = 100,
                enabled = !saving,
            ) { value ->
                onFieldChange { it.copy(brightnessHigh = value.toString()) }
            }
            StepperRow(
                label = "Brightness low",
                value = fields.brightnessLow,
                suffix = "%",
                min = 1,
                max = 100,
                enabled = !saving,
            ) { value ->
                onFieldChange { it.copy(brightnessLow = value.toString()) }
            }
        }
        GroupedSection(title = "Dim schedule") {
            ToggleRow("Dim schedule", fields.dimEnabled, enabled = !saving) { enabled ->
                onFieldChange { it.copy(dimEnabled = enabled) }
            }
            if (fields.dimEnabled) {
                StepperRow(
                    label = "Start",
                    value = fields.dimStart,
                    display = { minutes -> "%02d:%02d".format(minutes / 60, minutes % 60) },
                    min = 0,
                    max = 24 * 60 - 1,
                    enabled = !saving,
                ) { value ->
                    onFieldChange { it.copy(dimStart = value.toString()) }
                }
                StepperRow(
                    label = "End",
                    value = fields.dimEnd,
                    display = { minutes -> "%02d:%02d".format(minutes / 60, minutes % 60) },
                    min = 0,
                    max = 24 * 60 - 1,
                    enabled = !saving,
                ) { value ->
                    onFieldChange { it.copy(dimEnd = value.toString()) }
                }
            }
        }
        GroupedSection(title = "Panel") {
            var rotationExpanded by remember { mutableStateOf(false) }
            androidx.compose.material3.Surface(
                onClick = { rotationExpanded = true },
                enabled = !saving,
                shape = RoundedCornerShape(10.dp),
                color = MaterialTheme.colorScheme.surfaceContainerHighest,
            ) {
                Row(
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 12.dp, vertical = 12.dp),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = "Rotation",
                        style = BoostMetric,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                    Row(
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        Text(
                            text = "${fields.rotation}°",
                            style = BoostMetricValue,
                            color = MaterialTheme.colorScheme.onSurface,
                        )
                        Icon(
                            Icons.Filled.ArrowDropDown,
                            contentDescription = null,
                            tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
            }
            DropdownMenu(expanded = rotationExpanded, onDismissRequest = { rotationExpanded = false }) {
                listOf(0, 90, 180, 270).forEach { value ->
                    DropdownMenuItem(
                        text = { Text("$value°") },
                        onClick = {
                            rotationExpanded = false
                            onFieldChange { it.copy(rotation = value) }
                        },
                    )
                }
            }
            ToggleRow("Region double-buffer", fields.regionDBuf, enabled = !saving) { value ->
                onFieldChange { it.copy(regionDBuf = value) }
            }
            ToggleRow("TE sync", fields.teSync, enabled = !saving) { value ->
                onFieldChange { it.copy(teSync = value) }
            }
            ToggleRow("TE scanline", fields.teScanline, enabled = !saving) { value ->
                onFieldChange { it.copy(teScanline = value) }
            }
            ToggleRow("Pixel shift", fields.pixelShift, enabled = !saving) { value ->
                onFieldChange { it.copy(pixelShift = value) }
            }
            if (fields.pixelShift) {
                StepperRow(
                    label = "Pixel shift interval",
                    value = fields.pixelShiftSec,
                    suffix = "s",
                    min = 30,
                    max = 3600,
                    step = 30,
                    enabled = !saving,
                ) { value ->
                    onFieldChange { it.copy(pixelShiftSec = value.toString()) }
                }
            }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save display settings")
        }
    }
}

@Composable
private fun RangeSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    GroupedSection {
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
            NumberField("zeroAngle", fields.zeroAngle, Modifier.weight(1f)) { value ->
                onFieldChange { it.copy(zeroAngle = value) }
            }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save")
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DemoModeSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
) {
    GroupedSection {
        ToggleRow("Demo mode", fields.demoMode, enabled = !saving) { value ->
            onFieldChange { it.copy(demoMode = value) }
        }
        var waveformExpanded by remember { mutableStateOf(false) }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Demo waveform", style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
            ExposedDropdownMenuBox(
                expanded = waveformExpanded,
                onExpandedChange = { waveformExpanded = it },
            ) {
                Surface(
                    onClick = { waveformExpanded = true },
                    enabled = !saving && fields.demoMode,
                    modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable),
                    shape = RoundedCornerShape(8.dp),
                    color = MaterialTheme.colorScheme.surfaceContainerHighest,
                ) {
                    Row(
                        modifier = Modifier.padding(horizontal = 12.dp, vertical = 6.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        Text(
                            text = if (fields.demoFastSweep) "Linear sweep (9.789 psi/s)" else "Organic swell",
                            style = BoostSubheadline,
                            color = if (fields.demoMode && !saving) {
                                MaterialTheme.colorScheme.primary
                            } else {
                                MaterialTheme.colorScheme.onSurfaceVariant
                            },
                        )
                        ExposedDropdownMenuDefaults.TrailingIcon(expanded = waveformExpanded)
                    }
                }
                ExposedDropdownMenu(
                    expanded = waveformExpanded,
                    onDismissRequest = { waveformExpanded = false },
                ) {
                    DropdownMenuItem(
                        text = { Text("Organic swell") },
                        onClick = {
                            waveformExpanded = false
                            onFieldChange { it.copy(demoFastSweep = false) }
                        },
                    )
                    DropdownMenuItem(
                        text = { Text("Linear sweep (9.789 psi/s)") },
                        onClick = {
                            waveformExpanded = false
                            onFieldChange { it.copy(demoFastSweep = true) }
                        },
                    )
                }
            }
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save demo settings")
        }
    }
}

@Composable
private fun ClockTimezoneSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onApply: (Int, String) -> Unit,
    onSync: () -> Unit,
) {
    GroupedSection {
        val currentPosix = fields.timezoneTz
        val matched = Timezones.curated.firstOrNull { it.posix == currentPosix }
        var customPicked by rememberSaveable { mutableStateOf(false) }
        val isCustom = customPicked || (matched == null && currentPosix.isNotEmpty())
        val selectedLabel = when {
            isCustom -> "Custom"
            matched != null -> matched.label
            else -> "UTC"
        }
        var expanded by remember { mutableStateOf(false) }
        Surface(
            onClick = { expanded = true },
            enabled = !saving,
            color = Color.Transparent,
        ) {
            Row(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(vertical = 12.dp),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = "Timezone",
                    style = BoostMetric,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Row(
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        text = selectedLabel,
                        style = BoostMetricValue,
                        color = MaterialTheme.colorScheme.onSurface,
                    )
                    Icon(
                        Icons.Filled.ArrowDropDown,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.08f))
        DropdownMenu(expanded = expanded, onDismissRequest = { expanded = false }) {
            Timezones.curated.forEach { entry ->
                DropdownMenuItem(
                    text = { Text(entry.label) },
                    onClick = {
                        expanded = false
                        customPicked = false
                        onApply(entry.offsetMinutes, entry.posix)
                    },
                )
            }
            DropdownMenuItem(
                text = { Text("Custom") },
                onClick = {
                    expanded = false
                    customPicked = true
                },
            )
        }
        if (isCustom) {
            OutlinedTextField(
                value = currentPosix,
                onValueChange = { value ->
                    onFieldChange { it.copy(timezoneTz = value) }
                },
                label = { Text("POSIX TZ string") },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            TextButton(
                onClick = { onApply(fields.timezoneOffsetMinutes, fields.timezoneTz) },
                enabled = !saving && fields.timezoneTz.isNotBlank(),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text("Apply custom timezone")
            }
        }
        Button(
            onClick = onSync,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (saving) {
                CircularProgressIndicator(
                    strokeWidth = 2.dp,
                    modifier = Modifier.size(18.dp),
                    color = MaterialTheme.colorScheme.onPrimary,
                )
            } else {
                Text(
                    text = "Sync timezone to gauge",
                    maxLines = 1,
                    softWrap = false,
                )
            }
        }
    }
}

@Composable
private fun TpmsSection(
    fields: SettingsViewModel.FieldState,
    saving: Boolean,
    onFieldChange: ((SettingsViewModel.FieldState) -> SettingsViewModel.FieldState) -> Unit,
    onSave: () -> Unit,
    onBleSave: () -> Unit,
) {
    GroupedSection {
        NumberField("Low pressure (psi)", fields.lowPsi) { value ->
            onFieldChange { it.copy(lowPsi = value) }
        }
        NumberField("Stale after (ms)", fields.staleAfterMs) { value ->
            onFieldChange { it.copy(staleAfterMs = value) }
        }
        ToggleRow("TPMS BLE link", fields.tpmsBle, enabled = !saving) { value ->
            onFieldChange { it.copy(tpmsBle = value) }
            onBleSave()
        }
        Button(
            onClick = onSave,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            Text("Save")
        }
    }
}

@Composable
private fun ObdScannerSection(
    obd: Obd?,
    saving: Boolean,
    onForget: () -> Unit,
) {
    GroupedSection {
        val pillColor = when (obd?.state ?: 0) {
            3 -> BoostColors.success
            1, 2 -> BoostColors.navBlue
            else -> if ((obd?.lastError ?: 0L) != 0L) BoostColors.warning else MaterialTheme.colorScheme.onSurfaceVariant
        }
        Pill(
            text = obdPillLabel(obd),
            containerColor = pillColor.copy(alpha = 0.16f),
            textColor = pillColor,
            modifier = Modifier.padding(vertical = 4.dp),
        )
        MetricRow(label = "Peer", value = obdPeerLine(obd))
        Button(
            onClick = onForget,
            enabled = !saving,
            modifier = Modifier.fillMaxWidth(),
        ) {
            if (saving) {
                CircularProgressIndicator(
                    strokeWidth = 2.dp,
                    modifier = Modifier.size(18.dp),
                    color = MaterialTheme.colorScheme.onPrimary,
                )
            } else {
                Text("Forget")
            }
        }
        CaptionText("Gauge → OBD2 dongle link. Gauge auto-scans when the TPMS BLE link is on.")
    }
}

/** Live OBD state pill text, from `/state.obd` (state 1 scanning, 2 connecting,
 *  3 ready; 0 disabled / 4 disconnected idle). */
internal fun obdPillLabel(obd: Obd?): String {
    val state = obd?.state ?: 0
    val label = when (state) {
        1 -> "Scanning"
        2 -> {
            val name = obd?.peer?.ifBlank { obd?.peerAddr } ?: ""
            "Connecting to ${name.ifBlank { "adapter" }}"
        }
        3 -> "Connected"
        else -> "Idle"
    }
    val error = obd?.lastError ?: 0L
    return if (label == "Idle" && error != 0L) "Idle · error $error" else label
}

/** Peer row value: name + address, em dash when none. */
internal fun obdPeerLine(obd: Obd?): String {
    if (obd == null) return "—"
    val name = obd.peer.ifBlank { "—" }
    val addr = obd.peerAddr.ifBlank { "—" }
    return if (name == "—" && addr == "—") "—" else "$name · $addr"
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
private fun StepperRow(
    label: String,
    value: String,
    suffix: String = "",
    min: Int,
    max: Int,
    step: Int = 1,
    enabled: Boolean = true,
    display: (Int) -> String = { it.toString() },
    onValueChange: (Int) -> Unit,
) {
    val current = value.toIntOrNull()?.coerceIn(min, max) ?: min
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        Row(verticalAlignment = Alignment.CenterVertically) {
            TextButton(enabled = enabled && current > min, onClick = { onValueChange((current - step).coerceAtLeast(min)) }) {
                Text("-")
            }
            Text("${display(current)}$suffix", style = BoostMetricValue, color = MaterialTheme.colorScheme.onSurface)
            TextButton(enabled = enabled && current < max, onClick = { onValueChange((current + step).coerceAtMost(max)) }) {
                Text("+")
            }
        }
    }
}

@Composable
private fun ToggleRow(
    label: String,
    checked: Boolean,
    enabled: Boolean = true,
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        Switch(checked = checked, enabled = enabled, onCheckedChange = onCheckedChange)
    }
}

@Composable
private fun AboutSection(status: com.boostgauge.app.data.api.Status?) {
    val context = LocalContext.current
    val appVersion = try {
        val info = context.packageManager.getPackageInfo(context.packageName, 0)
        "${info.versionName} (${info.longVersionCode})"
    } catch (_: Exception) {
        "0.9.1"
    }
    val firmware = status?.firmwareVersion?.takeIf { it.isNotBlank() } ?: "Not connected"
    GroupedSection(title = "About") {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("App", style = BoostCaption, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(appVersion, style = BoostCaption, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.12f))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("Gauge firmware", style = BoostCaption, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Text(firmware, style = BoostCaption, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}
