package com.boostgauge.app

import android.app.Application

class BoostGaugeApp : Application() {
    val container: AppContainer by lazy { AppContainer(this) }
}
