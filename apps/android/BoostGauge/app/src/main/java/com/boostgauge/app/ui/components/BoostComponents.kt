package com.boostgauge.app.ui.components

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.boostgauge.app.ui.BoostCaption
import com.boostgauge.app.ui.BoostCaptionSemibold
import com.boostgauge.app.ui.BoostMetric
import com.boostgauge.app.ui.BoostMetricValue
import com.boostgauge.app.ui.BoostSectionTitle

/** iOS secondarySystemBackground-style major card: 16 dp radius, pale group surface. */
@Composable
fun BoostCard(
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(16.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    Surface(
        modifier = modifier.fillMaxWidth(),
        shape = RoundedCornerShape(16.dp),
        color = MaterialTheme.colorScheme.surfaceContainerLow,
    ) {
        Column(
            modifier = Modifier.padding(contentPadding),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = content,
        )
    }
}

/** iOS inset-grouped Form/List section header (uppercase caption over a pale group). */
@Composable
fun GroupedSectionHeader(title: String, modifier: Modifier = Modifier) {
    Text(
        text = title.uppercase(),
        style = BoostCaptionSemibold,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = modifier.padding(start = 4.dp),
    )
}

/** Pale rounded group surface used by Settings/Calibration/Themes grouped lists. */
@Composable
fun GroupedSection(
    title: String? = null,
    modifier: Modifier = Modifier,
    contentPadding: PaddingValues = PaddingValues(horizontal = 16.dp, vertical = 8.dp),
    content: @Composable ColumnScope.() -> Unit,
) {
    Column(modifier = modifier.fillMaxWidth(), verticalArrangement = Arrangement.spacedBy(6.dp)) {
        if (title != null) {
            GroupedSectionHeader(title)
        }
        Surface(
            shape = RoundedCornerShape(16.dp),
            color = MaterialTheme.colorScheme.surfaceContainerLow,
        ) {
            Column(
                modifier = Modifier.padding(contentPadding),
                verticalArrangement = Arrangement.spacedBy(4.dp),
                content = content,
            )
        }
    }
}

/** Section title inside a dashboard card (17 sp semibold). */
@Composable
fun BoostSectionTitleText(text: String, modifier: Modifier = Modifier) {
    Text(
        text = text,
        style = BoostSectionTitle,
        color = MaterialTheme.colorScheme.onSurface,
        modifier = modifier,
    )
}

/** Quiet label-leading / value-trailing metric row; values stay tabular. */
@Composable
fun MetricRow(
    label: String,
    value: String,
    modifier: Modifier = Modifier,
    valueColor: Color = MaterialTheme.colorScheme.onSurface,
    valueWeight: FontWeight = FontWeight.Normal,
) {
    Row(
        modifier = modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            style = BoostMetric,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = value,
            style = BoostMetricValue,
            color = valueColor,
            fontWeight = valueWeight,
        )
    }
}

/** Small colored presence/health dot. */
@Composable
fun PresenceDot(size: Dp, color: Color, modifier: Modifier = Modifier) {
    Box(
        modifier = modifier
            .size(size)
            .background(color, CircleShape),
    )
}

/** Caption-semibold health presence badge: dot + label (e.g. ADS / BMP). */
@Composable
fun PresenceBadge(label: String, present: Boolean, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier,
        horizontalArrangement = Arrangement.spacedBy(4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        PresenceDot(
            size = 8.dp,
            color = if (present) {
                com.boostgauge.app.ui.BoostColors.success
            } else {
                MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.6f)
            },
        )
        Text(
            text = label,
            style = BoostCaptionSemibold,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}

/** Fully rounded capsule pill used for zone/mode badges. */
@Composable
fun Pill(
    text: String,
    containerColor: Color,
    textColor: Color,
    modifier: Modifier = Modifier,
    horizontalPadding: Dp = 12.dp,
    verticalPadding: Dp = 4.dp,
    style: androidx.compose.ui.text.TextStyle = BoostCaptionSemibold,
) {
    Surface(color = containerColor, shape = CircleShape, modifier = modifier) {
        Text(
            text = text,
            style = style,
            color = textColor,
            modifier = Modifier.padding(horizontal = horizontalPadding, vertical = verticalPadding),
        )
    }
}

/** Small caption used for quiet helper text. */
@Composable
fun CaptionText(text: String, modifier: Modifier = Modifier, color: Color = MaterialTheme.colorScheme.onSurfaceVariant) {
    Text(text = text, style = BoostCaption, color = color, modifier = modifier)
}
