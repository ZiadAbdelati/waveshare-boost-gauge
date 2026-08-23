package com.boostgauge.app.ui

import java.util.Locale

object Format {
    fun fmt(value: Double, decimals: Int = 2): String =
        String.format(Locale.US, "%.${decimals}f", value)

    fun formatUptime(uptimeMs: Long): String {
        val totalSeconds = uptimeMs / 1000
        val hours = totalSeconds / 3600
        val minutes = (totalSeconds % 3600) / 60
        val seconds = totalSeconds % 60
        return if (hours > 0) {
            String.format(Locale.US, "%dh %02dm %02ds", hours, minutes, seconds)
        } else {
            String.format(Locale.US, "%02d:%02d", minutes, seconds)
        }
    }

    fun formatMinutesToTime(minutes: Int): String {
        val h = ((minutes / 60) % 24).toString().padStart(2, '0')
        val m = (minutes % 60).toString().padStart(2, '0')
        return "$h:$m"
    }
}
