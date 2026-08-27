@preconcurrency import CoreBluetooth
import Foundation

struct BleDevice: Identifiable, Hashable {
    let identifier: UUID
    let name: String
    let rssi: Int
    var id: UUID { identifier }
}

struct BleDeviceInfo {
    let name: String
    let firmware: String
    let api: Int
    let ip: String?
}

struct BleFramedResponse {
    let id: UInt32
    let status: Int
    let body: Data
}

final class BleRequestFramer {
    private(set) var nextRequestID: UInt32 = 0
    private var buffer: [UInt8] = []

    func makeRequest(method: String, path: String, body: [String: Any]) throws -> (id: UInt32, data: Data) {
        nextRequestID = nextRequestID &+ 1
        let payload: [String: Any] = ["id": nextRequestID, "path": path, "method": method, "body": body]
        let data = try JSONSerialization.data(withJSONObject: payload)
        guard data.count <= 480 else { throw TransportError.requestTooLarge }
        return (nextRequestID, data)
    }

    func clearBuffer() {
        buffer.removeAll(keepingCapacity: false)
    }

    func append(_ chunk: Data) -> [BleFramedResponse] {
        buffer.append(contentsOf: chunk)
        var responses: [BleFramedResponse] = []
        while let object = extractFirstJSONObject(from: buffer) {
            buffer.removeFirst(object.count)
            if let response = parseResponse(object) {
                responses.append(response)
            }
        }
        return responses
    }

    private func parseResponse(_ data: Data) -> BleFramedResponse? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let id = object["id"] as? NSNumber,
              let status = object["status"] as? NSNumber else {
            return nil
        }
        var body = Data()
        if let bodyObject = object["body"] as? [String: Any],
           let encoded = try? JSONSerialization.data(withJSONObject: bodyObject) {
            body = encoded
        }
        return BleFramedResponse(id: id.uint32Value, status: status.intValue, body: body)
    }

    func extractFirstJSONObject(from bytes: [UInt8]) -> Data? {
        var depth = 0
        var startIndex: Int?
        var inString = false
        var escaped = false
        for (index, byte) in bytes.enumerated() {
            if inString {
                if escaped {
                    escaped = false
                } else if byte == 0x5C {
                    escaped = true
                } else if byte == 0x22 {
                    inString = false
                }
            } else {
                switch byte {
                case 0x7B:
                    if depth == 0 {
                        startIndex = index
                    }
                    depth += 1
                case 0x7D:
                    if depth > 0 {
                        depth -= 1
                        if depth == 0, let startIndex {
                            return Data(bytes[startIndex...index])
                        }
                    }
                case 0x22:
                    inString = true
                default:
                    break
                }
            }
        }
        return nil
    }
}

final class BleTransport: NSObject, BLELinkTransport, CBCentralManagerDelegate, CBPeripheralDelegate {
    let transportKind = "BLE"

    static let serviceUUID = CBUUID(string: "b6a00000-0000-4000-8000-00000000b6a0")
    static let controlUUID = CBUUID(string: "b6a00001-0000-4000-8000-00000000b6a0")
    static let statusUUID = CBUUID(string: "b6a00002-0000-4000-8000-00000000b6a0")
    static let logUUID = CBUUID(string: "b6a00003-0000-4000-8000-00000000b6a0")
    static let deviceInfoUUID = CBUUID(string: "b6a00004-0000-4000-8000-00000000b6a0")

    private let queue = DispatchQueue(label: "com.boostgauge.ble")
    private let framer = BleRequestFramer()
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var controlChar: CBCharacteristic?
    private var statusChar: CBCharacteristic?
    private var logChar: CBCharacteristic?
    private var deviceInfoChar: CBCharacteristic?
    private var scannedPeripherals: [UUID: CBPeripheral] = [:]
    private var scannedRSSI: [UUID: Int] = [:]
    private(set) var isConnected = false
    private var controlNotifyEnabled = false
    private var statusNotifyEnabled = false
    private var lastStatusData: Data?
    private var statusBuffer = Data()

    private var isPoweredOn = false
    private var powerCont: CheckedContinuation<Void, Error>?
    private var scanPending: ScanPending?
    private var connectPending: ConnectPending?
    private var readPending: ReadPending?
    private var requestPending: RequestPending?
    private var sendWaiters: [() -> Void] = []
    private var statusCont: AsyncStream<Result<Data, Error>>.Continuation?
    private var linkCont: AsyncStream<Bool>.Continuation?

    private struct ScanPending {
        let continuation: CheckedContinuation<[BleDevice], Error>
    }

    private final class ConnectPending {
        let continuation: CheckedContinuation<Void, Error>
        init(_ continuation: CheckedContinuation<Void, Error>) {
            self.continuation = continuation
        }
    }

    private final class ReadPending {
        let continuation: CheckedContinuation<Data, Error>
        let characteristic: CBCharacteristic
        var data = Data()
        init(_ continuation: CheckedContinuation<Data, Error>, characteristic: CBCharacteristic) {
            self.continuation = continuation
            self.characteristic = characteristic
        }
    }

    private final class RequestPending {
        let id: UInt32
        let continuation: CheckedContinuation<Resp, Error>
        init(id: UInt32, continuation: CheckedContinuation<Resp, Error>) {
            self.id = id
            self.continuation = continuation
        }
    }

    override init() {
        super.init()
        // State restoration keeps the link alive when the app is backgrounded
        // (UIBackgroundModes bluetooth-central) and lets the system relaunch
        // into a restore instead of losing the peer, mirroring Android's
        // foreground-service resilience with the iOS-native mechanism.
        let options: [String: Any] = [
            CBCentralManagerOptionRestoreIdentifierKey: "com.boostgauge.central",
        ]
        central = CBCentralManager(delegate: self, queue: queue, options: options)
    }

    deinit {
        if let peripheral {
            central?.cancelPeripheralConnection(peripheral)
        }
    }

    // MARK: - Public API

    func scan(duration: TimeInterval = 6) async throws -> [BleDevice] {
        try await waitForPower()
        return try await withCheckedThrowingContinuation { continuation in
            queue.async {
                guard self.scanPending == nil else {
                    continuation.resume(throwing: TransportError.busy)
                    return
                }
                self.scannedPeripherals.removeAll()
                self.scannedRSSI.removeAll()
                self.central.stopScan()
                self.scanPending = ScanPending(continuation: continuation)
                self.central.scanForPeripherals(
                    withServices: [Self.serviceUUID],
                    options: [CBCentralManagerScanOptionAllowDuplicatesKey: false]
                )
                let timeout = DispatchWorkItem { [weak self] in
                    self?.finishScan()
                }
                self.queue.asyncAfter(deadline: .now() + duration, execute: timeout)
            }
        }
    }

    func connect(to device: BleDevice) async throws {
        guard let peripheral = scannedPeripherals[device.identifier] else {
            throw TransportError.deviceNotFound
        }
        try await waitForPower()
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                guard self.connectPending == nil else {
                    continuation.resume(throwing: TransportError.busy)
                    return
                }
                self.peripheral = peripheral
                peripheral.delegate = self
                self.controlNotifyEnabled = false
                self.statusNotifyEnabled = false
                self.connectPending = ConnectPending(continuation)
                self.central.connect(peripheral, options: nil)
                let timeout = DispatchWorkItem { [weak self] in
                    self?.failConnectIfPending(TransportError.connectTimeout)
                }
                self.queue.asyncAfter(deadline: .now() + 15, execute: timeout)
            }
        }
    }

    /// Retrieve a previously connected CoreBluetooth peer by its stable UUID
    /// and reconnect without requiring the user to scan again.
    func connect(toSavedIdentifier identifier: UUID, name: String) async throws {
        try await waitForPower()
        let device = try await withCheckedThrowingContinuation {
            (continuation: CheckedContinuation<BleDevice, Error>) in
            queue.async {
                guard let peripheral = self.central.retrievePeripherals(withIdentifiers: [identifier]).first else {
                    continuation.resume(throwing: TransportError.deviceNotFound)
                    return
                }
                self.scannedPeripherals[identifier] = peripheral
                self.scannedRSSI[identifier] = 0
                continuation.resume(returning: BleDevice(
                    identifier: identifier,
                    name: peripheral.name ?? name,
                    rssi: 0
                ))
            }
        }
        try await connect(to: device)
    }

    /// Whether `disconnect()` must call `cancelPeripheralConnection`. Always
    /// true while a peripheral is known: CoreBluetooth can hold a connection the
    /// transport has already forgotten — a connect timeout that raced a
    /// completed `didConnect`, or a cancel skipped under the old
    /// (isConnected || connectPending) predicate — and that zombie link keeps
    /// the board from resuming advertising, so the saved-peer reconnect loop
    /// can never find it again.
    static func shouldCancelConnection(hasPeripheral: Bool, isConnected: Bool, hasPendingConnect: Bool) -> Bool {
        hasPeripheral
    }

    func disconnect() {
        queue.async { [weak self] in
            guard let self else { return }
            self.central.stopScan()
            let wasConnected = self.isConnected
            // Full teardown: cancel whenever a peripheral is known at all, even
            // if the app believes the link is already down (see
            // `shouldCancelConnection`). The board only resumes advertising
            // after the central's detach lands.
            if let peripheral = self.peripheral {
                self.central.cancelPeripheralConnection(peripheral)
            }
            self.isConnected = false
            self.controlNotifyEnabled = false
            self.statusNotifyEnabled = false
            // Fail every in-flight operation so no continuation hangs behind a
            // cancelled link (a stale 10 s request timer resuming a torn-down
            // session is a zombie writer). Send waiters are dropped first so
            // `drainSendWaiters` cannot re-queue a write onto the dead link.
            self.sendWaiters.removeAll()
            self.failConnectIfPending(TransportError.notConnected)
            self.failReadIfPending(TransportError.notConnected)
            self.failRequestIfPending(TransportError.notConnected)
            self.framer.clearBuffer()
            self.lastStatusData = nil
            self.statusBuffer.removeAll()
            if wasConnected {
                self.linkCont?.yield(false)
            }
        }
    }

    func readDeviceInfo() async throws -> BleDeviceInfo {
        guard let characteristic = deviceInfoChar else { throw TransportError.notConnected }
        let data = try await readValueQuiescingStatus(from: characteristic)
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw TransportError.badResponse
        }
        let name = object["name"] as? String ?? "BoostGauge"
        let api = (object["api"] as? NSNumber)?.intValue ?? 0
        var firmware = ""
        if let string = object["fw"] as? String {
            firmware = string
        } else if let number = object["fw"] as? NSNumber {
            firmware = number.stringValue
        }
        let ip = object["ip"] as? String
        return BleDeviceInfo(name: name, firmware: firmware, api: api, ip: ip)
    }

    func readStatus(forceRead: Bool = false) async throws -> Data {
        // The live status is the full Control /state (polled by
        // liveStatusStream). Return the latest polled sample; if none has
        // arrived yet, fetch it directly via the Control route.
        if let lastStatusData {
            return lastStatusData
        }
        let response = try await send("GET", path: "state", body: [:])
        guard (200...299).contains(response.status) else {
            throw TransportError.badResponse
        }
        let body = response.body
        queue.async { self.lastStatusData = body }
        return body
    }

    func readLog() async throws -> [LogSample] {
        guard let characteristic = logChar else { throw TransportError.notConnected }
        let data = try await readValue(from: characteristic, timeout: 15)
        return try Self.parseLogData(data)
    }

    func readLogSamples(limit: Int) async throws -> [LogSample] {
        try await readLog()
    }

    static func parseLogData(_ data: Data) throws -> [LogSample] {
        guard let header = "BGL1\n".data(using: .utf8), data.starts(with: header) else {
            throw TransportError.badLogFormat
        }
        guard let text = String(data: data.dropFirst(header.count), encoding: .utf8) else {
            throw TransportError.badLogFormat
        }
        var samples: [LogSample] = []
        for line in text.split(separator: "\n") {
            let parts = line.split(separator: ",").map { String($0).trimmingCharacters(in: .whitespaces) }
            if parts.count == 5, parts[0] == "t_ms" { continue }
            guard parts.count >= 5,
                  let tMs = Int64(parts[0]),
                  let psi = Double(parts[1]) else { continue }
            samples.append(LogSample(
                tMs: tMs,
                epochTs: nil,
                psi: psi,
                peakPsi: Double(parts[2]),
                zone: parts[3],
                demo: parts[4] == "1"
            ))
        }
        return samples
    }

    func get(_ path: String) async throws -> Resp {
        try await send("GET", path: path, body: [:])
    }

    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        do {
            return try await sendOnce(method, path: path, body: body)
        } catch let error as TransportError {
            guard error == .writeFailed || error == .operationTimeout || error == .busy else { throw error }
            let retryBackoffNs: [UInt64] = [200_000_000, 800_000_000, 1_500_000_000, 2_500_000_000]
            var lastError: Error = error
            for delayNs in retryBackoffNs {
                NSLog("[BLE] send %@ %@ failed %@, retry in %.1fs (busy/writeFail share queue)", method, path, String(describing: lastError), Double(delayNs)/1e9)
                if lastError as? TransportError == .writeFailed || lastError as? TransportError == .operationTimeout {
                    resubscribeNotifications()
                }
                try? await Task.sleep(nanoseconds: delayNs)
                do {
                    return try await sendOnce(method, path: path, body: body)
                } catch let retryError as TransportError {
                    guard retryError == .writeFailed || retryError == .operationTimeout || retryError == .busy else { throw retryError }
                    lastError = retryError
                }
            }
            throw lastError
        }
    }

    private func resubscribeNotifications() {
        queue.async { [weak self] in
            guard let self, let peripheral = self.peripheral,
                  self.canIssueATT(peripheral, "resubscribe control notify") else { return }
            if let controlChar = self.controlChar {
                peripheral.setNotifyValue(true, for: controlChar)
            }
            // Status characteristic deliberately not resubscribed: the live
            // status stream polls the Control /state route instead (the
            // full-state notification push starves raw reads on this link).
        }
    }

    private func sendOnce(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        guard let _ = peripheral, let _ = controlChar, isConnected else {
            throw TransportError.notConnected
        }
        let wirePath = path.hasPrefix("/") ? path : "/" + path
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Resp, Error>) in
            queue.async {
                self.enqueueSend(method: method, path: wirePath, body: body, continuation: continuation)
            }
        }
    }

    private func enqueueSend(method: String, path: String, body: [String: Any], continuation: CheckedContinuation<Resp, Error>) {
        if requestPending != nil {
            sendWaiters.append { [weak self] in
                guard let self else {
                    continuation.resume(throwing: TransportError.notConnected)
                    return
                }
                self.performSendOnce(method: method, path: path, body: body, continuation: continuation)
            }
            return
        }
        performSendOnce(method: method, path: path, body: body, continuation: continuation)
    }

    private func performSendOnce(method: String, path: String, body: [String: Any], continuation: CheckedContinuation<Resp, Error>) {
        guard let peripheral, let controlChar else {
            continuation.resume(throwing: TransportError.notConnected)
            return
        }
        guard canIssueATT(peripheral, "control write") else {
            // Link dropped between connect and write: the request can never be
            // issued, so the continuation must fail NOW — returning without
            // resuming (or arming the timeout) hangs send() forever and, via
            // enqueueSend's drain gate, every later request behind it.
            continuation.resume(throwing: TransportError.notConnected)
            return
        }
        let request: (id: UInt32, data: Data)
        do {
            request = try framer.makeRequest(method: method, path: path, body: body)
        } catch {
            continuation.resume(throwing: error)
            return
        }
        framer.clearBuffer()
        requestPending = RequestPending(id: request.id, continuation: continuation)
        peripheral.writeValue(request.data, for: controlChar, type: .withResponse)
        let timeout = DispatchWorkItem { [weak self] in
            self?.failRequestIfPending(TransportError.operationTimeout)
        }
        queue.asyncAfter(deadline: .now() + 10, execute: timeout)
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
                        self.queue.async { self.lastStatusData = response.body }
                        continuation.yield(.success(response.body))
                    } catch {
                        consecutiveFailures += 1
                        continuation.yield(.failure(error))
                    }
                    let delay = consecutiveFailures > 0 ? Self.failurePollInterval : Self.pollInterval
                    try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    private static let pollInterval: TimeInterval = 1.0
    private static let failurePollInterval: TimeInterval = 3.0

    /// Yields true when the GATT session is up (characteristics discovered),
    /// false on link loss or explicit disconnect. Lets the session keep
    /// `connectionState` honest for BLE — link loss is otherwise invisible.
    func linkStateStream() -> AsyncStream<Bool> {
        AsyncStream { continuation in
            queue.async {
                continuation.yield(self.isConnected)
                self.linkCont = continuation
            }
            continuation.onTermination = { [weak self] _ in
                self?.queue.async {
                    self?.linkCont = nil
                }
            }
        }
    }

    // MARK: - Internal helpers

    private func waitForPower() async throws {
        if isPoweredOn { return }
        if central.state == .poweredOff || central.state == .unsupported || central.state == .unauthorized {
            throw TransportError.bluetoothUnavailable
        }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            queue.async {
                if self.isPoweredOn {
                    continuation.resume()
                    return
                }
                guard self.powerCont == nil else {
                    continuation.resume(throwing: TransportError.busy)
                    return
                }
                self.powerCont = continuation
                let timeout = DispatchWorkItem {
                    if let pending = self.powerCont {
                        self.powerCont = nil
                        pending.resume(throwing: TransportError.bluetoothUnavailable)
                    }
                }
                self.queue.asyncAfter(deadline: .now() + 10, execute: timeout)
            }
        }
    }

    private func readValue(from characteristic: CBCharacteristic, timeout: TimeInterval = 10) async throws -> Data {
        guard let peripheral else { throw TransportError.notConnected }
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Data, Error>) in
            queue.async {
                guard self.readPending == nil else {
                    continuation.resume(throwing: TransportError.busy)
                    return
                }
                guard self.canIssueATT(peripheral, "read \(characteristic.uuid.uuidString)") else {
                    // Same orphan-suspension hazard as the control-write guard:
                    // fail fast so the caller's timeout/retry path owns recovery.
                    continuation.resume(throwing: TransportError.notConnected)
                    return
                }
                self.readPending = ReadPending(continuation, characteristic: characteristic)
                peripheral.readValue(for: characteristic)
                let timeoutItem = DispatchWorkItem { [weak self] in
                    self?.failReadIfPending(TransportError.operationTimeout)
                }
                self.queue.asyncAfter(deadline: .now() + timeout, execute: timeoutItem)
            }
        }
    }

    /// A raw GATT read while the 1 Hz status stream is flooding the ATT path
    /// is starved on the firmware side (hardware-verified: readDeviceInfo timed
    /// out deterministically while the full-state status fragments streamed,
    /// and passed as soon as the status push was disabled). Quiesce status
    /// notifications around the read, mirroring the readStatus pattern that
    /// the firmware/companion BLE gate already validated.
    private func readValueQuiescingStatus(from characteristic: CBCharacteristic, timeout: TimeInterval = 10) async throws -> Data {
        guard let peripheral, let statusChar, statusNotifyEnabled else {
            return try await readValue(from: characteristic, timeout: timeout)
        }
        queue.async {
            guard self.canIssueATT(peripheral, "quiesce status notify off") else { return }
            peripheral.setNotifyValue(false, for: statusChar)
            // Any fragment already buffered belongs to a sample the firmware
            // stops mid-flight; drop it so the next sample reassembles clean
            // (otherwise the stale tail merges with the new sample and yields
            // a malformed object).
            self.statusBuffer.removeAll()
        }
        try await Task.sleep(nanoseconds: 300_000_000)
        do {
            let value = try await readValue(from: characteristic, timeout: timeout)
            queue.async {
                guard self.canIssueATT(peripheral, "restore status notify") else { return }
                peripheral.setNotifyValue(true, for: statusChar)
            }
            return value
        } catch {
            queue.async {
                guard self.canIssueATT(peripheral, "restore status notify") else { return }
                peripheral.setNotifyValue(true, for: statusChar)
            }
            throw error
        }
    }

    private func ensureStatusNotify() {
        guard let statusChar, let peripheral, !statusNotifyEnabled,
              canIssueATT(peripheral, "status notify") else { return }
        peripheral.setNotifyValue(true, for: statusChar)
    }

    private func finishScan() {
        guard let pending = scanPending else { return }
        scanPending = nil
        central.stopScan()
        let discovered = scannedPeripherals.values.map { peripheral in
            BleDevice(
                identifier: peripheral.identifier,
                name: peripheral.name ?? "Boost Gauge",
                rssi: scannedRSSI[peripheral.identifier] ?? 0
            )
        }
        pending.continuation.resume(returning: discovered.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending })
    }

    private func failConnectIfPending(_ error: Error) {
        guard let pending = connectPending else { return }
        connectPending = nil
        if let peripheral {
            central.cancelPeripheralConnection(peripheral)
        }
        pending.continuation.resume(throwing: error)
    }

    private func finishConnect() {
        guard let pending = connectPending else { return }
        connectPending = nil
        isConnected = true
        linkCont?.yield(true)
        pending.continuation.resume()
    }

    private func failReadIfPending(_ error: Error) {
        guard let pending = readPending else { return }
        readPending = nil
        pending.continuation.resume(throwing: error)
    }

    private func finishRead() {
        guard let pending = readPending else { return }
        readPending = nil
        pending.continuation.resume(returning: pending.data)
    }

    private func failRequestIfPending(_ error: Error) {
        guard let pending = requestPending else { return }
        requestPending = nil
        pending.continuation.resume(throwing: error)
        drainSendWaiters()
    }

    private func finishRequest(_ response: Resp) {
        guard let pending = requestPending else { return }
        requestPending = nil
        pending.continuation.resume(returning: response)
        drainSendWaiters()
    }

    private func drainSendWaiters() {
        guard requestPending == nil, !sendWaiters.isEmpty else { return }
        let waiter = sendWaiters.removeFirst()
        waiter()
    }

    /// CoreBluetooth asserts "API misuse" if `writeValue`/`setNotifyValue`/
    /// `readValue` is issued on a peripheral that is not `.connected`
    /// (observed at the exact board power-down instant). Refuse the call and
    /// let the in-flight request fail through its normal timeout path instead
    /// of issuing the misuse on a dropped link.
    private func canIssueATT(_ peripheral: CBPeripheral, _ action: String) -> Bool {
        guard peripheral.state == .connected else {
            NSLog("[BLE] %@ skipped: peripheral state %ld != .connected", action, peripheral.state.rawValue)
            return false
        }
        return true
    }

    private func handleControlData(_ data: Data) {
        for response in framer.append(data) {
            if response.id == requestPending?.id {
                finishRequest(Resp(status: response.status, body: response.body))
            }
        }
    }

    // MARK: - CBCentralManagerDelegate

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        let restored = dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral] ?? []
        for peripheral in restored {
            scannedPeripherals[peripheral.identifier] = peripheral
            peripheral.delegate = self
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {        switch central.state {
        case .poweredOn:
            isPoweredOn = true
            if let pending = powerCont {
                powerCont = nil
                pending.resume()
            }
        case .poweredOff, .unsupported, .unauthorized:
            isPoweredOn = false
            if let pending = powerCont {
                powerCont = nil
                pending.resume(throwing: TransportError.bluetoothUnavailable)
            }
            failInFlightOps(TransportError.bluetoothUnavailable)
        case .unknown, .resetting:
            break
        @unknown default:
            break
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        scannedPeripherals[peripheral.identifier] = peripheral
        scannedRSSI[peripheral.identifier] = RSSI.intValue
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        if peripheral === self.peripheral {
            peripheral.discoverServices([Self.serviceUUID])
        }
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        if peripheral === self.peripheral {
            failConnectIfPending(error ?? TransportError.connectTimeout)
        }
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        if peripheral === self.peripheral {
            if connectPending != nil {
                failConnectIfPending(error ?? TransportError.notConnected)
            } else if isConnected {
                isConnected = false
                linkCont?.yield(false)
                statusCont?.yield(.failure(TransportError.notConnected))
            } else {
                linkCont?.yield(false)
            }
            failReadIfPending(error ?? TransportError.notConnected)
            failRequestIfPending(error ?? TransportError.notConnected)
        }
    }

    // MARK: - CBPeripheralDelegate

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else {
            failConnectIfPending(error ?? TransportError.serviceNotFound)
            return
        }
        guard let service = services.first(where: { $0.uuid == Self.serviceUUID }) else {
            failConnectIfPending(TransportError.serviceNotFound)
            return
        }
        peripheral.discoverCharacteristics(
            [Self.controlUUID, Self.statusUUID, Self.logUUID, Self.deviceInfoUUID],
            for: service
        )
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else {
            failConnectIfPending(error ?? TransportError.characteristicNotFound)
            return
        }
        controlChar = characteristics.first { $0.uuid == Self.controlUUID }
        statusChar = characteristics.first { $0.uuid == Self.statusUUID }
        logChar = characteristics.first { $0.uuid == Self.logUUID }
        deviceInfoChar = characteristics.first { $0.uuid == Self.deviceInfoUUID }
        guard controlChar != nil, statusChar != nil, logChar != nil, deviceInfoChar != nil else {
            failConnectIfPending(TransportError.characteristicNotFound)
            return
        }
        guard canIssueATT(peripheral, "control notify (connect)") else {
            failConnectIfPending(TransportError.notConnected)
            return
        }
        peripheral.setNotifyValue(true, for: controlChar!)
        // The Status characteristic is NOT subscribed: its full-state push is
        // ~5 ATT fragments/second, which hardware-verified starves raw GATT
        // reads on the shared path. The live status stream polls the Control
        // /state route instead (same full JSON, fragmented response that the
        // framer reassembles), exactly like the HTTP transport.
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic === controlChar {
            guard error == nil, characteristic.isNotifying else {
                failConnectIfPending(error ?? TransportError.notConnected)
                return
            }
            controlNotifyEnabled = true
            finishConnect()
        }
        if characteristic === statusChar && error == nil {
            statusNotifyEnabled = characteristic.isNotifying
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if error != nil {
            failRequestIfPending(TransportError.writeFailed)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic === statusChar {
            if let error {
                if !statusBuffer.isEmpty {
                    statusBuffer.removeAll(keepingCapacity: false)
                }
                statusCont?.yield(.failure(error))
                if readPending?.characteristic === characteristic {
                    failReadIfPending(error)
                }
            } else if let value = characteristic.value {
                statusBuffer.append(contentsOf: value)
                while let obj = framer.extractFirstJSONObject(from: [UInt8](statusBuffer)) {
                    statusBuffer.removeFirst(obj.count)
                    lastStatusData = obj
                    statusCont?.yield(.success(obj))
                    if let pending = readPending, pending.characteristic === characteristic {
                        pending.data = obj
                        finishRead()
                    }
                }
            }
            return
        }
        if characteristic === controlChar {
            if let value = characteristic.value {
                handleControlData(value)
            }
            return
        }
        guard let pending = readPending, pending.characteristic === characteristic else { return }
        if let error {
            failReadIfPending(error)
        } else {
            pending.data = characteristic.value ?? Data()
            finishRead()
        }
    }

    private func failInFlightOps(_ error: Error) {
        if let pending = scanPending {
            scanPending = nil
            pending.continuation.resume(throwing: error)
        }
        failConnectIfPending(error)
        failReadIfPending(error)
        failRequestIfPending(error)
    }
}
