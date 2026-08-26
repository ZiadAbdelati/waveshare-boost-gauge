import Foundation
import CoreBluetooth

let GAUGE_SERVICE = CBUUID(string: "B6A00000")

final class Central: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    let queue = DispatchQueue(label: "ble.central")
    let logQ = DispatchQueue(label: "ble.log")
    var central: CBCentralManager!
    var scanning = false
    var didResolve = false
    var resetCount = 0
    let deadline: Date
    let filterService: Bool

    init(timeout: TimeInterval, filterService: Bool) {
        self.deadline = Date().addingTimeInterval(timeout)
        self.filterService = filterService
        super.init()
        makeCentral()
        queue.asyncAfter(deadline: .now() + max(0, deadline.timeIntervalSinceNow)) { [weak self] in
            self?.timeout()
        }
    }

    func makeCentral() {
        central = CBCentralManager(delegate: self, queue: queue,
            options: [CBCentralManagerOptionShowPowerAlertKey: false])
    }

    func log(_ s: String) {
        logQ.sync { print("[\(String(format: "%.3f", Date().timeIntervalSince1970))] \(s)") }
    }

    func stateName(_ s: CBManagerState) -> String {
        switch s {
        case .unknown: return "unknown(0)"
        case .resetting: return "resetting(1)"
        case .unsupported: return "unsupported(2)"
        case .unauthorized: return "unauthorized(3)"
        case .poweredOff: return "poweredOff(4)"
        case .poweredOn: return "poweredOn(5)"
        @unknown default: return "other(\(s.rawValue))"
        }
    }

    func timeout() {
        guard !didResolve else { return }
        log("TIMEOUT after \(Int(-deadline.timeIntervalSinceNow))s — no stable connection"
            + (resetCount > 0 ? " (saw \(resetCount) reset(s); try THIRD_PARTY_DONGLE / stop itlwm scans)" : ""))
        finish(2)
    }

    func finish(_ code: Int32) -> Never {
        log("exit \(code)")
        exit(code)
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        log("central state -> \(stateName(c.state))")
        switch c.state {
        case .poweredOn:
            startScan()
        case .resetting:
            // Intel firmware flap (5->1). Tear the central down and recreate once.
            log("reset detected; releasing central and scheduling recreate")
            central = nil
            resetCount += 1
            if resetCount > 1 {
                log("second reset: giving up to avoid wedging. Prefer a USB BT dongle or stop concurrent itlwm WiFi scans.")
                finish(3)
            }
            queue.asyncAfter(deadline: .now() + 3.0) { [weak self] in
                self?.log("recreating central after reset")
                self?.makeCentral()
            }
        case .unknown:
            log("state unknown; waiting for next update")
        default:
            log("central unavailable: \(stateName(c.state))")
            finish(4)
        }
    }

    func startScan() {
        guard !scanning else { return }
        scanning = true
        let opts: [String: Any] = [CBCentralManagerScanOptionAllowDuplicatesKey: false]
        if filterService {
            central.scanForPeripherals(withServices: [GAUGE_SERVICE], options: opts)
        } else {
            central.scanForPeripherals(withServices: nil, options: opts)
        }
        log("scanning (allowDuplicates=false, filterService=\(filterService))")
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? "?"
        let services = (advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID]) ?? []
        let matches = services.contains(GAUGE_SERVICE)
            || name.lowercased().contains("gauge") || name.lowercased().contains("boost")
        log("discovered \(p.identifier.uuidString) rssi=\(RSSI)dBm name=\(name) svc=\(services)")
        guard matches, !didResolve else { return }
        // Let the Intel stack settle before connecting; a 500ms gap avoids the
        // connect-hang that follows a fresh discovery on flapping firmware.
        queue.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            self?.connect(to: p)
        }
    }

    func connect(to p: CBPeripheral) {
        guard central.state == .poweredOn else {
            log("connect skipped: central \(stateName(central.state)); will retry after rescan")
            return
        }
        log("connecting to \(p.identifier.uuidString)")
        central.connect(p, options: [CBConnectPeripheralOptionNotifyOnDisconnectionKey: true])
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("CONNECTED \(p.identifier.uuidString)")
        didResolve = true
        p.delegate = self
        p.discoverServices([GAUGE_SERVICE])
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            log("service discovery error: \(error.localizedDescription)")
            finish(1)
        }
        log("services: \(p.services?.map { $0.uuid.uuidString } ?? [])")
        finish(0)
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("FAILED to connect: \(error?.localizedDescription ?? "unknown")")
        finish(1)
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        log("disconnected: \(error?.localizedDescription ?? "ok")")
        if !didResolve { finish(1) }
    }
}

var timeout = 20.0
var filter = false
var args = Array(CommandLine.arguments.dropFirst())
while let a = args.first {
    args.removeFirst()
    if a == "--timeout", let v = args.first, let d = Double(v) { args.removeFirst(); timeout = d }
    else if a == "--service" { filter = true }
    else if a == "--help" { print("usage: ble_central_test [--timeout s] [--service]"); exit(0) }
}

let c = Central(timeout: timeout, filterService: filter)
dispatchMain()
