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

    var transportID: ObjectIdentifier? {
        transport.map { ObjectIdentifier($0) }
    }

    private enum Keys {
        static let kind = "transport.kind"
        static let httpHost = "transport.httpHost"
        static let blePeerName = "transport.blePeerName"
        static let blePeerID = "transport.blePeerID"
    }

    private let defaults: UserDefaults
    private var scanner: BleTransport?
    private var httpMonitor: HTTPConnectionMonitor?
    private var statusContinuation: AsyncStream<Result<Data, Error>>.Continuation?
    private var statusContinuationToken = 0

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let arguments = ProcessInfo.processInfo.arguments
        if let index = arguments.firstIndex(of: "-e2eHTTPURL"), index + 1 < arguments.count {
            defaults.set(Kind.http.rawValue, forKey: Keys.kind)
            defaults.set(arguments[index + 1], forKey: Keys.httpHost)
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

    deinit {
        httpMonitor?.stop()
    }

    func setHTTPHost(_ host: String) -> Bool {
        let trimmed = host.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed), url.scheme != nil else { return false }
        httpMonitor?.stop()
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

    func scanBLE() async throws -> [BleDevice] {
        scanner?.disconnect()
        let newScanner = BleTransport()
        scanner = newScanner
        return try await newScanner.scan()
    }

    func connectBLE(to device: BleDevice) async throws -> BleDeviceInfo? {
        guard let scanner else { throw TransportError.notConnected }
        httpMonitor?.stop()
        transport?.disconnect()
        try await scanner.connect(to: device)
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
        return info
    }

    func disconnectBLE() {
        httpMonitor?.stop()
        transport?.disconnect()
        transport = nil
        bleInfo = nil
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

    func startConnectionMonitoring() {
        startHTTPMonitoring()
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
