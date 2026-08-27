package com.boostgauge.app.data.transport

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.IOException
import java.util.concurrent.TimeUnit

/** Plain HTTP transport against the gauge's /api/v1 endpoints. */
class HttpTransport(baseUrl: String) : GaugeTransport {

    private val client = OkHttpClient.Builder()
        .connectTimeout(6, TimeUnit.SECONDS)
        .readTimeout(12, TimeUnit.SECONDS)
        .writeTimeout(12, TimeUnit.SECONDS)
        .build()

    private val baseUrl: String = run {
        val trimmed = baseUrl.trim().trimEnd('/')
        if (trimmed.isEmpty()) {
            throw IllegalArgumentException("HTTP host is empty — BLE-derived LAN ip required")
        } else if (trimmed.contains("://")) {
            trimmed
        } else {
            "http://$trimmed"
        }
    }

    override suspend fun get(path: String): Resp =
        execute(Request.Builder().url(url(path)).get().build())

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp {
        val request = Request.Builder()
            .url(url(path))
            .method(method.uppercase(), bodyJson?.toRequestBody(JSON))
            .build()
        return execute(request)
    }

    private fun url(path: String): String {
        val p = path.trimStart('/')
        return "$baseUrl/api/v1/$p"
    }

    private suspend fun execute(request: Request): Resp = withContext(Dispatchers.IO) {
        try {
            client.newCall(request).execute().use { response ->
                Resp(response.code, response.body?.string().orEmpty())
            }
        } catch (e: IOException) {
            throw TransportException("HTTP request failed: ${e.message}", e)
        }
    }

    companion object {
        private val JSON = "application/json; charset=utf-8".toMediaType()
    }
}
