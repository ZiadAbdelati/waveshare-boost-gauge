package com.boostgauge.app.data.transport

/**
 * Stand-in for BleTransport used in JVM tests: implements the same
 * GaugeTransport contract and records the request frames it received.
 */
class FakeBleTransport(
    private val handler: (method: String, path: String, bodyJson: String?) -> Resp =
        { _, _, _ -> Resp(404, "{}") },
) : GaugeTransport {

    data class Request(val method: String, val path: String, val bodyJson: String?)

    val requests = mutableListOf<Request>()

    override suspend fun get(path: String): Resp {
        requests += Request("GET", path, null)
        return handler("GET", path, null)
    }

    override suspend fun send(method: String, path: String, bodyJson: String?): Resp {
        requests += Request(method, path, bodyJson)
        return handler(method, path, bodyJson)
    }
}
