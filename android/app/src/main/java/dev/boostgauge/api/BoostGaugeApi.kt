package dev.boostgauge.api

import android.net.Uri
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import java.io.IOException
import java.util.concurrent.TimeUnit

class BoostGaugeApi(
    private val baseUrl: String,
    private val client: OkHttpClient = defaultClient,
    private val json: Json = apiJson
) {
    private val apiBase = normalizeBaseUrl(baseUrl).trimEnd('/') + "/api/v1"
    val dashboardUrl: String = normalizeBaseUrl(baseUrl)
    val logsCsvUri: Uri = Uri.parse("$apiBase/logs.csv")

    suspend fun state(): GaugeState = get("/state")
    suspend fun config(): GaugeConfig = get("/config")
    suspend fun themes(): ThemeCatalog = get("/themes")
    suspend fun logs(limit: Int = 120): LogResponse = get("/logs?limit=$limit")

    suspend fun updateConfig(config: GaugeConfig): GaugeConfig =
        put("/config", json.encodeToString(config))

    suspend fun syncTime(epochMs: Long, timezoneOffsetMinutes: Int): Unit =
        post("/time", json.encodeToString(TimeSyncRequest(epochMs, timezoneOffsetMinutes)))

    suspend fun setActiveTheme(id: String): Unit =
        put("/themes/active", json.encodeToString(ActiveThemeRequest(id)))

    suspend fun clearLogs(): Unit = delete("/logs")

    fun openEvents(listener: EventListener): WebSocket {
        val wsUrl = apiBase
            .replaceFirst("https://", "wss://")
            .replaceFirst("http://", "ws://") + "/events"
        val request = Request.Builder().url(wsUrl).build()
        return client.newWebSocket(request, object : WebSocketListener() {
            override fun onMessage(webSocket: WebSocket, text: String) {
                runCatching {
                    val payload = text.removePrefix("data:").trim()
                    json.decodeFromString<GaugeState>(payload)
                }.onSuccess(listener::onState)
                    .onFailure(listener::onFailure)
            }

            override fun onFailure(webSocket: WebSocket, t: Throwable, response: Response?) {
                listener.onFailure(t)
            }

            override fun onClosed(webSocket: WebSocket, code: Int, reason: String) {
                listener.onClosed()
            }
        })
    }

    private suspend inline fun <reified T> get(path: String): T = request("GET", path)
    private suspend inline fun <reified T> put(path: String, body: String): T = request("PUT", path, body)
    private suspend fun post(path: String, body: String) {
        request<Unit>("POST", path, body)
    }

    private suspend fun delete(path: String) {
        request<Unit>("DELETE", path)
    }

    private suspend inline fun <reified T> request(
        method: String,
        path: String,
        body: String? = null
    ): T = withContext(Dispatchers.IO) {
        val requestBody = body?.toRequestBody("application/json".toMediaType())
        val request = Request.Builder()
            .url(apiBase + path)
            .method(method, requestBody)
            .build()
        client.newCall(request).execute().use { response ->
            if (!response.isSuccessful) {
                throw IOException("HTTP ${response.code} ${response.message}")
            }
            if (T::class == Unit::class) Unit as T
            else json.decodeFromString(response.body?.string().orEmpty())
        }
    }

    interface EventListener {
        fun onState(state: GaugeState)
        fun onFailure(error: Throwable)
        fun onClosed()
    }

    companion object {
        val apiJson = Json {
            ignoreUnknownKeys = true
            explicitNulls = false
            encodeDefaults = true
        }

        private val defaultClient = OkHttpClient.Builder()
            .connectTimeout(4, TimeUnit.SECONDS)
            .readTimeout(12, TimeUnit.SECONDS)
            .build()

        fun normalizeBaseUrl(input: String): String {
            val trimmed = input.trim().trimEnd('/')
            if (trimmed.isEmpty()) return "http://boostgauge.local"
            return if (trimmed.startsWith("http://") || trimmed.startsWith("https://")) {
                trimmed
            } else {
                "http://$trimmed"
            }
        }
    }
}
