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

    private func extractFirstJSONObject(from bytes: [UInt8]) -> Data? {
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

final class BleTransport: NSObject, GaugeTransport, CBCentralManagerDelegate, CBPeripheralDelegate {
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
    private var isConnected = false
    private var statusNotifyEnabled = false
    private var lastStatusData: Data?

    private var isPoweredOn = false
    private var powerCont: CheckedContinuation<Void, Error>?
    private var scanPending: ScanPending?
    private var connectPending: ConnectPending?
    private var readPending: ReadPending?
    private var requestPending: RequestPending?
    private var statusCont: AsyncStream<Result<Data, Error>>.Continuation?

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
        central = CBCentralManager(delegate: self, queue: queue)
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
                self.connectPending = ConnectPending(continuation)
                self.central.connect(peripheral, options: nil)
                let timeout = DispatchWorkItem { [weak self] in
                    self?.failConnectIfPending(TransportError.connectTimeout)
                }
                self.queue.asyncAfter(deadline: .now() + 15, execute: timeout)
            }
        }
    }

    func disconnect() {
        queue.async { [weak self] in
            guard let self else { return }
            self.central.stopScan()
            if let peripheral = self.peripheral, self.isConnected || self.connectPending != nil {
                self.central.cancelPeripheralConnection(peripheral)
            }
            self.isConnected = false
        }
    }

    func readDeviceInfo() async throws -> BleDeviceInfo {
        guard let characteristic = deviceInfoChar else { throw TransportError.notConnected }
        let data = try await readValue(from: characteristic)
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
        return BleDeviceInfo(name: name, firmware: firmware, api: api)
    }

    func readStatus() async throws -> Data {
        if let lastStatusData {
            return lastStatusData
        }
        guard let characteristic = statusChar else { throw TransportError.notConnected }
        return try await readValue(from: characteristic)
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
        guard let peripheral, let controlChar, isConnected else {
            throw TransportError.notConnected
        }
        let wirePath = path.hasPrefix("/") ? path : "/" + path
        let request = try framer.makeRequest(method: method, path: wirePath, body: body)
        return try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Resp, Error>) in
            queue.async {
                guard self.requestPending == nil else {
                    continuation.resume(throwing: TransportError.busy)
                    return
                }
                self.framer.clearBuffer()
                self.requestPending = RequestPending(id: request.id, continuation: continuation)
                peripheral.writeValue(request.data, for: controlChar, type: .withResponse)
                let timeout = DispatchWorkItem { [weak self] in
                    self?.failRequestIfPending(TransportError.operationTimeout)
                }
                self.queue.asyncAfter(deadline: .now() + 10, execute: timeout)
            }
        }
    }

    func liveStatusStream() -> AsyncStream<Result<Data, Error>> {
        AsyncStream { continuation in
            queue.async {
                if let lastStatusData = self.lastStatusData {
                    continuation.yield(.success(lastStatusData))
                }
                self.statusCont = continuation
                self.ensureStatusNotify()
            }
            continuation.onTermination = { [weak self] _ in
                self?.queue.async {
                    self?.statusCont = nil
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
                self.readPending = ReadPending(continuation, characteristic: characteristic)
                peripheral.readValue(for: characteristic)
                let timeoutItem = DispatchWorkItem { [weak self] in
                    self?.failReadIfPending(TransportError.operationTimeout)
                }
                self.queue.asyncAfter(deadline: .now() + timeout, execute: timeoutItem)
            }
        }
    }

    private func ensureStatusNotify() {
        guard let statusChar, let peripheral, !statusNotifyEnabled else { return }
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
    }

    private func finishRequest(_ response: Resp) {
        guard let pending = requestPending else { return }
        requestPending = nil
        pending.continuation.resume(returning: response)
    }

    private func handleControlData(_ data: Data) {
        for response in framer.append(data) {
            if response.id == requestPending?.id {
                finishRequest(Resp(status: response.status, body: response.body))
            }
        }
    }

    // MARK: - CBCentralManagerDelegate

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
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
                statusCont?.yield(.failure(TransportError.notConnected))
            }
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
        peripheral.setNotifyValue(true, for: controlChar!)
        peripheral.setNotifyValue(true, for: statusChar!)
        finishConnect()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic === statusChar && error == nil && characteristic.isNotifying {
            statusNotifyEnabled = true
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
                statusCont?.yield(.failure(error))
            } else if let value = characteristic.value {
                lastStatusData = value
                statusCont?.yield(.success(value))
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
