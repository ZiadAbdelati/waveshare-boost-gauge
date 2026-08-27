package com.boostgauge.app.data.transport

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

/**
 * Controllable fake for BleGaugeTransport used in repository reconnect tests.
 * Lets tests drive connect success/failure and statusLine emissions.
 */
class FakeBleGaugeTransport(
    private val connectBehavior: suspend () -> Unit = {},
    private val handler: (method: String, path: String, bodyJson: String?) -> Resp = { _, _, _ -> Resp(404, "{}") },
) : BleGaugeTransport {

    override val transportKind: String = "BLE"

    private val _statusLine = MutableStateFlow<String?>(null)
    override val statusLine: StateFlow<String?> = _statusLine.asStateFlow()

    private val _linkUp = MutableStateFlow(false)
    override val linkUp: StateFlow<Boolean> = _linkUp.asStateFlow()

    var connectCalls = 0
        private set
    var closed = false
        private set

    data class Request(val method: String, val path: String, val bodyJson: String?)
    val requests = mutableListOf<Request>()

    override suspend fun connect() {
        connectCalls++
        try {
            connectBehavior()
            _linkUp.value = true
        } catch (t: Throwable) {
            _linkUp.value = false
            throw t
        }
    }

    override suspend fun get(path: String): Resp {
        requests += Request("GET", path, null)
        return handler("GET", path, null)
    }

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp {
        requests += Request(method, path, bodyJson)
        return handler(method, path, bodyJson)
    }

    override suspend fun readLog(): String = """BGL1
t_ms,psi,peak_psi,zone,demo
1000,1.0,1.0,BOOST,1
"""

    override suspend fun readStatus(): String = _statusLine.value ?: """{"psi":1.0}"""

    override suspend fun readDeviceInfo(): String = """{"name":"BoostGauge","api":1,"ip":"192.168.1.42"}"""

    override suspend fun close() { closed = true }

    fun emitStatus(json: String) { _statusLine.value = json }
}
