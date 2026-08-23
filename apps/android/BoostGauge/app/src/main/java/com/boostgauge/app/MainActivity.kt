package com.boostgauge.app

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.enableEdgeToEdge
import androidx.activity.compose.setContent
import androidx.lifecycle.lifecycleScope
import com.boostgauge.app.ui.BoostGaugeApp
import kotlinx.coroutines.launch

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val container = (application as BoostGaugeApp).container
        lifecycleScope.launch { container.initialize() }
        container.repository.start(lifecycleScope)
        setContent {
            BoostGaugeApp(container)
        }
    }
}
