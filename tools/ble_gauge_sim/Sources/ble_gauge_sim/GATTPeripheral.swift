import CoreBluetooth
import Foundation

/// CoreBluetooth peripheral for the simulated gauge.
///
/// Service b6a00000-0000-4000-8000-00000000b6a0 with:
///   - Control   b6a00001-...-b6a0: write + notify (JSON request/response)
///   - Status    b6a00002-...-b6a0: read-only (full /state)
///   - Log       b6a00003-...-b6a0: read with offset (suffix reads)
///   - DeviceInfo b6a00004-...-b6a0: read
final class GATTPeripheral: NSObject, CBPeripheralManagerDelegate {
    static let serviceUUIDString = "b6a00000-0000-4000-8000-00000000b6a0"
    static let controlUUIDString = "b6a00001-0000-4000-8000-00000000b6a0"
    static let statusUUIDString = "b6a00002-0000-4000-8000-00000000b6a0"
    static let logUUIDString = "b6a00003-0000-4000-8000-00000000b6a0"
    static let infoUUIDString = "b6a00004-0000-4000-8000-00000000b6a0"

    private static let serviceUUID = CBUUID(string: serviceUUIDString)
    private static let controlUUID = CBUUID(string: controlUUIDString)
    private static let statusUUID = CBUUID(string: statusUUIDString)
    private static let logUUID = CBUUID(string: logUUIDString)
    private static let infoUUID = CBUUID(string: infoUUIDString)

    private let sim: SimModel
    private let router: ControlRouter
    private let verbose: Bool

    private var manager: CBPeripheralManager!
    private var controlCharacteristic: CBMutableCharacteristic?
    private var statusCharacteristic: CBMutableCharacteristic?

    private var isAdvertising = false

    /// One queued outbound control/status message, split into ≤180-byte
    /// notification packets (fits ATT MTU 185 → 182-byte payload). Clients
    /// reassemble by appending packets until the buffer is one complete JSON
    /// value, per docs/bluetooth-gatt.md.
    private struct PendingMessage {
        let characteristic: CBMutableCharacteristic
        let chunks: [Data]
        var nextChunk = 0
    }

    private static let maxNotificationPayloadBytes = 180
    private var pendingMessages: [PendingMessage] = []

    init(sim: SimModel, router: ControlRouter, verbose: Bool) {
        self.sim = sim
        self.router = router
        self.verbose = verbose
        super.init()
        manager = CBPeripheralManager(delegate: self, queue: nil)
    }

    /// Entry point once the process is running. CoreBluetooth drives the rest
    /// through delegate callbacks on the main queue.
    func start() {
        log("[peripheral] started; waiting for CoreBluetooth state (see state: lines)")
        log("[peripheral] pairing/encryption NOT simulated — 'encrypted' is advisory in sim mode")
    }

    // MARK: - CBPeripheralManagerDelegate

    func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        switch peripheral.state {
        case .poweredOn:
            log("[peripheral] state: poweredOn")
            publishServiceIfNeeded()
        case .poweredOff:
            log("[peripheral] state: poweredOff")
            teardown()
        case .unauthorized:
            log("[peripheral] state: unauthorized (grant Bluetooth access in System Settings)")
        case .unsupported:
            log("[peripheral] state: unsupported (no BLE peripheral support on this Mac)")
        case .resetting:
            log("[peripheral] state: resetting")
        case .unknown:
            log("[peripheral] state: unknown")
        @unknown default:
            log("[peripheral] state: unknown(\(peripheral.state.rawValue))")
        }
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        didAdd service: CBService,
        error: Error?
    ) {
        if let error {
            log("[peripheral] error: add service failed: \(error.localizedDescription)")
            return
        }
        log("[peripheral] service added: \(Self.serviceUUID.uuidString)")
        startAdvertisingIfNeeded()
    }

    func peripheralManagerDidStartAdvertising(
        _ peripheral: CBPeripheralManager,
        error: Error?
    ) {
        if let error {
            isAdvertising = false
            log("[peripheral] error: startAdvertising failed: \(error.localizedDescription)")
            return
        }
        log("[peripheral] advertising: name=BoostGauge service=\(Self.serviceUUID.uuidString)")
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didSubscribeTo characteristic: CBCharacteristic
    ) {
        log("[gatt] central connected (subscribed): \(shortUUID(characteristic.uuid))")
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        central: CBCentral,
        didUnsubscribeFrom characteristic: CBCharacteristic
    ) {
        log("[gatt] central disconnected (unsubscribed): \(shortUUID(characteristic.uuid))")
        pendingMessages.removeAll { $0.characteristic.uuid == characteristic.uuid }
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        didReceiveRead request: CBATTRequest
    ) {
        let fullValue: Data?
        switch request.characteristic.uuid {
        case Self.statusUUID:
            fullValue = sim.statusData()
        case Self.logUUID:
            fullValue = sim.logData()
        case Self.infoUUID:
            fullValue = sim.deviceInfoData()
        default:
            fullValue = nil
        }

        guard let fullValue else {
            peripheral.respond(to: request, withResult: .attributeNotFound)
            return
        }

        let offset = request.offset
        if offset >= fullValue.count {
            // Short read / zero bytes at-or-beyond the end ends the transfer.
            request.value = Data()
            peripheral.respond(to: request, withResult: .success)
            return
        }

        request.value = fullValue.subdata(in: offset..<fullValue.count)
        if verbose {
            log("[gatt] read \(shortUUID(request.characteristic.uuid)) offset=\(offset) -> \(fullValue.count - offset) B suffix")
        }
        peripheral.respond(to: request, withResult: .success)
    }

    func peripheralManager(
        _ peripheral: CBPeripheralManager,
        didReceiveWrite requests: [CBATTRequest]
    ) {
        for request in requests {
            let value = request.value ?? Data()
            guard request.characteristic.uuid == Self.controlUUID else {
                log("[gatt] error: write to non-control characteristic rejected")
                peripheral.respond(to: request, withResult: .attributeNotFound)
                continue
            }

            // Oversized (>480 B) payloads are answered with a 413 too_large JSON
            // envelope by the router; the ATT write itself is still accepted.
            if let response = router.handle(raw: value) {
                queueNotification(response, for: controlCharacteristic, reason: "control")
            }
            // ACK at the ATT layer regardless; the JSON reply rides notifications.
            peripheral.respond(to: request, withResult: .success)
        }
    }

    func peripheralManagerIsReady(toUpdateSubscribers peripheral: CBPeripheralManager) {
        drainPendingNotifications()
    }

    // MARK: - Service / advertising

    private func publishServiceIfNeeded() {
        guard manager.state == .poweredOn else { return }
        let service = CBMutableService(type: Self.serviceUUID, primary: true)

        let control = CBMutableCharacteristic(
            type: Self.controlUUID,
            properties: [.write, .notify],
            value: nil,
            permissions: [.writeable]
        )
        let status = CBMutableCharacteristic(
            type: Self.statusUUID,
            properties: [.read],
            value: nil,
            permissions: [.readable]
        )
        let log = CBMutableCharacteristic(
            type: Self.logUUID,
            properties: [.read],
            value: nil,
            permissions: [.readable]
        )
        let info = CBMutableCharacteristic(
            type: Self.infoUUID,
            properties: [.read],
            value: nil,
            permissions: [.readable]
        )

        controlCharacteristic = control
        statusCharacteristic = status
        service.characteristics = [control, status, log, info]
        manager.add(service)
    }

    private func startAdvertisingIfNeeded() {
        guard manager.state == .poweredOn, !isAdvertising else { return }
        isAdvertising = true
        manager.startAdvertising([
            CBAdvertisementDataLocalNameKey: "BoostGauge",
            CBAdvertisementDataServiceUUIDsKey: [Self.serviceUUID],
        ])
    }

    private func teardown() {
        manager.stopAdvertising()
        isAdvertising = false
        pendingMessages.removeAll()
    }

    private func queueNotification(
        _ data: Data,
        for characteristic: CBMutableCharacteristic?,
        reason: String
    ) {
        guard let characteristic else {
            log("[gatt] warn: cannot notify \(reason): characteristic not published")
            return
        }
        pendingMessages.append(
            PendingMessage(characteristic: characteristic, chunks: chunks(of: data))
        )
        drainPendingNotifications()
    }

    private func chunks(of data: Data) -> [Data] {
        guard data.count > Self.maxNotificationPayloadBytes else { return [data] }
        var out: [Data] = []
        var offset = 0
        while offset < data.count {
            let end = min(offset + Self.maxNotificationPayloadBytes, data.count)
            out.append(data.subdata(in: offset..<end))
            offset = end
        }
        return out
    }

    private func drainPendingNotifications() {
        guard manager.state == .poweredOn else { return }
        while let head = pendingMessages.first {
            var message = head
            var progress = true
            while message.nextChunk < message.chunks.count {
                if manager.updateValue(
                    message.chunks[message.nextChunk],
                    for: message.characteristic,
                    onSubscribedCentrals: nil
                ) {
                    message.nextChunk += 1
                } else {
                    progress = false
                    break
                }
            }
            if progress {
                pendingMessages.removeFirst()
            } else {
                // Buffer full; peripheralManagerIsReady(toUpdateSubscribers:)
                // will retry when the stack has room.
                pendingMessages[0] = message
                break
            }
        }
    }

    // MARK: - Logging

    private func shortUUID(_ uuid: CBUUID) -> String {
        let s = uuid.uuidString.lowercased()
        if s.hasPrefix("b6a0000") {
            return String(s.prefix(9))
        }
        return s
    }

    private func log(_ line: String) {
        print(line)
        fflush(stdout)
    }
}
