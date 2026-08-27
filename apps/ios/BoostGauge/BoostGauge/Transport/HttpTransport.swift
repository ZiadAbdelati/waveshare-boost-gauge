import Foundation

final class HttpTransport: GaugeTransport {
    let transportKind = "HTTP"

    static let requestTimeout: TimeInterval = 5
    static let failurePollInterval: TimeInterval = 3

    private let baseURL: URL
    private let session: URLSession
    private let pollInterval: TimeInterval

    init(baseURL: URL, session: URLSession = .shared, pollInterval: TimeInterval = 1.0) {
        self.baseURL = baseURL
        self.session = session
        self.pollInterval = pollInterval
    }

    func get(_ path: String) async throws -> Resp {
        try await send("GET", path: path, body: [:])
    }

    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        let trimmed = baseURL.absoluteString.hasSuffix("/")
            ? String(baseURL.absoluteString.dropLast())
            : baseURL.absoluteString
        guard let url = URL(string: trimmed + "/api/v1/" + path) else {
            throw TransportError.invalidURL
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        request.timeoutInterval = Self.requestTimeout
        if method != "GET" && method != "DELETE" {
            request.setValue("application/json", forHTTPHeaderField: "Content-Type")
            request.httpBody = try JSONSerialization.data(withJSONObject: body)
        }
        let (data, response) = try await session.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        return Resp(status: status, body: data)
    }

    func readLogSamples(limit: Int) async throws -> [LogSample] {
        let response = try await get("logs?limit=\(limit)")
        let decoded = try JSONDecoder().decode(LogResponse.self, from: response.body)
        return decoded.samples
    }

    func liveStatusStream() -> AsyncStream<Result<Data, Error>> {
        AsyncStream { continuation in
            let task = Task { [weak self] in
                var consecutiveFailures = 0
                while !Task.isCancelled {
                    guard let self else { return }
                    do {
                        let response = try await self.get("state")
                        consecutiveFailures = 0
                        continuation.yield(.success(response.body))
                    } catch {
                        consecutiveFailures += 1
                        continuation.yield(.failure(error))
                    }
                    let delay = consecutiveFailures > 0 ? Self.failurePollInterval : self.pollInterval
                    try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    func disconnect() {}
}
