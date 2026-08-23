import XCTest
@testable import BoostGauge

final class ViewModelTests: XCTestCase {
    func testStatusViewModelLoadsFromStream() async throws {
        let transport = FakeTransport()
        let stateJSON = Fixtures.stateJSON
        transport.stream = AsyncStream { continuation in
            continuation.yield(.success(Data(stateJSON.utf8)))
            continuation.finish()
        }
        let vm = StatusViewModel()
        vm.reset(transport: transport)
        let state = try await waitForState(vm)
        XCTAssertEqual(state.psi, 3.24)
        XCTAssertEqual(state.zone, .boost)
        XCTAssertEqual(state.tpms?.wheels.count, 4)
        XCTAssertEqual(state.obd?.rpm, 900.0)
    }

    func testStatusViewModelForceRefresh() async throws {
        let transport = FakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = StatusViewModel()
        vm.reset(transport: transport)
        await vm.forceRefresh()
        let state = try XCTUnwrap(vm.state)
        XCTAssertEqual(state.activeThemeId, "dyno-cell")
        XCTAssertEqual(state.uptimeMs, 1_234_000)
    }

    func testStatusViewModelSendsPagePut() async throws {
        let transport = FakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        transport.responses["page"] = FakeTransport.resp(200, ["ok": true, "activePage": 1])
        let vm = StatusViewModel()
        vm.reset(transport: transport)
        await vm.forceRefresh()
        XCTAssertEqual(vm.state?.activePage, 0)
        XCTAssertEqual(vm.displayedPage, 0)

        await vm.setPage(1)
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "page")
        XCTAssertEqual(transport.recordedBodies.last?["page"] as? Int, 1)
        XCTAssertEqual(vm.displayedPage, 1)
    }

    func testThemesViewModelLoadsAndSelects() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        XCTAssertEqual(vm.themes.count, 2)
        XCTAssertEqual(vm.activeThemeID, "dyno-cell")
        XCTAssertEqual(vm.themes.first?.name, "Dyno Cell")

        transport.responses["themes/active"] = FakeTransport.resp(200, Fixtures.themesObjectActiveNeon)
        await vm.select("neon")
        XCTAssertEqual(vm.activeThemeID, "neon")
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "themes/active")
        XCTAssertEqual(transport.recordedBodies.last?["id"] as? String, "neon")
    }

    func testSettingsViewModelLoadsConfigThemeFlagsAndTPMS() async throws {
        let transport = FakeTransport()
        transport.responses["config"] = FakeTransport.resp(200, Fixtures.configObject)
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        transport.responses["tpms/config"] = FakeTransport.resp(200, Fixtures.tpmsConfigObject)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        XCTAssertEqual(vm.brightnessHigh, 100)
        XCTAssertEqual(vm.psiMax, 10.0)
        XCTAssertTrue(vm.demoMode)
        XCTAssertTrue(vm.tpmsBle)
        XCTAssertFalse(vm.appBle)
        XCTAssertEqual(vm.tpmsLowPsi, 32.5, accuracy: 0.01)
        XCTAssertEqual(vm.tpmsStaleAfterMs, 15_000)
    }

    func testSettingsViewModelSavesConfig() async throws {
        let transport = FakeTransport()
        transport.responses["config"] = FakeTransport.resp(200, Fixtures.configObject)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        vm.brightnessHigh = 80
        transport.responses["config"] = FakeTransport.resp(200, Fixtures.configObjectSaved)
        await vm.saveConfig()
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "config")
        let body = try XCTUnwrap(transport.recordedBodies.last)
        XCTAssertEqual(body["brightnessHigh"] as? Int, 80)
        XCTAssertEqual((body["dimSchedule"] as? [String: Any])?["enabled"] as? Bool, true)
    }

    func testSettingsViewModelRoundTripsAppBle() async throws {
        let transport = FakeTransport()
        var enabledConfig = Fixtures.configObject
        enabledConfig["appBle"] = true
        transport.responses["config"] = FakeTransport.resp(200, enabledConfig)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        XCTAssertTrue(vm.appBle)

        let disabledConfig = Fixtures.configObject
        transport.responses["config"] = FakeTransport.resp(200, disabledConfig)
        vm.appBle = false
        await vm.saveConfig()
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "config")
        XCTAssertEqual(transport.recordedBodies.last?["appBle"] as? Bool, false)
        XCTAssertFalse(vm.appBle)
    }

    func testSettingsViewModelReportsLoadFailure() async {
        let transport = FakeTransport()
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        XCTAssertFalse(vm.isLoading)
        XCTAssertNil(vm.config)
        XCTAssertNotNil(vm.errorMessage)
    }

    func testCalibrationViewModelLoadsAndCalibrates() async throws {
        let transport = FakeTransport()
        transport.responses["sensors/calibration"] = FakeTransport.resp(200, Fixtures.calibrationObject)
        let vm = CalibrationViewModel()
        vm.reset(transport: transport)
        await vm.load()
        XCTAssertEqual(vm.calibration?.live?.mapVolts, 2.4180)
        XCTAssertEqual(vm.calibration?.calibration?.offsetPsi, 0.290)

        transport.responses["sensors/calibration"] = FakeTransport.resp(200, Fixtures.calibrationObjectCalibrated)
        await vm.calibrate()
        XCTAssertEqual(transport.recordedMethods.last, "POST")
        XCTAssertEqual(transport.recordedPaths.last, "sensors/calibration")
        XCTAssertNotNil(vm.calibration?.calibration?.valid)
        XCTAssertNotNil(vm.successMessage)
    }

    func testLogsViewModelLoadsAndBuildsCSV() async throws {
        let transport = FakeTransport()
        transport.responses["logs?limit=300"] = FakeTransport.resp(200, Fixtures.logsObject)
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 300)
        XCTAssertEqual(vm.samples.count, 2)
        XCTAssertEqual(vm.samples[0].zone, "BOOST")

        let csv = vm.csvText()
        let lines = csv.split(separator: "\n")
        XCTAssertEqual(lines[0], "timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo")
        XCTAssertTrue(lines[1].contains(",1.50,2.00,BOOST,0"))
    }

    func testLogsViewModelFallsBackToHTTPShape() async throws {
        let transport = BleLogFallbackTransport()
        transport.responses["logs"] = FakeTransport.resp(200, [
            "samples": [
                ["tMs": 1000, "psi": 2.5, "peakPsi": 3.0, "zone": "BOOST", "demo": false],
            ],
        ])
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 300)
        XCTAssertEqual(vm.samples.count, 1)
        XCTAssertEqual(vm.samples[0].psi, 2.5)
    }

    func testCSVUsesDeviceOffset() {
        let state = try! JSONDecoder().decode(GaugeState.self, from: Data(Fixtures.stateJSON.utf8))
        let sample = LogSample(tMs: 10_000, epochTs: nil, psi: 1.5, peakPsi: 2.0, zone: "BOOST", demo: false)
        let csv = LogsViewModel.csv(from: [sample], anchor: state)
        let lines = csv.split(separator: "\n")
        XCTAssertTrue(lines[1].contains("2026-"))
        XCTAssertTrue(lines[1].contains("1.50,2.00,BOOST,0"))
        let columns = lines[1].split(separator: ",")
        XCTAssertGreaterThan(Int64(columns[2]) ?? 0, 0)
    }

    // MARK: - Helpers

    private func waitForState(_ vm: StatusViewModel) async throws -> GaugeState {
        let deadline = Date().addingTimeInterval(2)
        while Date() < deadline {
            if let state = await MainActor.run(body: { vm.state }) {
                return state
            }
            try? await Task.sleep(nanoseconds: 50_000_000)
        }
        let unwrapped = await MainActor.run(body: { vm.state })
        return try XCTUnwrap(unwrapped)
    }
}

private final class BleLogFallbackTransport: FakeTransport {
    override func readLogSamples(limit: Int) async throws -> [LogSample] {
        throw TransportError.badLogFormat
    }
}

enum Fixtures {
    static let stateObject: [String: Any] = [
        "psi": 3.24,
        "peakPsi": 4.5,
        "zone": "BOOST",
        "demo": false,
        "brightness": 100,
        "firmwareVersion": "v0.8.0",
        "uptimeMs": 1_234_000,
        "epochMs": 1_784_000_000_000,
        "timezoneOffsetMinutes": -240,
        "activeThemeId": "dyno-cell",
        "activePage": 0,
        "display": [
            "renderFps": 60,
            "gaugeDemandPerSecond": 62,
            "worstRenderUs": 8000,
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
            "lowPsi": 32.0,
            "wheels": [
                ["psi": 32.5, "valid": true],
                ["psi": 33.0, "valid": true],
                ["psi": 31.8, "valid": true],
                ["psi": 32.8, "valid": true],
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

    static let stateJSON = """
    {"psi":3.24,"peakPsi":4.5,"zone":"BOOST","demo":false,"brightness":100,"firmwareVersion":"v0.8.0","uptimeMs":1234000,"epochMs":1784000000000,"timezoneOffsetMinutes":-240,"activeThemeId":"dyno-cell","activePage":0,"display":{"renderFps":60,"gaugeDemandPerSecond":62,"worstRenderUs":8000},"sensors":{"adsPresent":true,"bmpPresent":true,"fault":false,"mapVolts":1.418,"mapAbsKpa":123.4,"ambientKpa":101.3},"tpms":{"status":0,"lowPsi":32.0,"wheels":[{"psi":32.5,"valid":true},{"psi":33.0,"valid":true},{"psi":31.8,"valid":true},{"psi":32.8,"valid":true}]},"obd":{"state":3,"lastError":0,"peer":"vlinker fd+","peerAddr":"11:22:33:44:55:66","uptimeMs":9000,"ageMs":120,"valid":true,"lastReply":"41 0C 58","protocol":"ISO 15765-4","rpm":900.0,"speedKph":0.0,"coolantC":88.0,"mapKpa":33.0,"iatC":31.0,"throttlePct":18.0,"mafGps":5.2,"fuelPct":62.0,"batteryV":12.4}}
    """

    static let themesObject: [String: Any] = [
        "activeThemeId": "dyno-cell",
        "demoMode": true,
        "demoFastSweep": false,
        "tpmsBle": true,
        "rotation": 0,
        "regionDBuf": true,
        "themes": [
            [
                "id": "dyno-cell",
                "name": "Dyno Cell",
                "style": "arc",
                "colors": [
                    "face": "#090A0D", "track": "#20242C", "text": "#F5F7FA",
                    "muted": "#8C95A3", "vacuum": "#4DD2FF", "boost": "#B8F35A",
                    "overboost": "#FF4F6D", "zero": "#FFFFFF",
                ],
                "customized": false,
            ],
            [
                "id": "neon",
                "name": "Neon",
                "style": "neon",
                "colors": [
                    "face": "#000000", "track": "#241038", "text": "#FFFFFF",
                    "muted": "#5A3A7A", "vacuum": "#8B3DFF", "boost": "#FF2BD6",
                    "overboost": "#FF6A00", "zero": "#FFFFFF",
                ],
                "customized": false,
            ],
        ],
    ]

    static let themesObjectActiveNeon: [String: Any] = {
        var object = themesObject
        object["activeThemeId"] = "neon"
        return object
    }()

    static let configObject: [String: Any] = [
        "brightnessHigh": 100,
        "brightnessLow": 12,
        "dimSchedule": ["enabled": true, "startMinutes": 1260, "endMinutes": 420],
        "timezoneOffsetMinutes": -240,
        "timezoneTz": "EST5EDT,M3.2.0/2,M11.1.0/2",
        "activeThemeId": "dyno-cell",
        "psiMin": -15.0,
        "psiMax": 10.0,
        "psiOverboost": 8.0,
        "zeroAngle": 236.25,
        "appBle": false,
    ]

    static let configObjectSaved: [String: Any] = {
        var object = configObject
        object["brightnessHigh"] = 80
        return object
    }()

    static let tpmsConfigObject: [String: Any] = [
        "lowKpa": 224.0,
        "lowPsi": 32.5,
        "staleAfterMs": 15_000,
    ]

    static let calibrationObject: [String: Any] = [
        "supplyVolts": 5.2,
        "live": [
            "adsPresent": true,
            "bmpPresent": true,
            "fault": false,
            "mapVolts": 2.4180,
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

    static let calibrationObjectCalibrated: [String: Any] = {
        var object = calibrationObject
        var cal = object["calibration"] as! [String: Any]
        cal["version"] = 2
        object["calibration"] = cal
        return object
    }()

    static let logsObject: [String: Any] = [
        "samples": [
            ["tMs": 1000, "psi": 1.5, "peakPsi": 2.0, "zone": "BOOST", "demo": false],
            ["tMs": 2000, "psi": -0.5, "peakPsi": 2.0, "zone": "VAC", "demo": false],
        ],
    ]
}
