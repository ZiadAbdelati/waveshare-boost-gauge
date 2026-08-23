import Foundation

/// Zone tokens mirror the firmware /state vocabulary (boost_model.c
/// zone_for_psi): OVER (≥ overboost), BOOST, ATMO, VAC. Both the Status JSON
/// and the Log lines emit these tokens.
enum SimZone: String {
    case over = "OVER"
    case boost = "BOOST"
    case atmo = "ATMO"
    case vac = "VAC"
}

enum TPMStatus: String {
    case normal = "normal"
    case stale = "stale"
    case disconnected = "disconnected"
}

enum OBDState: String {
    case disabled = "disabled"
    case scanning = "scanning"
    case connecting = "connecting"
    case ready = "ready"
    case error = "error"
}

/// Small in-memory config accepted by the Control `/config` route. Field names
/// follow the BLE GATT brief (appBle / brightness / theme), which is a subset
/// of the firmware HTTP `/config` surface. "Persist" here means process
/// lifetime only — a reboot of the simulator returns to these defaults.
struct SimConfig {
    var appBle: Bool = true
    var brightness: Int = 80
    var theme: String = "dyno-cell"
}

/// TPMS threshold config, mirroring firmware `/tpms/config` defaults
/// (220 kPa ≈ 32 psi, 15 s staleness) and its `lowKpa`/`lowPsi`/`staleAfterMs`
/// field names.
struct SimTpmsConfig {
    var lowKpa: Double = 220.0
    var staleAfterMs: UInt32 = 15_000

    var lowPsi: Double {
        (lowKpa / 6.894_757_293_168_361 * 10).rounded() / 10
    }
}

/// One synthesized background-log sample. Log bodies carry 600 of these
/// (2 minutes at 5 Hz), consistent with the live sine waveform.
struct LogSample {
    let tMs: UInt64
    let psi: Double
    let peakPsi: Double
    let zone: String
    let demo: Bool
}

/// Mutable gauge state driving every simulated surface: the live status
/// waveform, the log ring, and the control channel's read/modify/write paths.
///
/// The simulator is single-threaded: CoreBluetooth callbacks and the 1 Hz
/// status timer all run on the main run loop, so no locking is needed.
final class SimModel {
    let firmware: String
    var config: SimConfig
    var tpmsConfig: SimTpmsConfig

    /// Overboost threshold used only by the waveform zone classifier.
    let overboostPsi: Double = 12.0
    /// Demo-waveform period (seconds) and range (psi): sine 0-15, ~8 s.
    let wavePeriodSeconds: Double = 8.0
    let waveMinPsi: Double = 0.0
    let waveMaxPsi: Double = 15.0

    private let startWallClock = Date()
    private var peakPsi: Double = 0.0

    init(firmware: String) {
        self.firmware = firmware
        self.config = SimConfig()
        self.tpmsConfig = SimTpmsConfig()
    }

    /// Monotonic uptime in milliseconds since launch.
    var uptimeMs: UInt64 {
        UInt64((Date().timeIntervalSince(startWallClock) * 1000.0).rounded())
    }

    /// Smooth demo waveform around mid-range: sine 0-15 psi, period ~8 s, so
    /// the gauge breathes through ATMO/BOOST and near the peak hits OVER.
    func psi(atUptimeMs tMs: Double) -> Double {
        let t = tMs / 1000.0
        let mid = (waveMinPsi + waveMaxPsi) / 2.0
        let amp = (waveMaxPsi - waveMinPsi) / 2.0
        return mid + amp * sin(2.0 * .pi * t / wavePeriodSeconds)
    }

    func zone(forPsi psi: Double) -> String {
        // Same thresholds as firmware boost_model.c:zone_for_psi().
        if psi >= overboostPsi { return SimZone.over.rawValue }
        if psi >= 0.35 { return SimZone.boost.rawValue }
        if psi > -0.35 { return SimZone.atmo.rawValue }
        return SimZone.vac.rawValue
    }

    /// Current /state-shaped status snapshot. `peak` is the running maximum
    /// since launch (peak-hold, like the physical gauge until a tap reset).
    func statusObject(uptimeMs uptime: UInt64) -> [String: Any] {
        let psi = psi(atUptimeMs: Double(uptime))
        if psi > peakPsi {
            peakPsi = psi
        }
        let wheelKpa: [Double] = [224.3, 225.1, 223.8, 226.0]
        let wheelValid: [Bool] = [true, true, true, true]
        return [
            "psi": psi,
            "peak": peakPsi,
            "zone": zone(forPsi: psi),
            "demo": true,
            "uptimeMs": uptime,
            "brightness": config.brightness,
            "theme": config.theme,
            "tpms": [
                "status": TPMStatus.normal.rawValue,
                "kpa": wheelKpa,
                "valid": wheelValid,
            ],
            "obd": [
                "state": OBDState.ready.rawValue,
                "rpm": 850.0,
            ],
            "fw": firmware,
            "transport": "ble",
        ]
    }

    func statusData() -> Data {
        data(from: statusObject(uptimeMs: uptimeMs))
    }

    /// DeviceInfo read: `{"name":"BoostGauge","fw":...,"api":1}`.
    func deviceInfoObject() -> [String: Any] {
        [
            "name": "BoostGauge",
            "fw": firmware,
            "api": 1,
        ]
    }

    func deviceInfoData() -> Data {
        data(from: deviceInfoObject())
    }

    /// The `/config` GET body (and the canonical in-memory config shape).
    func configObject() -> [String: Any] {
        [
            "appBle": config.appBle,
            "brightness": config.brightness,
            "theme": config.theme,
        ]
    }

    /// The `/tpms/config` GET body, mirroring firmware field names.
    func tpmsConfigObject() -> [String: Any] {
        [
            "lowKpa": tpmsConfig.lowKpa,
            "lowPsi": tpmsConfig.lowPsi,
            "staleAfterMs": tpmsConfig.staleAfterMs,
        ]
    }

    /// Synthesizes `count` log samples at 5 Hz ending at the current uptime,
    /// each consistent with the live sine waveform (running peak included).
    func synthesizeLogs(count: Int) -> [LogSample] {
        let capped = min(max(count, 1), 600)
        let intervalMs: UInt64 = 200
        let uptime = uptimeMs
        // Saturate the window start to 0 so a fresh process (uptime < 2 min)
        // never underflows UInt64: samples are [0 ... uptime], and once the
        // process has run 2+ minutes they end exactly at `uptime`.
        let windowMs = UInt64(capped - 1) * intervalMs
        let startMs = uptime >= windowMs ? uptime - windowMs : 0
        var runningPeak = 0.0
        var samples: [LogSample] = []
        samples.reserveCapacity(capped)
        for i in 0..<capped {
            let t = startMs + UInt64(i) * intervalMs
            let psi = psi(atUptimeMs: Double(t))
            runningPeak = max(runningPeak, psi)
            samples.append(
                LogSample(
                    tMs: t,
                    psi: psi,
                    peakPsi: runningPeak,
                    zone: zone(forPsi: psi),
                    demo: true
                )
            )
        }
        return samples
    }

    /// Log characteristic body: `BGL1\n` magic + 600 newline-terminated lines
    /// `t_ms,psi,peak_psi,zone,demo` with firmware zone tokens (VAC/ATMO/BOOST/OVER).
    /// Read-with-offset clients page through the suffix in chunks.
    func logData() -> Data {
        var text = GaugeRoutes.logHeader
        for sample in synthesizeLogs(count: 600) {
            let demo = sample.demo ? 1 : 0
            text += String(
                format: "%llu,%.2f,%.2f,%@,%ld\n",
                sample.tMs, sample.psi, sample.peakPsi, sample.zone, demo
            )
        }
        return Data(text.utf8)
    }

    /// /logs body: firmware-shaped `{"samples":[{tMs,psi,peakPsi,zone,demo},...]}`.
    func logsObject(count: Int) -> [String: Any] {
        let samples: [[String: Any]] = synthesizeLogs(count: count).map { sample in
            [
                "tMs": sample.tMs,
                "psi": sample.psi,
                "peakPsi": sample.peakPsi,
                "zone": sample.zone,
                "demo": sample.demo,
            ]
        }
        return ["samples": samples]
    }

    private func data(from object: Any) -> Data {
        guard let data = try? JSONSerialization.data(
            withJSONObject: object,
            options: [.sortedKeys]
        ) else {
            assertionFailure("failed to serialize JSON")
            return Data()
        }
        return data
    }
}
