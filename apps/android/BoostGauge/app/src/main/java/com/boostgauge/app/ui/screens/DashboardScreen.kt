package com.boostgauge.app.ui.screens

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.ConnectionStatus
import com.boostgauge.app.data.api.Obd
import com.boostgauge.app.data.api.Sensors
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.Tpms
import com.boostgauge.app.data.api.ThemesPayload
import com.boostgauge.app.data.api.Wheel
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
import com.boostgauge.app.ui.displayLabel
import com.boostgauge.app.ui.viewmodels.StatusViewModel

@Composable
fun DashboardScreen(container: AppContainer) {
    val viewModel: StatusViewModel = viewModel(
        factory = viewModelFactory {
            initializer { StatusViewModel(container.repository, container.api) }
        },
    )
    val status by viewModel.status.collectAsState()
    val connectionStatus by viewModel.connectionStatus.collectAsState()
    val reconnectAttempt by viewModel.reconnectAttempt.collectAsState()
    val lastError by viewModel.lastError.collectAsState()
    val themeNames by viewModel.themeNames.collectAsState()
    val themes by viewModel.themes.collectAsState()
    val selection by container.transportController.selection.collectAsState()
    LaunchedEffect(Unit) { viewModel.refresh() }

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

        val cards: List<@Composable () -> Unit> = buildList {
            add { BoostHeroCard(status, status == null, themeNames[status?.activeThemeId] ?: status?.activeThemeId ?: "—") }
            add { SensorsCard(status?.sensors) }
            add { TpmsCard(status) }
            add { ObdCard(status?.obd, themes?.tpmsBle == true) }
            // While reconnecting, the status is "Reconnecting… (attempt N)", not an error banner.
            if (lastError != null && reconnectAttempt == null) add { ErrorBanner(lastError!!) }
            add { TransportFooter(connectionStatus, peerKnown = selection.bleAddress.isNotBlank(), reconnectAttempt) }
        }

        if (LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE) {
            Row(
                modifier = Modifier.fillMaxSize().padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp),
                horizontalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                DashboardPane(cards.subList(0, 2), Modifier.weight(1f))
                DashboardPane(cards.subList(2, cards.size), Modifier.weight(1f))
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxSize(),
                contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                cards.forEach { item { it() } }
            }
        }
    }
}

/** Side-by-side scroll pane used by the landscape dashboard layout. */
@Composable
private fun DashboardPane(cards: List<@Composable () -> Unit>, modifier: Modifier = Modifier) {
    LazyColumn(
        modifier = modifier.fillMaxHeight(),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        cards.forEach { item { it() } }
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
                Box(
                    modifier = Modifier.fillMaxWidth(),
                    contentAlignment = Alignment.Center,
                ) {
                    ZoneChip(status.zone)
                    Box(
                        modifier = Modifier.fillMaxSize(),
                        contentAlignment = Alignment.CenterEnd,
                    ) {
                        ModeChip(status.demo, Modifier.padding(end = 4.dp))
                    }
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
private fun ModeChip(demo: Boolean, modifier: Modifier = Modifier) {
    Pill(
        text = if (demo) "DEMO" else "LIVE",
        containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
        textColor = MaterialTheme.colorScheme.onSurfaceVariant,
        style = BoostSectionTitle.copy(fontWeight = FontWeight.Bold),
        horizontalPadding = 12.dp,
        verticalPadding = 4.dp,
        modifier = modifier,
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
private fun TpmsCard(status: Status?) {
    val tpms = status?.tpms
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
        if (tpms != null) {
            val wheels = (0 until 4).map { tpms.wheels.getOrNull(it) ?: Wheel() }
            val labels = listOf("FL", "FR", "RL", "RR")
            (0 until 2).forEach { row ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(10.dp),
                ) {
                    (0 until 2).forEach { col ->
                        val index = row * 2 + col
                        TireCapsule(
                            wheel = wheels[index],
                            label = labels[index],
                            lowPsi = tpms.lowPsi,
                            status = tpms.status,
                            modifier = Modifier.weight(1f),
                        )
                    }
                }
            }
        }
    }
}

/** Native tire capsule mirroring the iOS 2x2 TPMS card. */
@Composable
private fun TireCapsule(wheel: Wheel, label: String, lowPsi: Double, status: Int, modifier: Modifier = Modifier) {
    val low = wheel.valid && lowPsi > 0.0 && wheel.psi <= lowPsi
    val tint = when {
        status == 1 -> BoostColors.amber
        !wheel.valid -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
        low -> BoostColors.warning
        else -> BoostColors.success
    }
    Column(
        modifier = modifier
            .background(tint.copy(alpha = 0.12f), RoundedCornerShape(12.dp))
            .border(1.dp, tint.copy(alpha = 0.35f), RoundedCornerShape(12.dp))
            .padding(vertical = 10.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(5.dp),
    ) {
        Text(
            text = label,
            style = BoostCaptionSemibold,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = if (status == 1 || wheel.valid) Format.fmt(wheel.psi, 1) else "--.-",
            style = BoostTileValue,
            color = tint,
        )
        Box(
            modifier = Modifier
                .size(8.dp)
                .background(if (wheel.valid) tint else Color.Transparent, CircleShape)
                .border(1.dp, tint.copy(alpha = 0.8f), CircleShape),
        )
    }
}

@Composable
private fun ObdCard(obd: Obd?, tpmsBle: Boolean) {
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
                text = if (tpmsBle) {
                    "No OBD2 adapter connected."
                } else {
                    "No OBD2 link. Enable tpmsBle in Settings."
                },
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
private fun TransportFooter(connectionStatus: ConnectionStatus, peerKnown: Boolean, reconnectAttempt: Int?) {
    val dotColor = when (connectionStatus) {
        ConnectionStatus.Connected -> BoostColors.success
        ConnectionStatus.Reconnecting -> MaterialTheme.colorScheme.error
        ConnectionStatus.Disconnected -> MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
    }
    Row(
        modifier = Modifier.padding(top = 4.dp),
        horizontalArrangement = Arrangement.spacedBy(6.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PresenceDot(size = 8.dp, color = dotColor)
        Text(
            text = connectionStatus.displayLabel(peerKnown, reconnectAttempt),
            style = BoostFooter,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
