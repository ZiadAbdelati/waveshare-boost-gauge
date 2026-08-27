package com.boostgauge.app.ui

import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.darkColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.runtime.Composable
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp

/**
 * Semantic accent palette matching the iOS app's sparse, semantic use of color.
 * These stay constant across light/dark (like SwiftUI system colors); error is
 * driven by the Material error role so dark mode gets the darker iOS red.
 */
object BoostColors {
    val vacuum = Color(0xFF00B5E2) // iOS-ish system cyan (zone badge)
    val success = Color(0xFF34C759) // iOS system green (health/link)
    val warning = Color(0xFFFF9500) // iOS system orange (inline errors)
    val danger = Color(0xFFFF3B30) // iOS system red (critical/fault)
    val navBlue = Color(0xFF007AFF) // iOS system blue (selected navigation)
}

// iOS-reference light scheme: white base, pale lavender-gray cards, slightly
// darker inset tiles, near-black text, gray secondary text.
private val BoostLightColorScheme = lightColorScheme(
    primary = BoostColors.navBlue,
    onPrimary = Color.White,
    primaryContainer = BoostColors.navBlue.copy(alpha = 0.15f),
    onPrimaryContainer = Color(0xFF003B75),
    secondary = BoostColors.navBlue,
    onSecondary = Color.White,
    secondaryContainer = BoostColors.navBlue.copy(alpha = 0.15f),
    onSecondaryContainer = Color(0xFF003B75),
    background = Color(0xFFFFFFFF),
    onBackground = Color(0xFF1C1C1E),
    surface = Color(0xFFFFFFFF),
    onSurface = Color(0xFF1C1C1E),
    surfaceVariant = Color(0xFFE4E3E9),
    onSurfaceVariant = Color(0xFF8A8992),
    surfaceContainerLowest = Color(0xFFFFFFFF),
    surfaceContainerLow = Color(0xFFF2F1F7),
    surfaceContainer = Color(0xFFECEBF1),
    surfaceContainerHigh = Color(0xFFE7E6EC),
    surfaceContainerHighest = Color(0xFFE4E3E9),
    outline = Color(0xFFDCDBE1),
    outlineVariant = Color(0xFFE9E8EE),
    error = BoostColors.danger,
    onError = Color.White,
    errorContainer = BoostColors.danger.copy(alpha = 0.12f),
    onErrorContainer = Color(0xFF8A1B1B),
)

// Equivalent dark hierarchy: black base, dark-gray cards, slightly lighter
// insets, white text, light-gray secondary (iOS secondarySystemBackground /
// tertiarySystemFill dark equivalents).
private val BoostDarkColorScheme = darkColorScheme(
    primary = Color(0xFF0A84FF),
    onPrimary = Color.White,
    primaryContainer = Color(0xFF0A84FF).copy(alpha = 0.2f),
    onPrimaryContainer = Color(0xFFD6E7FF),
    secondary = Color(0xFF0A84FF),
    onSecondary = Color(0xFF001B3D),
    secondaryContainer = Color(0xFF0A84FF).copy(alpha = 0.2f),
    onSecondaryContainer = Color(0xFFD6E7FF),
    background = Color(0xFF000000),
    onBackground = Color(0xFFFFFFFF),
    surface = Color(0xFF000000),
    onSurface = Color(0xFFFFFFFF),
    surfaceVariant = Color(0xFF2C2C2E),
    onSurfaceVariant = Color(0xFFAEAEB2),
    surfaceContainerLowest = Color(0xFF000000),
    surfaceContainerLow = Color(0xFF1C1C1E),
    surfaceContainer = Color(0xFF232325),
    surfaceContainerHigh = Color(0xFF28282A),
    surfaceContainerHighest = Color(0xFF2C2C2E),
    outline = Color(0xFF3A3A3C),
    outlineVariant = Color(0xFF2A2A2C),
    error = Color(0xFFFF453A),
    onError = Color.White,
    errorContainer = Color(0xFFFF453A).copy(alpha = 0.15f),
    onErrorContainer = Color(0xFFFFB4AB),
)

// ---------------------------------------------------------------------------
// Named text styles (the Compose translation of the iOS typography tokens).
// Numeric values carry fontFeatureSettings = "tnum" (SwiftUI monospacedDigit).
// ---------------------------------------------------------------------------

/** Hero PSI readout: 72 sp bold rounded/tabular. */
val BoostHeroValue = TextStyle(
    fontSize = 72.sp,
    lineHeight = 78.sp,
    fontWeight = FontWeight.Bold,
    fontFeatureSettings = "tnum",
)

/** Expanded navigation title: 34 sp bold. */
val BoostNavTitle = TextStyle(
    fontSize = 34.sp,
    lineHeight = 40.sp,
    fontWeight = FontWeight.Bold,
)

/** Section/zone headline: 17 sp semibold. */
val BoostSectionTitle = TextStyle(
    fontSize = 17.sp,
    lineHeight = 22.sp,
    fontWeight = FontWeight.SemiBold,
)

/** Hero unit: 20 sp regular. */
val BoostUnit = TextStyle(fontSize = 20.sp, lineHeight = 24.sp)

/** Metric row: 15 sp regular. */
val BoostMetric = TextStyle(fontSize = 15.sp, lineHeight = 20.sp)

/** Tabular metric value: 15 sp regular with tnum. */
val BoostMetricValue = TextStyle(
    fontSize = 15.sp,
    lineHeight = 20.sp,
    fontFeatureSettings = "tnum",
)

/** Metadata value: 15 sp semibold tabular. */
val BoostMetricEmphasis = TextStyle(
    fontSize = 15.sp,
    lineHeight = 20.sp,
    fontWeight = FontWeight.SemiBold,
    fontFeatureSettings = "tnum",
)

/** TPMS tile value: 17 sp semibold tabular. */
val BoostTileValue = TextStyle(
    fontSize = 17.sp,
    lineHeight = 22.sp,
    fontWeight = FontWeight.SemiBold,
    fontFeatureSettings = "tnum",
)

/** 12 sp regular caption. */
val BoostCaption = TextStyle(fontSize = 12.sp, lineHeight = 16.sp)

/** 12 sp semibold caption (pills, titles). */
val BoostCaptionSemibold = TextStyle(
    fontSize = 12.sp,
    lineHeight = 16.sp,
    fontWeight = FontWeight.SemiBold,
)

/** 11 sp regular caption (TPMS wheel labels). */
val BoostCaption2 = TextStyle(fontSize = 11.sp, lineHeight = 14.sp)

/** 13 sp footnote (inline errors/help). */
val BoostFootnote = TextStyle(fontSize = 13.sp, lineHeight = 18.sp)

/** Quiet transport footer: 12 sp monospaced (SwiftUI caption.monospaced). */
val BoostFooter = TextStyle(
    fontSize = 12.sp,
    lineHeight = 16.sp,
    fontFamily = FontFamily.Monospace,
)

/** Section header above cards: 13 sp regular uppercase / muted. */
val BoostSectionHeader = TextStyle(fontSize = 13.sp, lineHeight = 18.sp)

/** Monospaced caption: 12 sp regular monospace. */
val BoostMonoCaption = TextStyle(
    fontSize = 12.sp,
    lineHeight = 16.sp,
    fontFamily = FontFamily.Monospace,
    fontFeatureSettings = "tnum",
)

/** Subheadline / menu text: 15 sp regular. */
val BoostSubheadline = TextStyle(fontSize = 15.sp, lineHeight = 20.sp)

val BoostCardShape = RoundedCornerShape(16.dp)

private val BoostTypography = androidx.compose.material3.Typography(
    titleLarge = BoostSectionTitle,
    bodyLarge = BoostMetric,
    bodyMedium = BoostMetric,
    bodySmall = BoostFootnote,
    labelLarge = BoostMetric,
    labelMedium = BoostCaptionSemibold,
    labelSmall = BoostCaption,
)

@Composable
fun BoostGaugeTheme(content: @Composable () -> Unit) {
    val darkTheme = isSystemInDarkTheme()
    MaterialTheme(
        colorScheme = if (darkTheme) BoostDarkColorScheme else BoostLightColorScheme,
        typography = BoostTypography,
        content = content,
    )
}
