package com.boostgauge.app.ui

import androidx.compose.foundation.layout.padding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ReceiptLong
import androidx.compose.material.icons.filled.MyLocation
import androidx.compose.material.icons.filled.Palette
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material.icons.filled.Speed
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.NavigationBarItemDefaults
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.dp
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.boostgauge.app.AppContainer
import com.boostgauge.app.ui.screens.CalibrationScreen
import com.boostgauge.app.ui.screens.DashboardScreen
import com.boostgauge.app.ui.screens.LogsScreen
import com.boostgauge.app.ui.screens.SettingsScreen
import com.boostgauge.app.ui.screens.ThemesScreen

private data class Destination(val route: String, val label: String, val icon: ImageVector)

private val destinations = listOf(
    Destination("status", "Status", Icons.Filled.Speed),
    Destination("themes", "Themes", Icons.Filled.Palette),
    Destination("settings", "Settings", Icons.Filled.Settings),
    Destination("calibration", "Calibrate", Icons.Filled.MyLocation),
    Destination("logs", "Logs", Icons.AutoMirrored.Filled.ReceiptLong),
)

@Composable
fun BoostGaugeApp(container: AppContainer) {
    BoostGaugeTheme {
        val navController = rememberNavController()
        val backStackEntry by navController.currentBackStackEntryAsState()
        val currentRoute = backStackEntry?.destination?.route
        Scaffold(
            bottomBar = {
                NavigationBar(
                    containerColor = MaterialTheme.colorScheme.surface,
                    tonalElevation = 3.dp,
                ) {
                    destinations.forEach { destination ->
                        NavigationBarItem(
                            selected = currentRoute == destination.route,
                            onClick = {
                                navController.navigate(destination.route) {
                                    popUpTo(navController.graph.findStartDestination().id) {
                                        saveState = true
                                    }
                                    launchSingleTop = true
                                    restoreState = true
                                }
                            },
                            icon = { Icon(destination.icon, contentDescription = destination.label) },
                            label = { Text(destination.label) },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = MaterialTheme.colorScheme.primary,
                                selectedTextColor = MaterialTheme.colorScheme.primary,
                                indicatorColor = MaterialTheme.colorScheme.primary.copy(alpha = 0.12f),
                                unselectedIconColor = MaterialTheme.colorScheme.onSurfaceVariant,
                                unselectedTextColor = MaterialTheme.colorScheme.onSurfaceVariant,
                            ),
                        )
                    }
                }
            },
        ) { innerPadding ->
            NavHost(
                navController = navController,
                startDestination = "status",
                modifier = Modifier.padding(innerPadding),
            ) {
                composable("status") { DashboardScreen(container) }
                composable("themes") { ThemesScreen(container) }
                composable("settings") { SettingsScreen(container) }
                composable("calibration") { CalibrationScreen(container) }
                composable("logs") { LogsScreen(container) }
            }
        }
    }
}
