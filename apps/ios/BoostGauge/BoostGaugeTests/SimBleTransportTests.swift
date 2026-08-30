import XCTest
@testable import BoostGauge

final class SimBleTransportTests: XCTestCase {
    func testStateMatchesFirmwareShape() async throws {
        let sim = SimBleTransport()
        let response = try await sim.get("state")
        XCTAssertEqual(response.status, 200)
        let state = try JSONDecoder().decode(GaugeState.self, from: response.body)
        XCTAssertEqual(state.tpms?.wheels.count, 4)
        XCTAssertNotNil(state.sensors)
        XCTAssertNotNil(state.obd)
        XCTAssertTrue(state.demo)
        XCTAssertTrue(state.zone == .vacuum || state.zone == .boost)
    }

    func testThemesFullListAndMutation() async throws {
        let sim = SimBleTransport()
        let themes = try JSONDecoder().decode(ThemeList.self, from: (try await sim.get("themes")).body)
        XCTAssertEqual(themes.themes?.count, 5)
        XCTAssertEqual(themes.themes?.first?.name, "Dyno Cell")
        XCTAssertEqual(themes.activeThemeId, "neon")

        let putActive = try await sim.send("PUT", path: "themes/active", body: ["id": "vault-tec"])
        XCTAssertEqual(putActive.status, 200)
        let after = try JSONDecoder().decode(ThemeList.self, from: putActive.body)
        XCTAssertEqual(after.activeThemeId, "vault-tec")

        let putConfig = try await sim.send("PUT", path: "themes/config", body: [
            "id": "neon", "neonLayout": 2,
            "colors": ["boost": "#ff00aa"],
        ])
        XCTAssertEqual(putConfig.status, 200)
        let configured = try JSONDecoder().decode(ThemeList.self, from: putConfig.body)
        let neon = try XCTUnwrap(configured.themes?.first { $0.id == "neon" })
        XCTAssertEqual(neon.colors?.boost, "#ff00aa")
        XCTAssertEqual(neon.customized, true)
        XCTAssertEqual(configured.neonLayout, 2)
    }

    func testThemeResetRestoresDefaults() async throws {
        let sim = SimBleTransport()
        _ = try await sim.send("PUT", path: "themes/config", body: [
            "id": "neon", "colors": ["boost": "#ff00aa"],
        ])
        let reset = try await sim.send("PUT", path: "themes/config", body: ["id": "neon", "reset": true])
        let list = try JSONDecoder().decode(ThemeList.self, from: reset.body)
        let neon = try XCTUnwrap(list.themes?.first { $0.id == "neon" })
        XCTAssertEqual(neon.colors?.boost, "#c4172e")
        XCTAssertEqual(neon.customized, false)
    }

    func testPagePutRoundTrips() async throws {
        let sim = SimBleTransport()
        let response = try await sim.send("PUT", path: "page", body: ["page": 1])
        let object = try response.jsonObject()
        XCTAssertEqual(object["activePage"] as? Int, 1)
        let state = try JSONDecoder().decode(GaugeState.self, from: (try await sim.get("state")).body)
        XCTAssertEqual(state.activePage, 1)
    }

    func testLogsRingIsDense() async throws {
        let sim = SimBleTransport()
        // The capped BLE Log characteristic always carries the 8-sample BGL1
        // diagnostic window, regardless of the requested limit.
        let capped = try await sim.readLogSamples(limit: 18_000)
        XCTAssertEqual(capped.count, 8)
        let bounded = try await sim.readLogSamples(limit: 10)
        XCTAssertEqual(bounded.count, 8)
        let readLog = try await sim.readLog()
        XCTAssertEqual(readLog.count, 8)
        // The /logs route honors ?limit=: no limit = the full one-hour ring,
        // limit=1500 = the last 5 minutes at the 5 Hz log rate.
        let full = try JSONDecoder().decode(LogResponse.self, from: (try await sim.get("logs")).body)
        XCTAssertEqual(full.samples.count, 18_000)
        let window = try JSONDecoder().decode(LogResponse.self, from: (try await sim.get("logs?limit=1500")).body)
        XCTAssertEqual(window.samples.count, 1500)
        XCTAssertTrue(["VAC", "BOOST"].contains(window.samples.last?.zone ?? ""))
    }

    func testLiveStatusStreamYields() async throws {
        let sim = SimBleTransport()
        let stream = sim.liveStatusStream()
        var iterator = stream.makeAsyncIterator()
        guard let result = await iterator.next(), case .success(let data) = result else {
            return XCTFail("expected a state sample")
        }
        let state = try JSONDecoder().decode(GaugeState.self, from: data)
        XCTAssertNotNil(state.tpms)
    }

    func testDeviceInfoIsBLE() async {
        let info = SimBleTransport().readDeviceInfo()
        XCTAssertEqual(info.name, "BoostGauge")
        XCTAssertEqual(info.firmware, "sim")
        XCTAssertEqual(info.api, 1)
    }

    func testCalibrationPayloadAndTimezonePost() async throws {
        let sim = SimBleTransport()
        let calibration = try JSONDecoder().decode(Calibration.self, from: (try await sim.get("sensors/calibration")).body)
        XCTAssertEqual(calibration.live?.mapVolts, 1.4180)
        XCTAssertEqual(calibration.calibration?.offsetPsi, 0.290)
        XCTAssertEqual(calibration.calibration?.samples, 120)

        let time = try await sim.send("POST", path: "time", body: ["timezoneOffsetMinutes": -480, "timezoneTz": "UTC8"])
        XCTAssertEqual(time.status, 200)
    }

    func testTPMSDrivesLowWheelOverTime() async throws {
        let sim = SimBleTransport()
        // A fresh sim starts its low-pressure dip window at t=0 (wheel FL); by
        // ~2.5 s in, that wheel must have sagged at or below lowPsi so the
        // native TPMS card renders the amber low-pressure state.
        try await Task.sleep(nanoseconds: 2_500_000_000)
        let state = try JSONDecoder().decode(GaugeState.self, from: (try await sim.get("state")).body)
        let tpms = try XCTUnwrap(state.tpms)
        let lowPsi = try XCTUnwrap(tpms.lowPsi)
        let lowWheels = tpms.wheels.filter { $0.valid && $0.psi <= lowPsi }
        XCTAssertFalse(lowWheels.isEmpty, "at least one wheel must sag toward lowPsi for amber iteration")
        XCTAssertLessThanOrEqual(lowWheels.first?.psi ?? 99.0, lowPsi)
    }
}