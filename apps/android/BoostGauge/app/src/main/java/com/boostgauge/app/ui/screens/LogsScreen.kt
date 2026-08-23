package com.boostgauge.app.ui.screens

import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Inbox
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
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
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.LogSample
import com.boostgauge.app.ui.BoostCaption2
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFooter
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostMetricEmphasis
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.Format
import com.boostgauge.app.ui.components.Pill
import com.boostgauge.app.ui.viewmodels.LogsViewModel
import kotlinx.coroutines.launch

@Composable
fun LogsScreen(container: AppContainer) {
    val viewModel: LogsViewModel = viewModel(
        factory = viewModelFactory {
            initializer { LogsViewModel(container.api, container.repository) }
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
                    text = "Logs",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                IconButton(onClick = { viewModel.load() }) {
                    Icon(
                        Icons.Filled.Refresh,
                        contentDescription = "Refresh logs",
                        tint = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }
        }
        item {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                listOf(50, 300, 3000).forEach { limit ->
                    FilterChip(
                        selected = state.limit == limit,
                        onClick = { viewModel.load(limit) },
                        label = { Text("$limit") },
                    )
                }
                Spacer(modifier = Modifier.weight(1f))
                Button(
                    onClick = { exportLauncher.launch("boost-gauge-log.csv") },
                    modifier = Modifier.weight(1f),
                ) {
                    Text("Export CSV")
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
        when {
            state.loading -> item {
                Box(
                    modifier = Modifier.fillMaxWidth().padding(40.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator()
                }
            }
            state.samples.isEmpty() -> item {
                EmptyLogs(
                    error = state.error,
                    onLoad = { viewModel.load() },
                )
            }
            else -> itemsIndexed(state.samples, key = { _, sample -> sample.tMs }) { index, sample ->
                LogRow(sample)
                if (index != state.samples.lastIndex) {
                    HorizontalDivider(color = MaterialTheme.colorScheme.outlineVariant)
                }
            }
        }
    }
}

@Composable
private fun LogRow(sample: LogSample) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = Format.formatUptime(sample.tMs),
            style = BoostFooter,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(90.dp),
        )
        Spacer(modifier = Modifier.weight(1f))
        Text(
            text = Format.fmt(sample.psi),
            style = BoostMetricEmphasis,
            color = MaterialTheme.colorScheme.onSurface,
            textAlign = TextAlign.End,
            modifier = Modifier.width(64.dp),
        )
        Pill(
            text = sample.zone.ifBlank { "—" },
            containerColor = MaterialTheme.colorScheme.surfaceContainerHighest,
            textColor = MaterialTheme.colorScheme.onSurfaceVariant,
            style = BoostCaptionSemibold,
            horizontalPadding = 8.dp,
            verticalPadding = 2.dp,
            modifier = Modifier.padding(start = 12.dp),
        )
        if (sample.demo) {
            Text(
                text = "demo",
                style = BoostCaption2,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                modifier = Modifier.padding(start = 8.dp).width(36.dp),
            )
        }
    }
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
