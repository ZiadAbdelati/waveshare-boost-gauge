import Foundation
import XCTest
@testable import BoostGauge

final class ConnectionMonitorTests: XCTestCase {
    private final class Collector {
        private let lock = NSLock()
        private var items: [String] = []

        func add(_ item: String) {
            lock.lock()
            items.append(item)
            lock.unlock()
        }

        func all() -> [String] {
            lock.lock()
            defer { lock.unlock() }
            return items
        }
    }

    func testMonitorReportsConnectedOnSuccess() async throws {
        let transport = FakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let collector = Collector()
        let monitor = HTTPConnectionMonitor(
            transport: transport,
            intervals: .init(success: 0.01, failure: 0.01)
        ) { result in
            collector.add(HTTPConnectionMonitor.isSuccess(result) ? "connected" : "failed")
        }
        monitor.start()
        try await waitFor { collector.all().contains("connected") }
        monitor.stop()
        XCTAssertTrue(transport.recordedPaths.contains("state"))
    }

    func testMonitorReportsUnreachableWithErrorOnFailure() async throws {
        let transport = FakeTransport()
        let collector = Collector()
        let monitor = HTTPConnectionMonitor(
            transport: transport,
            intervals: .init(success: 0.01, failure: 0.01)
        ) { result in
            switch result {
            case .success:
                collector.add("connected")
            case .failure(let error):
                collector.add("unreachable:\(error.localizedDescription)")
            }
        }
        monitor.start()
        try await waitFor { collector.all().contains { $0.hasPrefix("unreachable:") } }
        let entries = collector.all()
        XCTAssertTrue(entries.contains { $0.hasPrefix("unreachable:") })
        monitor.stop()
    }

    func testMonitorNeverOverlapsRequests() async throws {
        let transport = ConcurrentFakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let monitor = HTTPConnectionMonitor(
            transport: transport,
            intervals: .init(success: 0.01, failure: 0.01)
        ) { _ in }
        monitor.start()
        try? await Task.sleep(nanoseconds: 300_000_000)
        monitor.stop()
        XCTAssertEqual(transport.maxActive, 1)
        XCTAssertGreaterThanOrEqual(transport.requestCount, 3)
    }

    func testBackoffPolicyUsesFailureIntervalAfterFailures() {
        let intervals = HTTPConnectionMonitor.Intervals(success: 1, failure: 3)
        XCTAssertEqual(HTTPConnectionMonitor.nextDelay(outcomeIsSuccess: true, intervals: intervals), 1)
        XCTAssertEqual(HTTPConnectionMonitor.nextDelay(outcomeIsSuccess: false, intervals: intervals), 3)
    }

    private func waitFor(_ condition: @escaping () -> Bool) async throws {
        let deadline = Date().addingTimeInterval(2)
        while Date() < deadline {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        XCTFail("Condition not met within 2 s")
    }
}

private final class ConcurrentFakeTransport: FakeTransport {
    private var active = 0
    private(set) var maxActive = 0
    private(set) var requestCount = 0

    override func get(_ path: String) async throws -> Resp {
        active += 1
        maxActive = max(maxActive, active)
        requestCount += 1
        defer { active -= 1 }
        try? await Task.sleep(nanoseconds: 40_000_000)
        return try await super.get(path)
    }
}

final class AppSessionTests: XCTestCase {
    private func makeDefaults() -> UserDefaults {
        let name = "BoostGaugeTests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    func testFirstRunHasNoConfiguredHost() {
        let session = AppSession(defaults: makeDefaults())
        XCTAssertNil(session.httpHost)
        XCTAssertNil(session.transport)
        XCTAssertEqual(session.connectionState, .notConfigured)
    }

    func testSetHTTPHostPersistsAndStartsConnecting() {
        let defaults = makeDefaults()
        let session = AppSession(defaults: defaults)
        XCTAssertTrue(session.setHTTPHost("http://127.0.0.1:18099"))
        XCTAssertEqual(session.kind, .http)
        XCTAssertEqual(session.httpHost, "http://127.0.0.1:18099")
        XCTAssertEqual(session.connectionState, .connecting)
        XCTAssertNotNil(session.transport)
        XCTAssertEqual(defaults.string(forKey: "transport.httpHost"), "http://127.0.0.1:18099")
    }

    func testSetHTTPHostRejectsSchemeLessHost() {
        let session = AppSession(defaults: makeDefaults())
        XCTAssertFalse(session.setHTTPHost("192.168.1.100"))
        XCTAssertNil(session.httpHost)
        XCTAssertNil(session.transport)
        XCTAssertEqual(session.connectionState, .notConfigured)
    }

    func testSavedHTTPHostStartsConnecting() {
        let defaults = makeDefaults()
        defaults.set(AppSession.Kind.http.rawValue, forKey: "transport.kind")
        defaults.set("http://127.0.0.1:9", forKey: "transport.httpHost")
        let session = AppSession(defaults: defaults)
        XCTAssertEqual(session.httpHost, "http://127.0.0.1:9")
        XCTAssertNotNil(session.transport)
        XCTAssertEqual(session.connectionState, .connecting)
    }

    func testBackToHTTPWithoutConfiguredHostIsNotConfigured() {
        let defaults = makeDefaults()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        let session = AppSession(defaults: defaults)
        session.backToHTTP()
        XCTAssertEqual(session.kind, .http)
        XCTAssertNil(session.httpHost)
        XCTAssertNil(session.transport)
        XCTAssertEqual(session.connectionState, .notConfigured)
    }
}
