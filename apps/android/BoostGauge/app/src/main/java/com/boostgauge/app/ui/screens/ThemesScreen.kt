package com.boostgauge.app.ui.screens

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
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
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Refresh
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
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.ThemeColors
import com.boostgauge.app.data.api.ThemeInfo
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaption2
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.BoostSectionTitle
import com.boostgauge.app.ui.components.Pill
import com.boostgauge.app.ui.viewmodels.ThemesViewModel

private fun parseColor(hex: String): Color = runCatching {
    Color(hex.removePrefix("#").toLong(16) or 0xFF000000L)
}.getOrDefault(Color.Transparent)

@Composable
fun ThemesScreen(container: AppContainer) {
    val viewModel: ThemesViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ThemesViewModel(container.api) }
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
                    text = "Themes",
                    style = BoostNavTitle,
                    color = MaterialTheme.colorScheme.onBackground,
                )
                IconButton(onClick = { viewModel.load() }) {
                    Icon(
                        Icons.Filled.Refresh,
                        contentDescription = "Refresh themes",
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
        when {
            state.loading && state.themes.isEmpty() -> item {
                Box(
                    modifier = Modifier.fillMaxWidth().padding(40.dp),
                    contentAlignment = Alignment.Center,
                ) {
                    CircularProgressIndicator()
                }
            }
            else -> {
                item {
                    ThemeGroup(
                        themes = state.themes,
                        activeThemeId = state.activeThemeId,
                        activatingId = state.activatingId,
                        onSelect = viewModel::activate,
                    )
                }
            }
        }
    }
}

/** iOS-List-style inset grouped rows inside one pale group surface. */
@Composable
private fun ThemeGroup(
    themes: List<ThemeInfo>,
    activeThemeId: String,
    activatingId: String?,
    onSelect: (String) -> Unit,
) {
    Surface(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(modifier = Modifier.fillMaxWidth()) {
            themes.forEachIndexed { index, theme ->
                ThemeRow(
                    theme = theme,
                    active = theme.id == activeThemeId,
                    activating = theme.id == activatingId,
                    onClick = { onSelect(theme.id) },
                )
                if (index != themes.lastIndex) {
                    HorizontalDivider(
                        modifier = Modifier.padding(start = 16.dp),
                        color = MaterialTheme.colorScheme.outlineVariant,
                    )
                }
            }
        }
    }
}

@Composable
private fun ThemeRow(
    theme: ThemeInfo,
    active: Boolean,
    activating: Boolean,
    onClick: () -> Unit,
) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 12.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
            Row(
                horizontalArrangement = Arrangement.spacedBy(6.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    text = theme.name,
                    style = BoostSectionTitle,
                    color = MaterialTheme.colorScheme.onSurface,
                )
                if (theme.customized) {
                    Pill(
                        text = "custom",
                        containerColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.15f),
                        textColor = MaterialTheme.colorScheme.primary,
                        style = BoostCaption2,
                        horizontalPadding = 6.dp,
                        verticalPadding = 2.dp,
                    )
                }
            }
            if (theme.style.isNotBlank()) {
                Text(
                    text = theme.style,
                    style = BoostCaption,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            ColorSwatches(theme.colors, modifier = Modifier.padding(top = 2.dp))
        }
        if (activating) {
            CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
        } else if (active) {
            Icon(
                Icons.Filled.CheckCircle,
                contentDescription = "Active",
                tint = BoostColors.success,
            )
        }
    }
}

@Composable
private fun ColorSwatches(colors: ThemeColors, modifier: Modifier = Modifier) {
    val swatches = listOfNotNull(
        colors.face, colors.track, colors.vacuum, colors.boost,
        colors.overboost, colors.text, colors.zero,
    )
    Row(modifier = modifier, horizontalArrangement = Arrangement.spacedBy(5.dp)) {
        swatches.forEach { hex ->
            Box(
                modifier = Modifier
                    .size(14.dp)
                    .background(parseColor(hex), RoundedCornerShape(3.dp)),
            )
        }
    }
}
