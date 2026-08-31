package com.boostgauge.app.ui.screens

import android.content.res.Configuration
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ChevronRight
import androidx.compose.material.icons.filled.Remove
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.TextButton
import androidx.compose.material3.TextField
import androidx.compose.material3.TextFieldDefaults
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.foundation.clickable
import androidx.compose.foundation.interaction.MutableInteractionSource
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostSubheadline
import com.boostgauge.app.ui.BoostCardShape
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.Switch
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.material3.FilledTonalIconButton
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.lifecycle.viewmodel.initializer
import androidx.lifecycle.viewmodel.viewModelFactory
import com.boostgauge.app.AppContainer
import com.boostgauge.app.data.api.Config
import com.boostgauge.app.data.api.Status
import com.boostgauge.app.data.api.ThemeColors
import com.boostgauge.app.data.api.ThemeInfo
import com.boostgauge.app.data.api.ThemesPayload
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaption2
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostColors
import com.boostgauge.app.ui.BoostFootnote
import com.boostgauge.app.ui.BoostNavTitle
import com.boostgauge.app.ui.BoostSectionTitle
import com.boostgauge.app.ui.components.Pill
import com.boostgauge.app.ui.components.CanonicalGaugePreview
import com.boostgauge.app.ui.viewmodels.ThemesViewModel

@Composable
fun ThemesScreen(container: AppContainer) {
    val viewModel: ThemesViewModel = viewModel(
        factory = viewModelFactory {
            initializer { ThemesViewModel(container.api, container.repository.connectionStatus) }
        },
    )
    val state by viewModel.state.collectAsState()

    // Tab re-entry: the board is authoritative and may have moved while the
    // user was on another tab (other phone, web UI). The ViewModel survives
    // tab switches (saveState/restoreState), so this must re-fire on every
    // return or the preview stays frozen on the old theme. Mirrors iOS
    // `.onAppear { resyncActiveTheme() }`. resyncActiveTheme() skips while the
    // initial load is in flight and is seq-guarded against a concurrent tap.
    LaunchedEffect(Unit) { viewModel.resyncActiveTheme() }

    if (LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE) {
        LandscapeThemes(viewModel, state)
    } else {
        PortraitThemes(viewModel, state)
    }
}

@Composable
private fun ThemesHeader(onRefresh: () -> Unit) {
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
        IconButton(onClick = onRefresh) {
            Icon(
                Icons.Filled.Refresh,
                contentDescription = "Refresh themes",
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
    }
}

@Composable
private fun PortraitThemes(viewModel: ThemesViewModel, state: ThemesViewModel.UiState) {
    LazyColumn(
        modifier = Modifier.fillMaxSize(),
        contentPadding = PaddingValues(start = 16.dp, end = 16.dp, top = 4.dp, bottom = 16.dp),
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        item {
            ThemesHeader(onRefresh = viewModel::load)
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
                    Text(
                        text = "Preview",
                        style = BoostSectionTitle,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                state.themes.firstOrNull { it.id == state.activeThemeId }?.let { active ->
                    item {
                        // The pod always keeps its laid-out size — while
                        // status/config load (or after a transport drop cleared
                        // them) it shows a placeholder spinner, never a collapse
                        // to an empty section.
                        ThemePreviewPod(
                            status = state.status,
                            config = state.config,
                            themes = state.payload,
                            theme = active,
                        )
                    }
                }
                item {
                    Text(
                        text = "Gauge themes",
                        style = BoostSectionTitle,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                item {
                    ThemeGroup(
                        themes = state.themes,
                        activeThemeId = state.activeThemeId,
                        activatingId = state.activatingId,
                        onSelect = viewModel::activate,
                        viewModel = viewModel,
                        state = state,
                    )
                }
            }
        }
    }
}

/**
 * Landscape two-pane: the circular preview stays square and centered on the
 * left; the theme list scrolls beside it. The preview is capped to the pane's
 * shorter side so it never overflows the short landscape viewport.
 */
@Composable
private fun LandscapeThemes(viewModel: ThemesViewModel, state: ThemesViewModel.UiState) {
    Column(modifier = Modifier.fillMaxSize()) {
        ThemesHeader(onRefresh = viewModel::load)
        state.error?.let {
            Text(
                text = it,
                style = BoostFootnote,
                color = BoostColors.warning,
                modifier = Modifier.padding(horizontal = 16.dp),
            )
        }
        when {
            state.loading && state.themes.isEmpty() -> Box(
                modifier = Modifier.fillMaxSize(),
                contentAlignment = Alignment.Center,
            ) {
                CircularProgressIndicator()
            }
            else -> {
                Row(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(start = 16.dp, end = 16.dp, top = 8.dp, bottom = 16.dp),
                    horizontalArrangement = Arrangement.spacedBy(16.dp),
                ) {
                    val active = state.themes.firstOrNull { it.id == state.activeThemeId }
                    if (active != null) {
                        BoxWithConstraints(
                            modifier = Modifier.weight(1f).fillMaxHeight(),
                            contentAlignment = Alignment.Center,
                        ) {
                            val side = minOf(
                                maxWidth,
                                (maxHeight - 40.dp).coerceAtLeast(0.dp),
                            ).coerceAtLeast(120.dp)
                            Box(Modifier.width(side)) {
                                ThemePreviewPod(
                                    status = state.status,
                                    config = state.config,
                                    themes = state.payload,
                                    theme = active,
                                )
                            }
                        }
                    } else {
                        Spacer(modifier = Modifier.weight(1f))
                    }
                    LazyColumn(
                        modifier = Modifier.weight(1.2f).fillMaxHeight(),
                        verticalArrangement = Arrangement.spacedBy(16.dp),
                    ) {
                        item {
                            Text(
                                text = "Gauge themes",
                                style = BoostSectionTitle,
                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                            )
                        }
                        item {
                            ThemeGroup(
                                themes = state.themes,
                                activeThemeId = state.activeThemeId,
                                activatingId = state.activatingId,
                                onSelect = viewModel::activate,
                                viewModel = viewModel,
                                state = state,
                            )
                        }
                    }
                }
            }
        }
    }
}

/**
 * Circular gauge preview matching iOS `ThemesView.themePreview`: the web
 * `.gauge-device` bezel as concentric disks — a 1 dp dark rim, an 8 dp
 * `#0c0e12` pod ring and a hairline rim on the clipped face — transparent
 * corners and no offset shadow. Padding keeps the ring clear of the row edge.
 *
 * The pod keeps its laid-out size even while [status]/[config] are null
 * (initial load, or a transport drop cleared the live preview): it renders a
 * placeholder spinner in the same silhouette instead of collapsing the section.
 */
@Composable
private fun ThemePreviewPod(
    status: Status?,
    config: Config?,
    themes: ThemesPayload?,
    theme: ThemeInfo,
) {
    val pod = Color(0xFF0C0E12)
    val darkRim = Color.Black.copy(alpha = 0.9f)
    val hairline = Color.White.copy(alpha = 0.05f)
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 20.dp, horizontal = 8.dp),
        contentAlignment = Alignment.Center,
    ) {
        Box(
            modifier = Modifier.fillMaxWidth().aspectRatio(1f),
            contentAlignment = Alignment.Center,
        ) {
            Box(Modifier.fillMaxSize().clip(CircleShape).background(darkRim))
            Box(
                Modifier.fillMaxSize().padding(1.dp).clip(CircleShape).background(pod),
            )
            Box(
                Modifier.fillMaxSize().padding(9.dp).clip(CircleShape),
            ) {
                if (status != null && config != null) {
                    CanonicalGaugePreview(
                        status = status,
                        config = config,
                        themes = themes,
                        theme = theme,
                        modifier = Modifier.fillMaxSize(),
                    )
                } else {
                    CircularProgressIndicator(
                        modifier = Modifier.align(Alignment.Center),
                        strokeWidth = 2.dp,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                Box(
                    Modifier
                        .fillMaxSize()
                        .border(1.dp, hairline, CircleShape),
                )
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
    viewModel: ThemesViewModel,
    state: ThemesViewModel.UiState,
) {
    var expandedId by remember(activeThemeId) { mutableStateOf<String?>(activeThemeId.ifBlank { themes.firstOrNull()?.id }) }
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
                    expanded = expandedId == theme.id,
                    onExpand = { expandedId = if (expandedId == theme.id) null else theme.id },
                    viewModel = viewModel,
                    state = state,
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
    expanded: Boolean,
    onExpand: () -> Unit,
    viewModel: ThemesViewModel,
    state: ThemesViewModel.UiState,
) {
    Column(modifier = Modifier.fillMaxWidth()) {
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
        }
        PaletteCircles(theme = theme, viewModel = viewModel)
        if (activating) {
            CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
        } else if (active) {
            Icon(
                Icons.Filled.CheckCircle,
                contentDescription = "Active",
                tint = BoostColors.success,
            )
        }
        IconButton(onClick = onExpand, modifier = Modifier.size(32.dp)) {
            Icon(
                Icons.Filled.ChevronRight,
                contentDescription = if (expanded) "Hide theme settings" else "Show theme settings",
                modifier = Modifier.rotate(if (expanded) 90f else 0f),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
      }
      if (expanded) {
          HorizontalDivider(
              modifier = Modifier.padding(horizontal = 16.dp),
              color = MaterialTheme.colorScheme.outlineVariant,
          )
          ThemeOptionsEditor(theme = theme, viewModel = viewModel, state = state)
      }
    }
}

@Composable
private fun PaletteCircles(theme: ThemeInfo, viewModel: ThemesViewModel) {
    Row(horizontalArrangement = Arrangement.spacedBy(3.dp), verticalAlignment = Alignment.CenterVertically) {
        ThemesViewModel.zoneKeys.forEach { key ->
            val hex = viewModel.colorHex(theme, key)
            if (hex != null) {
                val color = parseHexColor(hex)
                Box(
                    modifier = Modifier
                        .size(11.dp)
                        .background(color, CircleShape)
                )
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun ThemeOptionsEditor(
    theme: ThemeInfo,
    viewModel: ThemesViewModel,
    state: ThemesViewModel.UiState,
) {
    Column(
        modifier = Modifier
            .fillMaxWidth()
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(14.dp),
    ) {
        // GroupBox("Zone colors")
        Surface(
            modifier = Modifier.fillMaxWidth(),
            shape = RoundedCornerShape(12.dp),
            color = MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.5f),
        ) {
            Column(
                modifier = Modifier
                    .fillMaxWidth()
                    .padding(12.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Text(
                    text = "Zone colors",
                    style = BoostCaptionSemibold,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                ThemesViewModel.zoneKeys.forEach { key ->
                    val hex = viewModel.colorHex(theme, key) ?: "#000000"
                    ColorPickerRow(
                        label = paletteLabel(key),
                        hex = hex,
                        onHexChange = { newHex ->
                            viewModel.setColor(theme.id, key, newHex)
                        }
                    )
                }
            }
        }

        when (theme.id) {
            "dyno-cell" -> {
                ToggleRow(
                    label = "Gradient fill",
                    checked = state.arcGradient,
                    onCheckedChange = viewModel::updateArcGradient,
                )
            }
            "vault-tec" -> {
                OutlinedTextField(
                    value = state.vaultFace,
                    onValueChange = viewModel::updateVaultFace,
                    label = { Text("Face color (#RRGGBB)") },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.None,
                        autoCorrectEnabled = false,
                    ),
                    modifier = Modifier.fillMaxWidth(),
                )
                StepperRow(
                    label = "Vignette: ${state.vaultVignette}%",
                    value = state.vaultVignette,
                    onValueChange = viewModel::updateVaultVignette,
                    range = 0..90,
                    step = 5,
                )
                ToggleRow(
                    label = "Red needle",
                    checked = state.vaultNeedleRed,
                    onCheckedChange = viewModel::updateVaultNeedleRed,
                )
                ToggleRow(
                    label = "Counterweight tail",
                    checked = state.vaultNeedleTail,
                    onCheckedChange = viewModel::updateVaultNeedleTail,
                )
            }
            "night-city" -> {
                ToggleRow(
                    label = "Gradient fill",
                    checked = state.hudGradient,
                    onCheckedChange = viewModel::updateHudGradient,
                )
                ToggleRow(
                    label = "True black background",
                    checked = state.hudTrueBlack,
                    onCheckedChange = viewModel::updateHudTrueBlack,
                )
            }
            "big-digit" -> {
                ToggleRow(
                    label = "Static background",
                    checked = state.bigDigitStaticBg,
                    onCheckedChange = viewModel::updateBigDigitStaticBg,
                )
                ToggleRow(
                    label = "Color the readout",
                    checked = state.bigDigitColorText,
                    onCheckedChange = viewModel::updateBigDigitColorText,
                )
                if (state.bigDigitStaticBg) {
                    ColorPickerRow(
                        label = "Static background color",
                        hex = state.bigDigitStaticColor,
                        onHexChange = viewModel::updateBigDigitStaticColor,
                    )
                }
                if (!state.bigDigitColorText) {
                    ColorPickerRow(
                        label = "Readout text color",
                        hex = state.bigDigitTextColor,
                        onHexChange = viewModel::updateBigDigitTextColor,
                    )
                }
            }
            "neon" -> {
                DropdownPickerRow(
                    label = "Layout",
                    options = listOf("Tube", "Segments", "Marquee"),
                    selectedIndex = state.neonLayout.coerceIn(0, 2),
                    onSelect = viewModel::updateNeonLayout,
                )
                DropdownPickerRow(
                    label = "Preset",
                    options = listOf("Violet", "Miami", "Toxic", "Blood Moon"),
                    selectedIndex = state.neonPreset.coerceIn(0, 3),
                    onSelect = viewModel::updateNeonPreset,
                )
                DropdownPickerRow(
                    label = "Readout font",
                    options = listOf("SF Alien", "Doto"),
                    selectedIndex = state.neonFont.coerceIn(0, 1),
                    onSelect = viewModel::updateNeonFont,
                )
                if (state.neonLayout == 2) {
                    ToggleRow(
                        label = "Marquee spin",
                        checked = state.neonMarqueeSpin,
                        onCheckedChange = viewModel::updateNeonMarqueeSpin,
                    )
                }
            }
            else -> {
                Text(
                    text = "This theme has no additional options.",
                    style = BoostCaption,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }

        Button(
            onClick = { viewModel.saveOptions(theme.id) },
            modifier = Modifier.fillMaxWidth().height(44.dp),
            shape = RoundedCornerShape(10.dp),
            enabled = !state.loading,
        ) {
            Text("Apply ${theme.name} options")
        }

        if (theme.customized) {
            TextButton(
                onClick = { viewModel.resetColors(theme.id) },
                enabled = !state.loading,
                colors = ButtonDefaults.textButtonColors(contentColor = MaterialTheme.colorScheme.error),
            ) {
                Text("Reset to default colors")
            }
        }
    }
}

@Composable
private fun ColorPickerRow(
    label: String,
    hex: String,
    onHexChange: (String) -> Unit,
) {
    var showDialog by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clickable { showDialog = true }
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(hex.uppercase(), style = BoostCaption, color = MaterialTheme.colorScheme.onSurfaceVariant)
            Box(
                modifier = Modifier
                    .size(24.dp)
                    .background(parseHexColor(hex), CircleShape)
                    .border(1.dp, MaterialTheme.colorScheme.outlineVariant, CircleShape)
            )
        }
    }

    if (showDialog) {
        ColorPickerDialog(
            initialHex = hex,
            title = label,
            onDismiss = { showDialog = false },
            onConfirm = { newHex ->
                onHexChange(newHex)
                showDialog = false
            }
        )
    }
}

@Composable
private fun ColorPickerDialog(
    initialHex: String,
    title: String,
    onDismiss: () -> Unit,
    onConfirm: (String) -> Unit,
) {
    var hexText by remember { mutableStateOf(initialHex.removePrefix("#")) }
    androidx.compose.ui.window.Dialog(onDismissRequest = onDismiss) {
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.surfaceContainerHigh,
            modifier = Modifier.fillMaxWidth().padding(16.dp),
        ) {
            Column(
                modifier = Modifier.padding(20.dp),
                verticalArrangement = Arrangement.spacedBy(16.dp),
            ) {
                Text(title, style = BoostSectionTitle)
                val previewColor = parseHexColor("#$hexText")
                Box(
                    modifier = Modifier
                        .fillMaxWidth()
                        .height(48.dp)
                        .background(previewColor, RoundedCornerShape(8.dp))
                        .border(1.dp, MaterialTheme.colorScheme.outlineVariant, RoundedCornerShape(8.dp))
                )
                OutlinedTextField(
                    value = hexText,
                    onValueChange = { if (it.length <= 6) hexText = it },
                    label = { Text("Hex Color (RRGGBB)") },
                    prefix = { Text("#") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                // Quick swatches
                val quickSwatches = listOf(
                    "#00E5FF", "#00FF66", "#FF0055", "#FFE600",
                    "#FF3366", "#3388FF", "#FFFFFF", "#000000",
                    "#05281A", "#0C0E12", "#003311", "#FF9900"
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    quickSwatches.take(6).forEach { swatch ->
                        Box(
                            modifier = Modifier
                                .size(28.dp)
                                .background(parseHexColor(swatch), CircleShape)
                                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, CircleShape)
                                .clickable { hexText = swatch.removePrefix("#") }
                        )
                    }
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                ) {
                    quickSwatches.drop(6).forEach { swatch ->
                        Box(
                            modifier = Modifier
                                .size(28.dp)
                                .background(parseHexColor(swatch), CircleShape)
                                .border(1.dp, MaterialTheme.colorScheme.outlineVariant, CircleShape)
                                .clickable { hexText = swatch.removePrefix("#") }
                        )
                    }
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End,
                ) {
                    TextButton(onClick = onDismiss) { Text("Cancel") }
                    Spacer(Modifier.width(8.dp))
                    Button(onClick = {
                        val valid = hexText.padStart(6, '0')
                        onConfirm("#$valid")
                    }) {
                        Text("Select")
                    }
                }
            }
        }
    }
}

@Composable
private fun StepperRow(
    label: String,
    value: Int,
    onValueChange: (Int) -> Unit,
    range: IntRange,
    step: Int = 1,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        Row(
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            FilledTonalIconButton(
                onClick = { onValueChange((value - step).coerceIn(range)) },
                enabled = value > range.first,
                modifier = Modifier.size(32.dp),
            ) {
                Icon(Icons.Filled.Remove, contentDescription = "Decrease", modifier = Modifier.size(16.dp))
            }
            FilledTonalIconButton(
                onClick = { onValueChange((value + step).coerceIn(range)) },
                enabled = value < range.last,
                modifier = Modifier.size(32.dp),
            ) {
                Icon(Icons.Filled.Add, contentDescription = "Increase", modifier = Modifier.size(16.dp))
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DropdownPickerRow(
    label: String,
    options: List<String>,
    selectedIndex: Int,
    onSelect: (Int) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = BoostMetric, color = MaterialTheme.colorScheme.onSurface)
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it },
        ) {
            Surface(
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
                        options.getOrElse(selectedIndex) { "" },
                        style = BoostSubheadline,
                        color = MaterialTheme.colorScheme.primary,
                    )
                    ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded)
                }
            }
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
            ) {
                options.forEachIndexed { index, option ->
                    DropdownMenuItem(
                        text = { Text(option) },
                        onClick = {
                            onSelect(index)
                            expanded = false
                        },
                    )
                }
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

private fun paletteLabel(key: String): String = when (key) {
    "face" -> "Face"
    "track" -> "Track"
    "text" -> "Text"
    "muted" -> "Muted"
    "vacuum" -> "Vacuum"
    "boost" -> "Boost"
    "overboost" -> "Overboost"
    "zero" -> "Zero marker"
    else -> key.replaceFirstChar { it.uppercase() }
}

private fun parseHexColor(hex: String): Color {
    val clean = hex.removePrefix("#")
    val num = clean.toLongOrNull(16) ?: 0L
    return when (clean.length) {
        6 -> Color(num or 0xFF000000L)
        8 -> Color(num)
        else -> Color.Gray
    }
}
