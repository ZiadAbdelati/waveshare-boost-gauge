import Foundation

/// In-process stand-in for `BleTransport` so the BLE-mode UI can be iterated
/// in the iOS simulator, which cannot use CoreBluetooth. Mirrors the firmware's
/// BLE behaviour: the Log characteristic is capped (8-sample BGL1 window, so
/// `readLogSamples`/`readLog` always return 8), while the bounded log graph
/// window (`GET /logs?limit=1500`, the last 5 minutes at 5 Hz) is served
/// through `get("logs?limit=…")` exactly like the HTTP /logs endpoint.
///
/// Enabled via the `-e2eSimBle` launch argument in `AppSession`.
final class SimBleTransport: GaugeTransport {
    let transportKind = "BLE"

    private let startDate = Date()
    private var networkPayload: [String: Any] = [
        "mode": "apsta",
        "staEnabled": true,
        "staConnected": false,
        "staSsid": "",
        "staIp": "",
        "rssi": -58,
        "apSsid": "BoostGauge-TEST",
        "saved": [String](),
    ]

    private static let scanPayload: [[String: Any]] = [
        ["ssid": "HomeNet 5G", "rssi": -42, "auth": 3],
        ["ssid": "CoffeeShop", "rssi": -71, "auth": 3],
        ["ssid": "OpenGuest", "rssi": -80, "auth": 0],
    ]

    private var themesPayload: [String: Any] = SimBleTransport.defaultThemesPayload
    private var activePage = 0

    func readDeviceInfo() -> BleDeviceInfo {
        BleDeviceInfo(name: "BoostGauge", firmware: "sim", api: 1, ip: nil)
    }

    func get(_ path: String) async throws -> Resp {
        switch path {
        case "state":
            return Resp(status: 200, body: stateBody())
        case "themes":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: themesPayload))
        case "config":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: Self.configPayload))
        case "tpms/config":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: Self.tpmsConfigPayload))
        case "sensors/calibration":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: Self.calibrationPayload))
        case let path where path == "logs" || path.hasPrefix("logs?"):
            return Resp(status: 200, body: logsBody(limit: Self.limit(from: path)))
        case "network":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: networkPayload))
        case "network/scan":
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: Self.scanPayload))
        default:
            throw TransportError.deviceNotFound
        }
    }

    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp {
        switch (method, path) {
        case ("PUT", "themes/active"):
            if let id = body["id"] as? String {
                themesPayload["activeThemeId"] = id
            }
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: themesPayload))
        case ("PUT", "themes/config"):
            applyThemeConfig(body)
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: themesPayload))
        case ("PUT", "page"):
            if let page = body["page"] as? Int {
                activePage = page
            }
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: ["ok": true, "activePage": activePage]))
        case ("POST", "time"):
            // Timezone-only sync mirror: the sim has no RTC, so the timezone
            // body is acknowledged but nothing is persisted.
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: ["ok": true]))
        case ("PUT", "tpms/config"):
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: Self.tpmsConfigPayload))
        case ("PUT", "network"):
            if let ssid = body["ssid"] as? String, !ssid.isEmpty {
                networkPayload["staSsid"] = ssid
                if body["keepPassword"] as? Bool == true {
                    // keepPassword mirror: reuse the stored PSK, no password needed
                } else if let pass = body["password"] as? String, !pass.isEmpty {
                    networkPayload["saved"] = [ssid]
                }
                var saved = (networkPayload["saved"] as? [String]) ?? []
                if !saved.contains(ssid) { saved.append(ssid) }
                networkPayload["saved"] = saved
                networkPayload["staConnected"] = true
                networkPayload["staIp"] = "192.168.1.234"
            }
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: networkPayload))
        case ("POST", "network/reconnect"):
            networkPayload["staConnected"] = true
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: networkPayload))
        case ("DELETE", "network"):
            return Resp(status: 200, body: try JSONSerialization.data(withJSONObject: ["ok": true]))
        default:
            throw TransportError.deviceNotFound
        }
    }

    /// The app's bounded BLE window asks for 300 samples (the real BLE
    /// characteristic only ever carries the last 8); anything at or above
    /// that is the full-ring request and gets the dense hour of history.
    func readLogSamples(limit: Int) async throws -> [LogSample] {
        // Mirrors the firmware: the BLE Log characteristic is capped at the
        // 8-sample BGL1 diagnostic window regardless of the requested limit.
        // The 5-minute /logs?limit=1500 window is served by `get("logs…")`.
        Array(denseSamples.suffix(8))
    }

    /// The firmware BLE Log characteristic: the last 8 samples.
    func readLog() async throws -> [LogSample] {
        Array(denseSamples.suffix(8))
    }

    func liveStatusStream() -> AsyncStream<Result<Data, Error>> {
        AsyncStream { continuation in
            let task = Task { [weak self] in
                while !Task.isCancelled {
                    guard let self else { return }
                    continuation.yield(.success(self.stateBody()))
                    try? await Task.sleep(nanoseconds: 1_000_000_000)
                }
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    func disconnect() {}

    // MARK: - State

    private func stateBody() -> Data {
        let uptimeMs = Int64(Date().timeIntervalSince(startDate) * 1000)
        let sample = Self.simSample(atMs: uptimeMs)
        var state = Self.baseState
        state["psi"] = sample.psi
        state["peakPsi"] = 10.0
        state["zone"] = sample.zone
        state["uptimeMs"] = uptimeMs
        state["epochMs"] = Int64(Date().timeIntervalSince1970 * 1000)
        state["activeThemeId"] = themesPayload["activeThemeId"] ?? "neon"
        state["activePage"] = activePage
        state["tpms"] = tpms(atMs: uptimeMs)
        return (try? JSONSerialization.data(withJSONObject: state)) ?? Data()
    }

    /// Plausible tire data (the captured fixture has a bench board with no
    /// TPMS, so its wheels are all zero/invalid). Four valid wheels near
    /// 30-34 psi, with one wheel occasionally sagging toward lowPsi so the
    /// native TPMS card's amber low-pressure state is visible while iterating.
    private func tpms(atMs uptimeMs: Int64) -> [String: Any] {
        var base: [Double] = [31.2, 32.0, 30.5, 33.1]
        let lowPsi = 26.0
        let cycleSeconds = 14.0
        let t = Double(uptimeMs) / 1000.0
        let inWindow = t.truncatingRemainder(dividingBy: cycleSeconds) / cycleSeconds
        if inWindow < 0.4 {
            // One wheel reads 22.0 → 25.0 psi (always at/below lowPsi) for
            // ~40% of the cycle so the amber low-pressure state is reliably
            // visible while iterating; the next cycle dips a different wheel.
            let lowIndex = Int(t / cycleSeconds) % 4
            base[lowIndex] = 22.0 + 3.0 * (inWindow / 0.4)
        }
        let wheels = base.enumerated().map { index, psi -> [String: Any] in
            // Small live wobble on every wheel so each capsule visibly
            // updates between sim ticks (real TPMS jitters too), while the
            // low-pressure cycle still drives one wheel to amber.
            let jitter = sin(t * 1.7 + Double(index) * 2.1) * 0.15
            return ["psi": ((psi + jitter) * 10).rounded() / 10, "valid": true]
        }
        return ["status": 0, "lowPsi": lowPsi, "wheels": wheels]
    }

    /// Demo waveform around -3..+10 psi: a slow boost/vacuum swell with
    /// smaller ripples so the log graph reads like a real hour of driving.
    static func simSample(atMs tMs: Int64) -> (psi: Double, zone: String) {
        let t = Double(tMs) / 1000.0
        let psi = -2.0
            + 5.5 * sin(2 * .pi * t / 360.0)
            + 3.0 * sin(2 * .pi * t / 90.0 + 1.3)
            + 1.0 * sin(2 * .pi * t / 22.0 + 0.7)
        let rounded = (psi * 100).rounded() / 100
        return (rounded, rounded < 0 ? "VAC" : "BOOST")
    }

    // MARK: - Theme mutation

    private func applyThemeConfig(_ body: [String: Any]) {
        for (key, value) in body where key != "id" && key != "colors" && key != "reset" {
            themesPayload[key] = value
        }
        if let colors = body["colors"] as? [String: String], let id = body["id"] as? String {
            updateTheme(id) { theme in
                var themeColors = theme["colors"] as? [String: Any] ?? [:]
                for (key, value) in colors { themeColors[key] = value }
                theme["colors"] = themeColors
                theme["customized"] = true
            }
        }
        if body["reset"] as? Bool == true, let id = body["id"] as? String {
            resetTheme(id)
        }
    }

    private func updateTheme(_ id: String, _ mutate: (inout [String: Any]) -> Void) {
        guard var themes = themesPayload["themes"] as? [[String: Any]] else { return }
        for i in themes.indices where themes[i]["id"] as? String == id {
            var theme = themes[i]
            mutate(&theme)
            themes[i] = theme
        }
        themesPayload["themes"] = themes
    }

    private func resetTheme(_ id: String) {
        guard let originals = Self.defaultThemesPayload["themes"] as? [[String: Any]],
              let original = originals.first(where: { $0["id"] as? String == id }) else { return }
        updateTheme(id) { theme in
            theme = original
        }
    }

    // MARK: - Dense log ring

    private lazy var denseSamples: [LogSample] = {
        (0..<18_000).map { index in
            let tMs = Int64(index) * 200
            let sample = Self.simSample(atMs: tMs)
            return LogSample(tMs: tMs, epochTs: nil, psi: sample.psi, peakPsi: 10.0, zone: sample.zone, demo: true)
        }
    }()

    /// Serves the last `limit` ring samples as the HTTP `/logs?limit=` shape
    /// (no limit = the full one-hour ring, matching the firmware default).
    private func logsBody(limit: Int?) -> Data {
        let samples = (limit.map { denseSamples.suffix($0) } ?? denseSamples).map { sample in
            let tMs = sample.tMs ?? 0
            let peak = sample.peakPsi ?? 10.0
            return ["tMs": tMs, "psi": sample.psi, "peakPsi": peak, "zone": sample.zone, "demo": sample.demo]
        }
        return (try? JSONSerialization.data(withJSONObject: ["samples": samples])) ?? Data()
    }

    private static func limit(from path: String) -> Int? {
        guard let range = path.range(of: "limit=") else { return nil }
        return Int(path[range.upperBound...])
    }

    // MARK: - Fixtures (byte-for-byte shapes captured from the board)

    private static let configPayload: [String: Any] = [
        "brightnessHigh": 100,
        "brightnessLow": 25,
        "dimSchedule": ["enabled": true, "startMinutes": 1230, "endMinutes": 420],
        "timezoneOffsetMinutes": -300,
        "timezoneTz": "EST5EDT,M3.2.0/2,M11.1.0/2",
        "activeThemeId": "neon",
        "psiMin": -15.00,
        "psiMax": 10.00,
        "psiOverboost": 8.00,
        "zeroAngle": 220.00,
        "appBle": true,
    ]

    private static let tpmsConfigPayload: [String: Any] = [
        "lowPsi": 26.0,
        "staleAfterMs": 15_000,
    ]

    private static let calibrationPayload: [String: Any] = [
        "supplyVolts": 5.2,
        "live": [
            "adsPresent": true,
            "bmpPresent": true,
            "fault": false,
            "mapVolts": 1.4180,
            "mapAgeMs": 12,
            "nominalKpa": 153.2,
            "correctedKpa": 155.2,
            "bmpKpa": 101.3,
            "bmpAgeMs": 45,
            "bmpUpdates": 4000,
            "ambientIsFallback": false,
        ],
        "calibration": [
            "valid": true,
            "version": 1,
            "offsetKpa": 2.0,
            "offsetPsi": 0.290,
            "supplyVolts": 5.2,
            "refMapVolts": 1.2250,
            "refNominalKpa": 99.3,
            "refBmpKpa": 101.3,
            "samples": 120,
            "epochMs": 1_784_000_000_000,
        ],
    ]

    private static let baseState: [String: Any] = [
        "psi": 3.29,
        "peakPsi": 10.00,
        "zone": "BOOST",
        "demo": true,
        "brightness": 100,
        "firmwareVersion": "sim (v0.8.1 shape)",
        "uptimeMs": 0,
        "epochMs": 0,
        "timezoneOffsetMinutes": -240,
        "activeThemeId": "neon",
        "activePage": 0,
        "display": [
            "renderFps": 60,
            "gaugeDemandPerSecond": 62,
            "flushesPerSecond": 392,
            "pixelsPerSecond": 1110788,
            "worstRenderUs": 34037,
            "renderGapP50Us": 16000,
            "renderGapMaxUs": 35355,
            "framesOverBudget": 7,
            "tePeriodUs": 16728,
            "teWaits": 54,
            "teTimeouts": 0,
            "teSkips": 44,
            "teScanlineWaits": 0,
        ],
        "sensors": [
            "adsPresent": true,
            "bmpPresent": true,
            "fault": false,
            "mapVolts": 1.4180,
            "mapAbsKpa": 123.4,
            "ambientKpa": 101.3,
        ],
        "tpms": [
            "status": 0,
            "lowPsi": 26.0,
            "wheels": [
                ["psi": 31.2, "valid": true],
                ["psi": 32.0, "valid": true],
                ["psi": 30.5, "valid": true],
                ["psi": 33.1, "valid": true],
            ],
        ],
        "obd": [
            "state": 3,
            "lastError": 0,
            "peer": "vlinker fd+",
            "peerAddr": "11:22:33:44:55:66",
            "uptimeMs": 9000,
            "ageMs": 120,
            "valid": true,
            "lastReply": "41 0C 58",
            "protocol": "ISO 15765-4",
            "rpm": 900.0,
            "speedKph": 0.0,
            "coolantC": 88.0,
            "mapKpa": 33.0,
            "iatC": 31.0,
            "throttlePct": 18.0,
            "mafGps": 5.2,
            "fuelPct": 62.0,
            "batteryV": 12.4,
        ],
    ]

    private static let defaultThemesPayload: [String: Any] = [
        "activeThemeId": "neon",
        "bigDigitStaticBg": true,
        "bigDigitColorText": false,
        "bigDigitStaticColor": "#000000",
        "bigDigitTextColor": "#ffffff",
        "arcGradient": false,
        "hudGradient": false,
        "hudTrueBlack": true,
        "neonMarqueeSpin": false,
        "teSync": true,
        "regionDBuf": true,
        "teScanline": false,
        "rotation": 0,
        "vaultFace": "#041c11",
        "vaultVignette": 45,
        "vaultNeedleRed": true,
        "vaultNeedleTail": false,
        "neonLayout": 1,
        "neonFont": 0,
        "neonPreset": 3,
        "demoMode": true,
        "demoFastSweep": true,
        "tpmsBle": true,
        "pixelShift": false,
        "pixelShiftSec": 90,
        "themes": [
            ["id": "dyno-cell", "name": "Dyno Cell", "style": "arc", "customized": false,
             "colors": ["face": "#090a0d", "track": "#20242c", "text": "#f5f7fa", "muted": "#8c95a3",
                        "vacuum": "#4dd2ff", "boost": "#b8f35a", "overboost": "#ff4f6d", "zero": "#ffffff"]],
            ["id": "vault-tec", "name": "Vault-Tec", "style": "vault", "customized": false,
             "colors": ["face": "#05281a", "track": "#0c3d24", "text": "#38f08a", "muted": "#1f7a4d",
                        "vacuum": "#38f08a", "boost": "#38f08a", "overboost": "#eafc50", "zero": "#38f08a"]],
            ["id": "night-city", "name": "Night City", "style": "hud", "customized": false,
             "colors": ["face": "#080a08", "track": "#1a1c0a", "text": "#fcee0a", "muted": "#5a7a0a",
                        "vacuum": "#00e5ff", "boost": "#fcee0a", "overboost": "#ff003c", "zero": "#00e5ff"]],
            ["id": "big-digit", "name": "Big Digit", "style": "bigdigit", "customized": false,
             "colors": ["face": "#0b0c0e", "track": "#20242c", "text": "#ffffff", "muted": "#0b0c0e",
                        "vacuum": "#4dd2ff", "boost": "#b8f35a", "overboost": "#ff4f6d", "zero": "#ffffff"]],
            ["id": "neon", "name": "Neon", "style": "neon", "customized": false,
             "colors": ["face": "#000000", "track": "#0c1440", "text": "#ffffff", "muted": "#35509e",
                        "vacuum": "#0064ff", "boost": "#c4172e", "overboost": "#ff6a00", "zero": "#ffffff"]],
        ],
    ]
}