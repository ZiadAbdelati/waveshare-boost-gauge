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

    func testSavedPeerIsDeduplicatedByPeripheralIdentifier() {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Old name", forKey: "transport.blePeerName")
        let session = AppSession(defaults: defaults)
        let current = BleDevice(identifier: id, name: "BoostGauge Current", rssi: -61)
        let rows = session.authoritativeBLEDevices([current, current])
        XCTAssertEqual(rows, [current])
    }

    func testSavedPeerRemainsReconnectableWhenNotDiscovered() {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let session = AppSession(defaults: defaults)
        XCTAssertEqual(session.authoritativeBLEDevices([]), [
            BleDevice(identifier: id, name: "Saved Gauge", rssi: 0),
        ])
    }

    func testBLEConnectionPresentationIsMutuallyExclusive() {
        let defaults = makeDefaults()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let session = AppSession(defaults: defaults)
        XCTAssertEqual(session.bleConnectionMessage, "Not connected")
        XCTAssertFalse(session.bleConnectionMessage?.contains("Connected to") == true)
    }

    func testReconnectingMessageMatchesAndroidString() {
        XCTAssertEqual(AppSession.reconnectingMessage(attempt: 3), "Reconnecting… (attempt 3)")
        XCTAssertEqual(AppSession.reconnectingMessage(attempt: 1), "Reconnecting… (attempt 1)")
    }

    func testReconnectLoopPublishesConnectedAndStopsOnSuccess() async throws {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let fake = FakeBleLinkTransport(failsUntil: 1)
        let session = AppSession(defaults: defaults, bleTransportFactory: { fake })
        await session.startConnectionMonitoring()

        // Attempt 1 sleeps 1 s then fails; attempt 2 sleeps 2 s then connects.
        // The loop must publish .connected the moment the GATT link is up and
        // then stop — never keep counting attempts (BUG A: "Reconnecting… (attempt N)"
        // while the link was actually up).
        let deadline = Date().addingTimeInterval(12)
        while Date() < deadline && session.connectionState != .connected {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(session.connectionState, .connected, "loop must publish .connected on connect success")
        XCTAssertNil(session.reconnectAttempt, "no reconnect banner while connected")
        XCTAssertEqual(fake.connectCalls, 2, "one failed attempt then a successful connect")
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertEqual(fake.connectCalls, 2, "loop must cancel itself after success (no further attempts)")
    }

    func testLoopExitsWithinOneTickWhenLinkIsAlreadyHealthy() async throws {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        // Always-failing connects; attempt 2 blocks on the gate so the test can
        // mark the link healthy while the loop is mid-retry, then release it.
        let fake = FakeBleLinkTransport(failsUntil: 100, blockAt: 2)
        let session = AppSession(defaults: defaults, bleTransportFactory: { fake })
        await session.startConnectionMonitoring()

        var deadline = Date().addingTimeInterval(8)
        while Date() < deadline && fake.connectCalls < 1 {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertGreaterThanOrEqual(fake.connectCalls, 1, "attempt 1 ran")
        XCTAssertEqual(session.connectionState, .connecting)
        XCTAssertNotNil(session.reconnectAttempt, "loop is retrying")

        // Attempt 2 reaches the connect gate.
        deadline = Date().addingTimeInterval(8)
        while Date() < deadline && fake.connectCalls < 2 {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertEqual(fake.connectCalls, 2, "attempt 2 reached the gate")

        // The link is actually healthy while the loop still counts attempts.
        // The loop MUST surface .connected and stop within this tick instead of
        // escalating to attempt 3, 4, 12… (observed bug).
        fake.reportHealthy()
        fake.releaseBlockedConnect()
        deadline = Date().addingTimeInterval(3)
        while Date() < deadline && session.connectionState != .connected {
            try await Task.sleep(nanoseconds: 50_000_000)
        }
        XCTAssertEqual(session.connectionState, .connected, "healthy link must surface as .connected within one tick")
        XCTAssertNil(session.reconnectAttempt, "banner must clear once the link is up")
        try await Task.sleep(nanoseconds: 400_000_000)
        XCTAssertEqual(fake.connectCalls, 2, "loop stopped: no further connect attempts over a healthy link")
    }

    func testLinkLossReconnectsAndLoopStops() async throws {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let fake = FakeBleLinkTransport(failsUntil: 0)
        let session = AppSession(defaults: defaults, bleTransportFactory: { fake })
        await session.startConnectionMonitoring()

        let deadline = Date().addingTimeInterval(8)
        while Date() < deadline && session.connectionState != .connected {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(session.connectionState, .connected)
        XCTAssertEqual(fake.connectCalls, 1)

        // Simulated link loss: the transport's didDisconnect path publishes
        // false → the session must flip off .connected, re-enter the loop,
        // reconnect, and land back on .connected with the loop stopped.
        fake.simulateLinkLoss()
        let reconnectDeadline = Date().addingTimeInterval(8)
        while Date() < reconnectDeadline && !(session.connectionState == .connected && fake.connectCalls >= 2) {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(session.connectionState, .connected, "link loss must heal via the reconnect loop")
        XCTAssertNil(session.reconnectAttempt)
        let settledCount = fake.connectCalls
        XCTAssertGreaterThanOrEqual(settledCount, 2)
        try await Task.sleep(nanoseconds: 500_000_000)
        XCTAssertEqual(fake.connectCalls, settledCount, "loop must stop after the reconnect")
    }

    func testInfoPlistDeclaresBluetoothCentralBackgroundMode() {
        let modes = Bundle.main.object(forInfoDictionaryKey: "UIBackgroundModes") as? [String]
        XCTAssertTrue(modes?.contains("bluetooth-central") == true,
                      "Info.plist must declare UIBackgroundModes bluetooth-central so the BLE link survives backgrounding")
    }

    /// PARITY.md row 1 saved-gauge visibility matrix (2026-08-26): visible
    /// whenever a peer is remembered AND not connected, hidden while connected;
    /// while the reconnect loop retries, the row renders without a Connect
    /// button and the pill owns the "Reconnecting… (attempt N)" banner.
    func testSavedGaugeRowActionMatrix() {
        // Hidden while connected.
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .connected, reconnectAttempt: nil), .hidden)
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .connected, reconnectAttempt: 3), .hidden)
        // Reconnecting (peer remembered, link down, loop retrying): row, no Connect.
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .connecting, reconnectAttempt: 1), .reconnecting)
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .connecting, reconnectAttempt: 12), .reconnecting)
        // Truly down: row with a Connect action.
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .notConnected, reconnectAttempt: nil), .connect)
        // Manual connect (no attempt yet) offers Connect too.
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .connecting, reconnectAttempt: nil), .connect)
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .notConfigured, reconnectAttempt: nil), .connect)
        XCTAssertEqual(AppSession.savedGaugeAction(connectionState: .unreachable("err"), reconnectAttempt: nil), .connect)
    }

    /// Explicit user Disconnect (AUDIT 1): the transport must be torn down via
    /// `disconnect()`, the session fully reset to .notConnected, the reconnect
    /// loop cancelled (no further connect attempts over the dead link), and the
    /// peer STILL remembered so the Saved-gauge row can offer Connect again.
    func testExplicitDisconnectTearsDownTransportAndStopsReconnect() async throws {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let fake = FakeBleLinkTransport(failsUntil: 0)
        let session = AppSession(defaults: defaults, bleTransportFactory: { fake })
        await session.startConnectionMonitoring()

        let deadline = Date().addingTimeInterval(8)
        while Date() < deadline && session.connectionState != .connected {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(session.connectionState, .connected)
        XCTAssertEqual(fake.connectCalls, 1)

        await MainActor.run { session.disconnectBLE() }

        XCTAssertEqual(fake.disconnectCalls, 1, "explicit disconnect must tear down the transport")
        XCTAssertNil(session.transport, "transport must be released after disconnect")
        XCTAssertEqual(session.connectionState, .notConnected)
        XCTAssertNil(session.reconnectAttempt)
        XCTAssertEqual(defaults.string(forKey: "transport.blePeerID"), id.uuidString,
                       "the peer stays remembered for the Saved-gauge row")

        try await Task.sleep(nanoseconds: 700_000_000)
        XCTAssertEqual(fake.connectCalls, 1, "disconnect must cancel the reconnect loop — no further connects")
    }

    /// The Saved-gauge row's Connect action reconnects the remembered peer
    /// without a fresh scan, via CoreBluetooth's retrievePeripherals path
    /// (Android's onConnectSaved parity).
    func testConnectToSavedGaugeReconnectsRememberedPeer() async throws {
        let defaults = makeDefaults()
        let id = UUID()
        defaults.set(AppSession.Kind.ble.rawValue, forKey: "transport.kind")
        defaults.set(id.uuidString, forKey: "transport.blePeerID")
        defaults.set("Saved Gauge", forKey: "transport.blePeerName")
        let fake = FakeBleLinkTransport(failsUntil: 0)
        let session = AppSession(defaults: defaults, bleTransportFactory: { fake })

        let info = try await session.connectToSavedGauge()
        XCTAssertNotNil(info)
        XCTAssertEqual(session.connectionState, .connected)
        XCTAssertEqual(fake.connectCalls, 1)
        XCTAssertNil(session.reconnectAttempt)
        XCTAssertEqual(session.blePeerName, "Saved Gauge")
    }

    /// Zombie-link teardown: `disconnect()` MUST cancel the central connection
    /// whenever a peripheral is known, even when the app already believes the
    /// link is down (a connect timeout that raced a completed didConnect, or a
    /// prior cancel skipped under the old predicate). If cancel is skipped, the
    /// board keeps the link alive and never resumes advertising.
    func testBleTransportCancelsConnectionForAppForgottenLink() {
        XCTAssertTrue(BleTransport.shouldCancelConnection(hasPeripheral: true, isConnected: false, hasPendingConnect: false),
                      "a CoreBluetooth-held link the app has forgotten must still be cancelled")
        XCTAssertTrue(BleTransport.shouldCancelConnection(hasPeripheral: true, isConnected: true, hasPendingConnect: false))
        XCTAssertTrue(BleTransport.shouldCancelConnection(hasPeripheral: true, isConnected: false, hasPendingConnect: true))
        XCTAssertFalse(BleTransport.shouldCancelConnection(hasPeripheral: false, isConnected: false, hasPendingConnect: false))
    }
}

final class BLEBackoffTests: XCTestCase {
    func testBackoffProgressionMatchesAndroidReference() {
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 1), 1_000)
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 2), 2_000)
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 3), 5_000)
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 4), 10_000)
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 5), 30_000)
        // 6+ caps at 60 s, indefinitely.
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 6), 60_000)
        XCTAssertEqual(BLEBackoff.delayMs(forAttempt: 100), 60_000)
    }
}

/// In-process stand-in for `BLELinkTransport` so the reconnect loop can be
/// exercised without CoreBluetooth: connects fail while `connectCalls <=
/// failsUntil` (matching the real 15 s timeout path), and optionally block on
/// the `blockAt`-th connect so the test can drive the loop deterministically.
final class FakeBleLinkTransport: BLELinkTransport {
    let transportKind = "FAKEBLE"

    private let queue = DispatchQueue(label: "fake.blelink")
    private let failsUntil: Int
    private let blockAt: Int?
    private var connectGate: CheckedContinuation<Void, Never>?
    private var linkCont: AsyncStream<Bool>.Continuation?
    private(set) var connectCalls = 0
    private(set) var disconnectCalls = 0
    private(set) var isConnected = false

    init(failsUntil: Int, blockAt: Int? = nil) {
        self.failsUntil = failsUntil
        self.blockAt = blockAt
    }

    func connect(toSavedIdentifier identifier: UUID, name: String) async throws {
        connectCalls += 1
        if let blockAt, connectCalls == blockAt {
            await withCheckedContinuation { (gate: CheckedContinuation<Void, Never>) in
                self.connectGate = gate
            }
            self.connectGate = nil
        }
        guard connectCalls > failsUntil else { throw TransportError.connectTimeout }
        queue.async {
            self.isConnected = true
            self.linkCont?.yield(true)
        }
    }

    func releaseBlockedConnect() {
        queue.async { [weak self] in self?.connectGate?.resume() }
    }

    /// Marks the GATT session healthy without a connect — models a link that is
    /// actually up while the loop is still counting attempts.
    func reportHealthy() {
        queue.async { self.isConnected = true }
    }

    func readDeviceInfo() async throws -> BleDeviceInfo {
        BleDeviceInfo(name: "Saved Gauge", firmware: "sim", api: 1, ip: nil)
    }

    func linkStateStream() -> AsyncStream<Bool> {
        AsyncStream { continuation in
            queue.async {
                continuation.yield(self.isConnected)
                self.linkCont = continuation
            }
            continuation.onTermination = { [weak self] _ in
                self?.queue.async { self?.linkCont = nil }
            }
        }
    }

    /// Publishes the didDisconnect `false` the real transport would emit.
    func simulateLinkLoss() {
        queue.async {
            self.isConnected = false
            self.linkCont?.yield(false)
        }
    }

    // MARK: - GaugeTransport

    func get(_ path: String) async throws -> Resp { throw TransportError.notConnected }
    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        throw TransportError.notConnected
    }
    func readLogSamples(limit: Int) async throws -> [LogSample] { [] }
    func liveStatusStream() -> AsyncStream<Result<Data, Error>> { AsyncStream { $0.finish() } }
    func disconnect() {
        disconnectCalls += 1
        queue.async {
            let wasConnected = self.isConnected
            self.isConnected = false
            if wasConnected { self.linkCont?.yield(false) }
        }
    }
}
