package com.boostgauge.app.ui.screens

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.CallMade
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.WarningAmber
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.Obd
import com.boostgauge.app.data.api.Sensors
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.Tpms
import com.boostgauge.app.data.api.Wheel
import com.boostgauge.app.data.settings.TransportType
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaption2
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFooter
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostHeroValue
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostMetricEmphasis
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.BoostSectionTitle
import com.boostgauge.app.ui.BoostTileValue
import com.boostgauge.app.ui.BoostUnit
import com.boostgauge.app.ui.Format
import com.boostgauge.app.ui.components.BoostCard
import com.boostgauge.app.ui.components.BoostSectionTitleText
import com.boostgauge.app.ui.components.MetricRow
import com.boostgauge.app.ui.components.Pill
import com.boostgauge.app.ui.components.PresenceBadge
import com.boostgauge.app.ui.components.PresenceDot
import com.boostgauge.app.ui.viewmodels.StatusViewModel

@Composable
fun DashboardScreen(container: AppContainer) {
    val viewModel: StatusViewModel = viewModel(
        factory = viewModelFactory {
            initializer { StatusViewModel(container.repository, container.api) }
        },
    )
    val status by viewModel.status.collectAsState()
    val connected by viewModel.connected.collectAsState()
    val lastError by viewModel.lastError.collectAsState()
    val themeNames by viewModel.themeNames.collectAsState()
    val selection by container.transportController.selection.collectAsState()

    Column(modifier = Modifier.fillMaxSize()) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(start = 16.dp, end = 8.dp, top = 4.dp),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "Boost Gauge",
                style = BoostNavTitle,
                color = MaterialTheme.colorScheme.onBackground,
            )
            IconButton(onClick = { viewModel.refresh() }) {
                Icon(
                    Icons.Filled.Refresh,
                    contentDescription = "Refresh",
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        val transportLabel = when (selection.type) {
            TransportType.HTTP -> "HTTP"
            TransportType.BLE -> "BLE"
        }

        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp),
            verticalArrangement = Arrangement.spacedBy(16.dp),
        ) {
            item {
                BoostHeroCard(
                    status = status,
                    loading = status == null,
                    themeName = themeNames[status?.activeThemeId] ?: status?.activeThemeId ?: "—",
                )
            }
            item { SensorsCard(status?.sensors) }
            item { TpmsCard(status?.tpms) }
            item { ObdCard(status?.obd) }
            if (lastError != null) {
                item(key = "error-banner") { ErrorBanner(lastError!!) }
            }
            item {
                TransportFooter(
                    connected = connected,
                    transportLabel = transportLabel,
                    hasError = lastError != null,
                )
            }
        }
    }
}

@Composable
private fun BoostHeroCard(status: Status?, loading: Boolean, themeName: String) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.Center,
                verticalAlignment = Alignment.Bottom,
            ) {
                Text(
                    text = status?.let { Format.fmt(it.psi) } ?: "--.-",
                    style = BoostHeroValue,
                    color = MaterialTheme.colorScheme.onSurface,
                    modifier = Modifier.alignByBaseline(),
                )
                Text(
                    text = "psi",
                    style = BoostUnit,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier
                        .alignByBaseline()
                        .padding(start = 8.dp),
                )
            }
            if (status == null) {
                if (loading) {
                    CircularProgressIndicator(
                        modifier = Modifier.size(24.dp),
                        strokeWidth = 2.dp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            } else {
                Row(
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    ZoneChip(status.zone)
                    ModeChip(status.demo)
                }
                Row(
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Icon(
                        Icons.AutoMirrored.Filled.CallMade,
                        contentDescription = null,
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.size(16.dp),
                    )
                    Text(
                        text = "Peak ${Format.fmt(status.peakPsi)} psi",
                        style = BoostMetric,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                HorizontalDivider(color = MaterialTheme.colorScheme.onSurface.copy(alpha = 0.18f))
                Column(verticalArrangement = Arrangement.spacedBy(12.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        MetadataTile(
                            title = "Theme",
                            value = themeName,
                            modifier = Modifier.weight(1f),
                        )
                        MetadataTile(
                            title = "Uptime",
                            value = status.uptimeMs.let { Format.formatUptime(it) },
                            modifier = Modifier.weight(1f),
                        )
                    }
                    Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
                        MetadataTile(
                            title = "Firmware",
                            value = status.firmwareVersion.ifBlank { "—" },
                            modifier = Modifier.weight(1f),
                        )
                        MetadataTile(
                            title = "Page",
                            value = if (status.activePage == 1) "TPMS" else "Boost",
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun MetadataTile(title: String, value: String, modifier: Modifier = Modifier) {
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(10.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHighest,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 8.dp, vertical = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(2.dp),
        ) {
            Text(
                text = value,
                style = BoostMetricEmphasis,
                color = MaterialTheme.colorScheme.onSurface,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
            Text(
                text = title,
                style = BoostCaption,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
            )
        }
    }
}

@Composable
private fun ZoneChip(zone: String) {
    val color = when (zone.uppercase()) {
        "VAC", "VACUUM" -> BoostColors.vacuum
        "BOOST" -> BoostColors.success
        "OVER", "OVERBOOST" -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.onSurfaceVariant
    }
    Pill(
        text = zone.ifBlank { "—" },
        containerColor = color.copy(alpha = 0.18f),
        textColor = color,
        style = BoostSectionTitle.copy(fontWeight = FontWeight.Bold),
        horizontalPadding = 12.dp,
        verticalPadding = 4.dp,
    )
}

@Composable
private fun ModeChip(demo: Boolean) {
    Pill(
        text = if (demo) "DEMO" else "LIVE",
        containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
        textColor = MaterialTheme.colorScheme.onSurfaceVariant,
        style = BoostCaptionSemibold,
        horizontalPadding = 10.dp,
        verticalPadding = 4.dp,
    )
}

@Composable
private fun SensorsCard(sensors: Sensors?) {
    BoostCard {
        BoostSectionTitleText("Sensors")
        Row(
            horizontalArrangement = Arrangement.spacedBy(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            PresenceBadge("ADS", present = sensors?.adsPresent == true)
            PresenceBadge("BMP", present = sensors?.bmpPresent == true)
            if (sensors?.fault == true) {
                Text(
                    text = "FAULT",
                    style = BoostCaptionSemibold,
                    color = MaterialTheme.colorScheme.error,
                )
            }
        }
        MetricRow(
            label = "MAP",
            value = sensors?.let { "${Format.fmt(it.mapVolts, 4)} V" } ?: "—",
        )
        MetricRow(
            label = "Abs kPa",
            value = sensors?.let { Format.fmt(it.mapAbsKpa, 2) } ?: "—",
        )
        MetricRow(
            label = "Ambient",
            value = sensors?.let { Format.fmt(it.ambientKpa, 2) } ?: "—",
        )
    }
}

@Composable
private fun TpmsCard(tpms: Tpms?) {
    BoostCard {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            BoostSectionTitleText("TPMS")
            if ((tpms?.lowPsi ?: 0.0) > 0.0) {
                Text(
                    text = "low ${Format.fmt(tpms!!.lowPsi, 1)}",
                    style = BoostCaption,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        val labels = listOf("FL", "FR", "RL", "RR")
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            labels.forEachIndexed { index, label ->
                TpmsTile(
                    label = label,
                    wheel = tpms?.wheels?.getOrNull(index),
                    lowPsi = tpms?.lowPsi ?: 0.0,
                    modifier = Modifier.weight(1f),
                )
            }
        }
    }
}

@Composable
private fun TpmsTile(label: String, wheel: Wheel?, lowPsi: Double, modifier: Modifier = Modifier) {
    val valid = wheel != null && wheel.valid
    val valueColor = when {
        wheel == null || !wheel.valid -> MaterialTheme.colorScheme.onSurfaceVariant
        lowPsi > 0.0 && wheel.psi < lowPsi -> BoostColors.warning
        else -> MaterialTheme.colorScheme.onSurface
    }
    Surface(
        modifier = modifier,
        shape = RoundedCornerShape(10.dp),
        color = MaterialTheme.colorScheme.surfaceContainerHighest,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 4.dp, vertical = 8.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.spacedBy(4.dp),
        ) {
            Text(
                text = label,
                style = BoostCaption2,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Text(
                text = if (valid) Format.fmt(wheel.psi, 1) else "—",
                style = BoostTileValue,
                color = valueColor,
                maxLines = 1,
            )
            PresenceDot(
                size = 7.dp,
                color = if (valid) {
                    BoostColors.success
                } else {
                    MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
                },
            )
        }
    }
}

@Composable
private fun ObdCard(obd: Obd?) {
    BoostCard {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            BoostSectionTitleText("OBD2")
            if (obd?.valid == true) {
                Text(
                    text = "link",
                    style = BoostCaptionSemibold,
                    color = BoostColors.success,
                )
            }
        }
        if (obd?.valid == true) {
            MetricRow("Adapter", obd.peer.ifBlank { "ELM" })
            MetricRow("RPM", Format.fmt(obd.rpm, 0))
            MetricRow("Speed", "${Format.fmt(obd.speedKph, 0)} km/h")
            MetricRow("Coolant", "${Format.fmt(obd.coolantC, 0)} °C")
            MetricRow("Battery", "${Format.fmt(obd.batteryV, 1)} V")
        } else {
            Text(
                text = "No OBD2 link. Enable tpmsBle in Settings.",
                style = BoostMetric,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun ErrorBanner(message: String) {
    var expanded by remember { mutableStateOf(false) }
    Surface(
        onClick = { expanded = !expanded },
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(10.dp),
        color = BoostColors.warning.copy(alpha = 0.12f),
    ) {
        Row(
            modifier = Modifier.padding(10.dp),
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.Top,
        ) {
            Icon(
                Icons.Filled.WarningAmber,
                contentDescription = null,
                tint = BoostColors.warning,
                modifier = Modifier.size(18.dp),
            )
            Text(
                text = message,
                style = BoostFootnote,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.weight(1f),
                maxLines = if (expanded) Int.MAX_VALUE else 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
    }
}

@Composable
private fun TransportFooter(connected: Boolean, transportLabel: String, hasError: Boolean) {
    val dotColor = when {
        connected -> BoostColors.success
        hasError -> MaterialTheme.colorScheme.error
        else -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
    }
    Row(
        modifier = Modifier.padding(top = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PresenceDot(size = 8.dp, color = dotColor)
        Text(
            text = if (connected) "Live · $transportLabel" else "Disconnected",
            style = BoostFooter,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
