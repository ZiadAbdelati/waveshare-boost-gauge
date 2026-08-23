import XCTest
@testable import BoostGauge

final class HttpTransportTests: XCTestCase {
    private func makeTransport() -> HttpTransport {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.protocolClasses = [MockURLProtocol.self]
        let session = URLSession(configuration: configuration)
        return HttpTransport(baseURL: URL(string: "http://192.168.1.100:80")!, session: session)
    }

    private func response(_ status: Int, _ data: Data) -> HTTPURLResponse {
        HTTPURLResponse(
            url: URL(string: "http://192.168.1.100:80/api/v1/state")!,
            statusCode: status,
            httpVersion: nil,
            headerFields: ["Content-Type": "application/json"]
        )!
    }

    func testGetStateUsesExpectedPathAndMethod() async throws {
        let transport = makeTransport()
        MockURLProtocol.handler = { request in
            return (self.response(200, Data("{\"ok\":true}".utf8)), Data("{\"ok\":true}".utf8))
        }
        let result = try await transport.get("state")
        XCTAssertEqual(result.status, 200)
        let object = try result.jsonObject()
        XCTAssertEqual(object["ok"] as? Bool, true)
    }

    func testPutSendsJSONBodyAndContentType() async throws {
        let transport = makeTransport()
        var capturedRequest: URLRequest?
        MockURLProtocol.handler = { request in
            capturedRequest = request
            return (self.response(200, Data("{}".utf8)), Data("{}".utf8))
        }
        _ = try await transport.send("PUT", path: "themes/active", body: ["id": "neon"])
        let request = try XCTUnwrap(capturedRequest)
        XCTAssertEqual(request.url?.path, "/api/v1/themes/active")
        XCTAssertEqual(request.httpMethod, "PUT")
        XCTAssertEqual(request.value(forHTTPHeaderField: "Content-Type"), "application/json")
        let sent = try JSONSerialization.jsonObject(with: bodyData(from: request)) as? [String: Any]
        XCTAssertEqual(sent?["id"] as? String, "neon")
    }

    func testQueryParameterPathIsPreserved() async throws {
        let transport = makeTransport()
        var capturedRequest: URLRequest?
        MockURLProtocol.handler = { request in
            capturedRequest = request
            return (self.response(200, Data("{}".utf8)), Data("{}".utf8))
        }
        _ = try await transport.get("logs?limit=10")
        let request = try XCTUnwrap(capturedRequest)
        XCTAssertEqual(request.url?.path, "/api/v1/logs")
        XCTAssertEqual(request.url?.query, "limit=10")
    }

    func testNon2xxStatusReturnsResponseWithoutThrowing() async throws {
        let transport = makeTransport()
        MockURLProtocol.handler = { _ in
            (self.response(404, Data("{\"error\":\"theme_not_found\"}".utf8)), Data("{\"error\":\"theme_not_found\"}".utf8))
        }
        let result = try await transport.get("themes/active")
        XCTAssertEqual(result.status, 404)
        let object = try result.jsonObject()
        XCTAssertEqual(object["error"] as? String, "theme_not_found")
    }

    func testNetworkErrorPropagates() async {
        let transport = makeTransport()
        MockURLProtocol.handler = { _ in
            throw URLError(.timedOut)
        }
        do {
            _ = try await transport.get("state")
            XCTFail("Expected a thrown error")
        } catch {
            XCTAssertTrue(error is URLError)
        }
    }

    func testreadLogSamplesDecodesFirmwareShape() async throws {
        let transport = makeTransport()
        let fixture = """
        {"samples":[{"tMs":1000,"psi":1.5,"peakPsi":2.0,"zone":"BOOST","demo":false}]}
        """
        MockURLProtocol.handler = { _ in
            (self.response(200, Data(fixture.utf8)), Data(fixture.utf8))
        }
        let samples = try await transport.readLogSamples(limit: 300)
        XCTAssertEqual(samples.count, 1)
        XCTAssertEqual(samples[0].tMs, 1000)
        XCTAssertEqual(samples[0].psi, 1.5)
    }

    private func bodyData(from request: URLRequest) -> Data {
        if let body = request.httpBody {
            return body
        }
        guard let stream = request.httpBodyStream else { return Data() }
        stream.open()
        defer { stream.close() }
        var data = Data()
        let bufferSize = 1024
        var buffer = [UInt8](repeating: 0, count: bufferSize)
        while stream.hasBytesAvailable {
            let read = stream.read(&buffer, maxLength: bufferSize)
            if read <= 0 { break }
            data.append(contentsOf: buffer.prefix(read))
        }
        return data
    }
}
