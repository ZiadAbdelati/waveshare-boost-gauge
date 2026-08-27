package com.boostgauge.app.data.api

import kotlinx.serialization.json.Json

object ApiJson {
    val json = Json {
        ignoreUnknownKeys = true
        coerceInputValues = true
        isLenient = true
    }
}
