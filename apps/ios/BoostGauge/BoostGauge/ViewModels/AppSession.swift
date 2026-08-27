import Foundation

final class AppSession: ObservableObject {
    enum Kind: String {
        case http
        case ble
    }

    static let defaultHost = "http://192.168.1.100"

    @Published private(set) var transport: GaugeTransport?
    @Published private(set) var kind: Kind
    @Published private(set) var httpHost: String?
    @Published private(set) var blePeerName: String?
    @Published private(set) var bleInfo: BleDeviceInfo?
    @Published private(set) var connectionState: GaugeConnectionState
    @Published private(set) var reconnectAttempt: Int?
    @Published private(set) var hardwareBleE2EStatus: String?
    @Published private(set) var hardwareBleE2ESteps: [String: String] = [:]

    let hardwareBleE2ERequested: Bool
    let simBleRequested: Bool

    var transportID: ObjectIdentifier? {
        transport.map { ObjectIdentifier($0) }
    }

    /// The HTTP host the app can reach the gauge's full HTTP API from. For
    /// HTTP transports this is the persisted `httpHost`; for BLE-only
    /// connections the firmware's device-info advertises its STA IP, which
    /// lets the app pull the log graph window (`GET /logs?limit=1500`, the
    /// last 5 minutes) that the capped BLE Log characteristic cannot carry.
    var effectiveHTTPHost: String? {
        if let httpHost { return httpHost }
        guard kind == .ble, let ip = bleInfo?.ip, !ip.isEmpty else { return nil }
        return "http://\(ip)"
    }

    var bleConnectionMessage: String? {
        guard kind == .ble else { return nil }
        if let attempt = reconnectAttempt {
            return Self.reconnectingMessage(attempt: attempt)
        }
        switch connectionState {
        case .connected: return "Connected to \(blePeerName ?? "gauge")"
        case .connecting: return "Connecting…"
        case .notConnected, .notConfigured, .unreachable: return "Not connected"
        }
    }

    /// Android's reconnect banner string, kept byte-identical for parity.
    static func reconnectingMessage(attempt: Int) -> String {
        "Reconnecting… (attempt \(attempt))"
    }

    /// The Saved-gauge row action per the cross-platform visibility matrix
    /// (PARITY.md row 1, 2026-08-26): the row is visible whenever a peer is
    /// remembered AND the link is not connected, hidden while connected. While
    /// the auto-reconnect loop is retrying, the row renders with no Connect
    /// button (the "Reconnecting… (attempt N)" pill is the single status
    /// string). Mirrors Android's `peerKnown && Disconnected` matrix plus the
    /// reconnecting row.
    enum SavedGaugeAction: Equatable {
        case hidden
        case connect
        case reconnecting
    }

    static func savedGaugeAction(connectionState: GaugeConnectionState, reconnectAttempt: Int?) -> SavedGaugeAction {
        switch connectionState {
        case .connected:
            return .hidden
        case .connecting where reconnectAttempt != nil:
            return .reconnecting
        case .connecting, .notConnected, .notConfigured, .unreachable:
            return .connect
        }
    }

    private enum Keys {
        static let kind = "transport.kind"
        static let httpHost = "transport.httpHost"
        static let blePeerName = "transport.blePeerName"
        static let blePeerID = "transport.blePeerID"
    }

    private let defaults: UserDefaults
    private let bleTransportFactory: () -> any BLELinkTransport
    private var scanner: BleTransport?
    private var bleLinkTask: Task<Void, Never>?
    private var bleAutoReconnectTask: Task<Void, Never>?
    private var httpMonitor: HTTPConnectionMonitor?
    private var statusContinuation: AsyncStream<Result<Data, Error>>.Continuation?
    private var statusContinuationToken = 0
    private var hardwareBleE2EStarted = false

    init(defaults: UserDefaults = .standard,
         bleTransportFactory: (() -> any BLELinkTransport)? = nil) {
        self.defaults = defaults
        self.bleTransportFactory = bleTransportFactory ?? { BleTransport() }
        let arguments = ProcessInfo.processInfo.arguments
        // App-hosted unit tests launch the host app with the scheme's Run
        // launch arguments, so -e2eSimBle/-hardwareBleE2E would leak into the
        // test process and force the sim transport for every AppSession. The
        // E2E flags are for the interactive app only — ignore them under XCTest.
        let runningUnderXCTest = NSClassFromString("XCTestCase") != nil
        hardwareBleE2ERequested = arguments.contains("-hardwareBleE2E") && !runningUnderXCTest
        simBleRequested = arguments.contains("-e2eSimBle") && !runningUnderXCTest
        if let index = arguments.firstIndex(of: "-e2eHTTPURL"), index + 1 < arguments.count {
            defaults.set(Kind.http.rawValue, forKey: Keys.kind)
            defaults.set(arguments[index + 1], forKey: Keys.httpHost)
        }
        // UI-test seam for the "Saved gauge" row: seed a remembered BLE peer
        // (name + stable identifier) as if the user had connected before.
        if let index = arguments.firstIndex(of: "-e2eSavedGauge"), index + 1 < arguments.count {
            defaults.set(arguments[index + 1], forKey: Keys.blePeerName)
            defaults.set(UUID().uuidString, forKey: Keys.blePeerID)
        }
        if simBleRequested {
            // Simulator-only BLE simulation: no CoreBluetooth, no persisted
            // transport state — drives the exact BLE-mode UI (status stream,
            // themes editor, bounded log window over a dense /logs ring).
            kind = .ble
            httpHost = nil
            blePeerName = defaults.string(forKey: Keys.blePeerName) ?? "BoostGauge (sim)"
            bleInfo = BleDeviceInfo(name: "BoostGauge", firmware: "sim", api: 1, ip: nil)
            transport = SimBleTransport()
            connectionState = .connected
            return
        }
        let savedKind = Kind(rawValue: defaults.string(forKey: Keys.kind) ?? "") ?? .http
        var savedHost = defaults.string(forKey: Keys.httpHost)?.trimmingCharacters(in: .whitespacesAndNewlines)
        if savedHost?.isEmpty == true { savedHost = nil }
        kind = savedKind
        httpHost = savedHost
        blePeerName = defaults.string(forKey: Keys.blePeerName)
        if savedKind == .http {
            if let savedHost, let url = URL(string: savedHost), url.scheme != nil {
                transport = HttpTransport(baseURL: url)
                connectionState = .connecting
            } else {
                transport = nil
                connectionState = .notConfigured
            }
        } else {
            transport = nil
            connectionState = .notConnected
        }
    }

    @MainActor
    func startHardwareBleE2EIfRequested() async {
        guard hardwareBleE2ERequested, !hardwareBleE2EStarted else { return }
        hardwareBleE2EStarted = true
        hardwareBleE2EStatus = "RUNNING · scanning"

        let stepNames = [
            "getState", "getStatePayload", "getConfig", "getThemes", "getThemesPayload",
            "putPage0Start", "putPage1", "putPage0Restore",
            "putThemeNeon", "putThemeRestore",
            "readDeviceInfo", "readStatus", "readStatusPayload", "readLog",
        ]
        hardwareBleE2ESteps = Dictionary(uniqueKeysWithValues: stepNames.map { ($0, "PENDING") })

        func requireSuccess(_ response: Resp, _ label: String) throws {
            guard (200...299).contains(response.status) else {
                throw NSError(domain: "HardwareBleE2E", code: response.status,
                              userInfo: [NSLocalizedDescriptionKey: "\(label) returned HTTP \(response.status)"])
            }
        }

        func pass(_ step: String) {
            hardwareBleE2ESteps[step] = "PASS"
            hardwareBleE2EStatus = "RUNNING · \(step)"
        }

        do {
            let devices = try await scanBLE()
            guard let gauge = devices.first(where: {
                $0.name.localizedCaseInsensitiveContains("BoostGauge")
                    || $0.name.localizedCaseInsensitiveContains("Boost Gauge")
            }) ?? devices.first else {
                throw NSError(domain: "HardwareBleE2E", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "no gauge discovered"])
            }
            hardwareBleE2EStatus = "RUNNING · connecting \(gauge.name)"
            _ = try await connectBLE(to: gauge)
            guard let transport, let ble = transport as? BleTransport else {
                throw TransportError.notConnected
            }

            var response = try await transport.get("state")
            try requireSuccess(response, "GET /state")
            let state = try response.jsonObject()
            guard let wheels = (state["tpms"] as? [String: Any])?["wheels"] as? [Any],
                  wheels.count == 4,
                  state["sensors"] != nil,
                  state["display"] != nil,
                  let firmwareVersion = state["firmwareVersion"] as? String,
                  !firmwareVersion.isEmpty else {
                throw TransportError.badResponse
            }
            pass("getState")
            pass("getStatePayload")

            response = try await transport.get("config")
            try requireSuccess(response, "GET /config")
            _ = try response.jsonObject()
            pass("getConfig")

            response = try await transport.get("themes")
            try requireSuccess(response, "GET /themes")
            let themes = try response.jsonObject()
            guard let originalTheme = themes["activeThemeId"] as? String, !originalTheme.isEmpty,
                  let themeArray = themes["themes"] as? [Any],
                  let firstTheme = themeArray.first as? [String: Any],
                  let colors = firstTheme["colors"] as? [String: Any],
                  colors["face"] != nil, colors["track"] != nil, colors["text"] != nil,
                  colors["muted"] != nil, colors["vacuum"] != nil, colors["boost"] != nil,
                  colors["overboost"] != nil, colors["zero"] != nil,
                  themes["bigDigitStaticColor"] != nil,
                  themes["neonFont"] != nil,
                  themes["vaultVignette"] != nil,
                  themes["pixelShiftSec"] != nil else {
                throw TransportError.badResponse
            }
            pass("getThemes")
            pass("getThemesPayload")

            response = try await transport.send("PUT", path: "page", body: ["page": 0])
            try requireSuccess(response, "PUT /page 0")
            pass("putPage0Start")

            response = try await transport.send("PUT", path: "page", body: ["page": 1])
            try requireSuccess(response, "PUT /page 1")
            pass("putPage1")

            response = try await transport.send("PUT", path: "page", body: ["page": 0])
            try requireSuccess(response, "PUT /page restore 0")
            pass("putPage0Restore")

            response = try await transport.send("PUT", path: "themes/active", body: ["id": "neon"])
            try requireSuccess(response, "PUT /themes/active neon")
            pass("putThemeNeon")

            response = try await transport.send("PUT", path: "themes/active", body: ["id": originalTheme])
            try requireSuccess(response, "PUT /themes/active restore")
            pass("putThemeRestore")

            let info = try await ble.readDeviceInfo()
            guard !info.name.isEmpty else { throw TransportError.badResponse }
            var ip = info.ip
            for _ in 0..<10 where ip == nil || ip!.isEmpty {
                try await Task.sleep(nanoseconds: 2_000_000_000)
                ip = try await ble.readDeviceInfo().ip
            }
            guard let ip, !ip.isEmpty else {
                throw NSError(domain: "HardwareBleE2E", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "device-info lacks STA ip"])
            }
            bleInfo = BleDeviceInfo(name: info.name, firmware: info.firmware, api: info.api, ip: ip)
            pass("readDeviceInfo")
            pass("readDeviceInfoIP")

            let status = try await ble.readStatus(forceRead: true)
            guard !status.isEmpty else { throw TransportError.badResponse }
            let statusObject: [String: Any]
            do {
                guard let object = try JSONSerialization.jsonObject(with: status) as? [String: Any] else {
                    throw TransportError.badResponse
                }
                statusObject = object
            } catch {
                let preview = String(data: status.prefix(120), encoding: .utf8) ?? "<binary>"
                throw NSError(domain: "HardwareBleE2E", code: 2,
                              userInfo: [NSLocalizedDescriptionKey: "Status malformed (\(status.count) B): \(preview)"])
            }
            guard statusObject["tpms"] != nil else { throw TransportError.badResponse }
            pass("readStatus")
            pass("readStatusPayload")

            _ = try await ble.readLog() // Parser requires the firmware's BGL1 header.
            pass("readLog")

            hardwareBleE2EStatus = "PASS · full BLE matrix"
        } catch {
            let phase = hardwareBleE2EStatus ?? "RUNNING · unknown"
            hardwareBleE2EStatus = "FAIL · \(phase) · \(error.localizedDescription)"
        }
    }

    deinit {
        httpMonitor?.stop()
        bleLinkTask?.cancel()
        bleAutoReconnectTask?.cancel()
    }

    func setHTTPHost(_ host: String) -> Bool {
        let trimmed = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), url.scheme != nil else { return false }
        httpMonitor?.stop()
        bleAutoReconnectTask?.cancel()
        reconnectAttempt = nil
        transport?.disconnect()
        transport = HttpTransport(baseURL: url)
        kind = .http
        httpHost = trimmed
        blePeerName = nil
        bleInfo = nil
        connectionState = .connecting
        defaults.set(Kind.http.rawValue, forKey: Keys.kind)
        defaults.set(trimmed, forKey: Keys.httpHost)
        startHTTPMonitoring()
        return true
    }

    /// The BLE connect path publishes `transport`/`connectionState` AFTER the
    /// transport's continuation resumes on the BLE queue, so it must be main
    /// actor-isolated or SwiftUI logs "Publishing changes from background
    /// threads" (observed 13x attributed to the TabView's navigation state host
    /// during reconnect churn). Callers are SettingsView's `@MainActor` tasks.
    @MainActor
    func scanBLE() async throws -> [BleDevice] {
        bleAutoReconnectTask?.cancel()
        scanner?.disconnect()
        let newScanner = BleTransport()
        scanner = newScanner
        return authoritativeBLEDevices(try await newScanner.scan())
    }

    func authoritativeBLEDevices(_ discovered: [BleDevice]) -> [BleDevice] {
        var byID = Dictionary(discovered.map { ($0.identifier, $0) }, uniquingKeysWith: { first, _ in first })
        if let text = defaults.string(forKey: Keys.blePeerID), let id = UUID(uuidString: text), byID[id] == nil {
            byID[id] = BleDevice(identifier: id, name: blePeerName ?? "BoostGauge", rssi: 0)
        }
        return byID.values.sorted { lhs, rhs in
            if lhs.rssi == rhs.rssi { return lhs.name < rhs.name }
            return lhs.rssi > rhs.rssi
        }
    }

    @MainActor
    func connectBLE(to device: BleDevice) async throws -> BleDeviceInfo? {
        guard let scanner else { throw TransportError.notConnected }
        httpMonitor?.stop()
        bleAutoReconnectTask?.cancel()
        bleAutoReconnectTask = nil
        reconnectAttempt = nil
        stopBleLinkMonitoring()
        transport?.disconnect()
        connectionState = .connecting
        do {
            do {
                try await scanner.connect(to: device)
            } catch TransportError.notConnected {
                let saved = defaults.string(forKey: Keys.blePeerID).flatMap(UUID.init(uuidString:))
                guard saved == device.identifier else { throw TransportError.notConnected }
                try await scanner.connect(toSavedIdentifier: device.identifier, name: device.name)
            }
        } catch {
            connectionState = .notConnected
            throw error
        }
        var info: BleDeviceInfo?
        do {
            info = try await scanner.readDeviceInfo()
        } catch {
            info = nil
        }
        transport = scanner
        self.scanner = nil
        kind = .ble
        blePeerName = device.name
        bleInfo = info
        connectionState = .connected
        defaults.set(Kind.ble.rawValue, forKey: Keys.kind)
        defaults.set(device.name, forKey: Keys.blePeerName)
        defaults.set(device.identifier.uuidString, forKey: Keys.blePeerID)
        startBleLinkMonitoring()
        return info
    }

    /// Connect the remembered peer from the Saved-gauge row without requiring
    /// a fresh scan (CoreBluetooth's `retrievePeripherals` path). Mirrors
    /// Android's `onConnectSaved` → reconnect-to-address.
    @MainActor
    func connectToSavedGauge() async throws -> BleDeviceInfo? {
        guard let idText = defaults.string(forKey: Keys.blePeerID),
              let identifier = UUID(uuidString: idText) else {
            throw TransportError.deviceNotFound
        }
        let name = blePeerName ?? "BoostGauge"
        httpMonitor?.stop()
        bleAutoReconnectTask?.cancel()
        bleAutoReconnectTask = nil
        reconnectAttempt = nil
        stopBleLinkMonitoring()
        transport?.disconnect()
        connectionState = .connecting
        let candidate = bleTransportFactory()
        do {
            try await candidate.connect(toSavedIdentifier: identifier, name: name)
        } catch {
            connectionState = .notConnected
            throw error
        }
        var info: BleDeviceInfo?
        do {
            info = try await candidate.readDeviceInfo()
        } catch {
            info = nil
        }
        transport = candidate
        kind = .ble
        blePeerName = name
        bleInfo = info
        connectionState = .connected
        startBleLinkMonitoring()
        return info
    }

    /// Indefinite auto-reconnect with exponential backoff
    /// (1→2→5→10→30→60 s, capped, resetting on connect) — mirrors Android's
    /// GaugeRepository loop. Runs at launch when a BLE peer is persisted, and
    /// is re-entered on link loss so the link heals without user action.
    ///
    /// Single source of truth: a healthy GATT session (the transport's own
    /// `isConnected`, never the loop's bookkeeping) MUST surface as `.connected`
    /// and end the loop immediately. That holds on three paths: the loop-top
    /// health guard (a live link stops the loop within one tick, no backoff
    /// sleep consumed), a connect that resolves, and a connect that throws
    /// while the session actually came up. Link loss thereafter is reported by
    /// `linkStateStream`, which re-enters the loop. A cancelled loop must never
    /// publish over a newer connect.
    @MainActor
    private func startBLEReconnectLoop() {
        guard kind == .ble,
              transport == nil,
              bleAutoReconnectTask == nil,
              let idText = defaults.string(forKey: Keys.blePeerID),
              let identifier = UUID(uuidString: idText) else { return }
        let savedName = blePeerName ?? "BoostGauge"
        connectionState = .connecting
        reconnectAttempt = 1
        bleAutoReconnectTask = Task { [weak self] in
            // Every exit — success, cancellation, deinit — must clear the
            // stored reference or a cancelled loop blocks a future loop.
            defer { self?.bleAutoReconnectTask = nil }
            var attempt = 1
            var lastCandidate: (any BLELinkTransport)?
            while !Task.isCancelled {
                guard let self else { return }
                // Loop-top health guard: if a healthy link already exists (a
                // concurrent publish, a restored session, or a previous attempt
                // whose session actually came up), publish `.connected` and stop
                // within this tick — never keep counting attempts over a live
                // link (observed: "Reconnecting… (attempt 12)" while the board
                // reported the phone connected and wrote status for 18 min).
                if let healthy = self.transport as? any BLELinkTransport, healthy.isConnected {
                    self.adoptReconnected(healthy, fallbackName: savedName)
                    return
                }
                if let previous = lastCandidate, previous.isConnected {
                    self.adoptReconnected(previous, fallbackName: savedName)
                    return
                }
                // A genuinely dead previous attempt gets torn down before the
                // next connect; an in-progress one is left alone.
                lastCandidate?.disconnect()
                self.connectionState = .connecting
                self.reconnectAttempt = attempt
                try? await Task.sleep(nanoseconds: BLEBackoff.delayNs(forAttempt: attempt))
                guard !Task.isCancelled else { return }
                let candidate = self.bleTransportFactory()
                lastCandidate = candidate
                do {
                    try await candidate.connect(toSavedIdentifier: identifier, name: savedName)
                } catch {
                    // The transport is the source of truth, not connect()'s
                    // error: an established session must be adopted even when
                    // the connect threw (CoreBluetooth can complete a link
                    // while our pending timeout already fired).
                    guard !candidate.isConnected else {
                        self.adoptReconnected(candidate, fallbackName: savedName)
                        return
                    }
                    guard self.kind == .ble, !Task.isCancelled else { return }
                    attempt += 1
                    self.reconnectAttempt = attempt
                    continue
                }
                guard !Task.isCancelled, self.kind == .ble, self.transport == nil else {
                    candidate.disconnect()
                    return
                }
                self.adoptReconnected(candidate, fallbackName: savedName)
                return
            }
        }
    }

    /// Publish a reconnected transport as `.connected`, start link monitoring,
    /// and refresh the displayed name in the background (never gating the state
    /// transition on a slow `readDeviceInfo`).
    @MainActor
    private func adoptReconnected(_ candidate: any BLELinkTransport, fallbackName: String) {
        transport = candidate
        connectionState = .connected
        reconnectAttempt = nil
        startBleLinkMonitoring()
        Task { [weak self] in
            let info = try? await candidate.readDeviceInfo()
            await MainActor.run {
                guard let self, self.kind == .ble,
                      self.transportID == ObjectIdentifier(candidate) else { return }
                self.bleInfo = info
                self.blePeerName = info?.name ?? fallbackName
            }
        }
    }

    /// Release a dead link and hand off to the indefinite reconnect loop.
    @MainActor
    private func handleBleLinkLost() {
        guard kind == .ble, transport != nil else { return }
        stopBleLinkMonitoring()
        transport?.disconnect()
        transport = nil
        bleInfo = nil
        startBLEReconnectLoop()
    }

    /// Track the BLE link so `connectionState` reflects reality: link loss
    /// (gauge reboot, out of range) must flip the badge off "connected" and
    /// every screen's error text must match what writes will actually do.
    private func startBleLinkMonitoring() {
        bleLinkTask?.cancel()
        guard let ble = transport as? any BLELinkTransport else { return }
        let id = ObjectIdentifier(ble)
        let stream = ble.linkStateStream()
        bleLinkTask = Task { [weak self] in
            for await up in stream {
                guard !Task.isCancelled else { return }
                await MainActor.run { [weak self] in
                    guard let self, self.kind == .ble, self.transportID == id else { return }
                    if up {
                        self.connectionState = .connected
                    } else {
                        self.connectionState = .notConnected
                        self.handleBleLinkLost()
                    }
                }
            }
            // Stream finished without an explicit `false` (e.g. task
            // cancelled while backgrounded, deinit). Make the badge and
            // every "Not connected" banner agree.
            if !Task.isCancelled {
                await MainActor.run { [weak self] in
                    guard let self, self.kind == .ble, self.transportID == id,
                          self.connectionState == .connected else { return }
                    self.connectionState = .notConnected
                }
            }
        }
    }

    /// Call from scenePhase .active to heal a link that died while the app
    /// was suspended and CoreBluetooth delivered didDisconnect without a
    /// running listener.
    @MainActor
    func refreshBLELinkState() {
        guard kind == .ble, let ble = transport as? any BLELinkTransport else { return }
        if !ble.isConnected && connectionState == .connected {
            connectionState = .notConnected
            handleBleLinkLost()
        }
    }

    private func stopBleLinkMonitoring() {
        bleLinkTask?.cancel()
        bleLinkTask = nil
    }

    func disconnectBLE() {
        bleAutoReconnectTask?.cancel()
        bleAutoReconnectTask = nil
        stopBleLinkMonitoring()
        httpMonitor?.stop()
        transport?.disconnect()
        scanner?.disconnect()
        scanner = nil
        transport = nil
        bleInfo = nil
        reconnectAttempt = nil
        connectionState = .notConnected
    }

    func backToHTTP() {
        httpMonitor?.stop()
        disconnectBLE()
        kind = .http
        defaults.set(Kind.http.rawValue, forKey: Keys.kind)
        guard let host = httpHost, let url = URL(string: host), url.scheme != nil else {
            transport = nil
            connectionState = .notConfigured
            return
        }
        transport = HttpTransport(baseURL: url)
        connectionState = .connecting
        startHTTPMonitoring()
    }

    @MainActor
    func startConnectionMonitoring() {
        guard !hardwareBleE2ERequested, !simBleRequested else { return }
        if kind == .ble {
            startBLEReconnectLoop()
        } else {
            startHTTPMonitoring()
        }
    }

    func lastPeerID() -> String? {
        defaults.string(forKey: Keys.blePeerID)
    }

    /// Status data fed by the single HTTP connection probe. BLE uses the
    /// transport's notify stream instead, so this is only consumed for HTTP.
    func statusStream() -> AsyncStream<Result<Data, Error>> {
        statusContinuationToken += 1
        let token = statusContinuationToken
        statusContinuation?.finish()
        return AsyncStream { continuation in
            self.statusContinuation = continuation
            continuation.onTermination = { [weak self] _ in
                guard let self else { return }
                if self.statusContinuationToken == token {
                    self.statusContinuation = nil
                }
            }
        }
    }

    private func startHTTPMonitoring() {
        httpMonitor?.stop()
        guard kind == .http,
              let transport,
              let host = httpHost,
              let url = URL(string: host), url.scheme != nil else { return }
        let id = ObjectIdentifier(transport)
        let monitor = HTTPConnectionMonitor(transport: transport) { [weak self] result in
            let state: GaugeConnectionState
            if HTTPConnectionMonitor.isSuccess(result) {
                state = .connected
            } else if case .success(let response) = result {
                state = .unreachable("HTTP \(response.status)")
            } else if case .failure(let error) = result {
                state = .unreachable(error.localizedDescription)
            } else {
                state = .unreachable("Request failed")
            }
            await MainActor.run { [weak self] in
                guard let self, self.kind == .http, self.transportID == id else { return }
                self.connectionState = state
                switch result {
                case .success(let response):
                    self.statusContinuation?.yield(.success(response.body))
                case .failure(let error):
                    self.statusContinuation?.yield(.failure(error))
                }
            }
        }
        httpMonitor = monitor
        monitor.start()
    }
}
