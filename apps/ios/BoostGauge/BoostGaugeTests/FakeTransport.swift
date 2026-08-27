import Foundation
@testable import BoostGauge

class FakeTransport: GaugeTransport {
    let transportKind = "FAKE"

    var responses: [String: Resp] = [:]
    var stream: AsyncStream<Result<Data, Error>>?
    var recordedMethods: [String] = []
    var recordedPaths: [String] = []
    var recordedBodies: [[String: Any]] = []

    func get(_ path: String) async throws -> Resp {
        recordedMethods.append("GET")
        recordedPaths.append(path)
        guard let response = responses[path] else {
            throw URLError(.resourceUnavailable)
        }
        return response
    }

    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        recordedMethods.append(method)
        recordedPaths.append(path)
        recordedBodies.append(body)
        guard let response = responses[path] else {
            throw URLError(.resourceUnavailable)
        }
        return response
    }

    func readLogSamples(limit: Int) async throws -> [LogSample] {
        let response = try await get("logs?limit=\(limit)")
        return try JSONDecoder().decode(LogResponse.self, from: response.body).samples
    }

    func liveStatusStream() -> AsyncStream<Result<Data, Error>> {
        stream ?? AsyncStream { $0.finish() }
    }

    func disconnect() {}

    static func resp(_ status: Int, _ object: [String: Any]) -> Resp {
        Resp(status: status, body: try! JSONSerialization.data(withJSONObject: object))
    }

    static func resp(_ status: Int, _ string: String) -> Resp {
        Resp(status: status, body: Data(string.utf8))
    }
}
