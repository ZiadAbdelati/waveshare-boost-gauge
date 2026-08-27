import XCTest
@testable import BoostGauge

/// View models mutate `@Published` on the main thread (the app funnels
/// transport callbacks through `MainActor.run`; `assertMainThread` enforces it
/// in DEBUG). XCTest async methods are not MainActor by default, so the class
/// must be main actor-isolated for the tests to exercise that contract.
@MainActor
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

    func testStatusViewModelReadsPersistedTPMSBLEFlag() async throws {
        let transport = FakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = StatusViewModel()
        vm.reset(transport: transport)
        await vm.forceRefresh()
        await vm.loadThemeNames(transport)
        XCTAssertEqual(vm.tpmsBleEnabled, true)
    }

    func testLogPressureChartSeriesIsOneValuePerColumn() {
        // 100 samples into 4 columns must reduce to a SINGLE series (one value
        // per column), never a min/max envelope pair. Regression: the old
        // envelope stroked both extrema as separate lines, which drew two
        // distinct squiggles on a dense sweep.
        let values = (0..<100).map { Double($0) }
        let series = LogPressureChart.series(values, columns: 4)
        XCTAssertEqual(series.count, 4)
        XCTAssertEqual(series, [24, 49, 74, 99])
    }

    func testLogPressureChartSeriesFlatSingleSample() {
        let series = LogPressureChart.series([3.5], columns: 5)
        XCTAssertEqual(series, [3.5, 3.5, 3.5, 3.5, 3.5])
    }

    func testLogPressureChartSeriesPreservesFastSweepExtremes() {
        // The board's demo fast-sweep triangle is the fixture behind the
        // two-line bug: per-column minima and maxima sat ~6 psi apart. The
        // single series must hold the FULL raw extent (peaks/vacuum not
        // rounded off) and contain no second value track.
        let columns = 350
        var values = [Double]()
        let period = 25.0
        for index in 0..<1500 {
            let phase = (Double(index).truncatingRemainder(dividingBy: period) / period) * 2 - 1
            values.append(phase * 12.0)
        }
        let series = LogPressureChart.series(values, columns: columns)
        XCTAssertEqual(series.count, columns)
        XCTAssertEqual(series.min(), values.min())
        XCTAssertEqual(series.max(), values.max())
    }

    func testLogPressureChartDomainUsesNiceStepAndSpansData() {
        // Board fixture extent: -14.49..9.98 psi. The plot domain must be
        // snapped to round gridlines (5 psi steps) while still containing the
        // raw data, and always include 0 so the dashed zero line is meaningful.
        let domain = LogPressureChart.chartDomain(minimum: -14.49, maximum: 9.98)
        XCTAssertLessThanOrEqual(domain.min, -14.49)
        XCTAssertGreaterThanOrEqual(domain.max, 9.98)
        XCTAssertEqual(domain.min.truncatingRemainder(dividingBy: domain.step), 0)
        XCTAssertEqual(domain.max.truncatingRemainder(dividingBy: domain.step), 0)
        XCTAssertLessThanOrEqual(domain.min, 0)
        XCTAssertGreaterThanOrEqual(domain.max, 0)
        let ticks = LogPressureChart.psiTicks(domain: domain)
        XCTAssertGreaterThanOrEqual(ticks.count, 4)
        XCTAssertEqual(ticks.first, domain.min)
        XCTAssertEqual(ticks.last, domain.max)
    }

    func testLogPressureChartNiceStepAndTickLabels() {
        XCTAssertEqual(LogPressureChart.niceStep(0.3), 0.5)
        XCTAssertEqual(LogPressureChart.niceStep(1.0), 1.0)
        XCTAssertEqual(LogPressureChart.niceStep(4.9), 5.0)
        XCTAssertEqual(LogPressureChart.niceStep(6.2), 10.0)
        XCTAssertEqual(LogPressureChart.tickLabel(0), "0")
        XCTAssertEqual(LogPressureChart.tickLabel(-15), "-15")
        XCTAssertEqual(LogPressureChart.tickLabel(2.5), "2.5")
    }

    func testLogPressureChartSampleIndexMapsColumnToRawSample() {
        // Column → the raw sample it represents (the column's last), so the
        // crosshair reads the exact psi + timestamp the line draws.
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 0, count: 100, columns: 4), 24)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 3, count: 100, columns: 4), 99)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 0, count: 1, columns: 5), 0)
    }

    func testLogPressureChartSampleIndexClampsBelowRange() {
        // Below-range columns clamp to the first column's last sample
        // (column 0 → index 24 for 100/4; column 0 → index 1 for 10/5).
        XCTAssertEqual(LogPressureChart.sampleIndex(column: -5, count: 100, columns: 4), 24)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: -1, count: 10, columns: 5), 1)
    }

    func testLogPressureChartSampleIndexClampsAboveRange() {
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 99, count: 100, columns: 4), 99)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 10, count: 10, columns: 5), 9)
    }

    func testLogPressureChartSampleIndexEmptyArray() {
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 0, count: 0, columns: 4), 0)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: 5, count: 0, columns: 0), 0)
        XCTAssertEqual(LogPressureChart.sampleIndex(column: -1, count: 0, columns: 5), 0)
        XCTAssertTrue(LogPressureChart.series([], columns: 4).isEmpty)
    }

    func testLogPressureChartRelativeTimeLabels() {
        XCTAssertEqual(LogPressureChart.relativeTime(10_000, newestMs: 10_200), "now")
        XCTAssertEqual(LogPressureChart.relativeTime(10_000, newestMs: 240_000), "-3:50")
        XCTAssertEqual(LogPressureChart.relativeTime(10_000, newestMs: 310_000), "-5:00")
        XCTAssertEqual(LogPressureChart.relativeTime(nil, newestMs: 310_000), "")
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

    func testThemesViewModelDecodesThemeEditorFields() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        // New editor fields must decode from /themes and reach the published flags.
        XCTAssertTrue(vm.bigDigitStaticBg)
        XCTAssertFalse(vm.bigDigitColorText)
        XCTAssertEqual(vm.bigDigitStaticColor, "#000000")
        XCTAssertEqual(vm.bigDigitTextColor, "#ffffff")
        XCTAssertEqual(vm.vaultVignette, 60)
        XCTAssertEqual(vm.neonLayout, 1)
        XCTAssertEqual(vm.neonFont, 1)
        XCTAssertEqual(vm.neonPreset, 2)
    }

    func testThemesViewModelPreviewPayloadCarriesAllPaletteRolesAndSettings() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        let theme = try XCTUnwrap(vm.themes.first)
        let payload = vm.previewPayload(for: theme)
        let themeObject = try XCTUnwrap(payload["theme"] as? [String: Any])
        let colors = try XCTUnwrap(themeObject["colors"] as? [String: String])
        // The canonical mirror needs every palette role so the web renderer can
        // build the exact firmware face; all eight must be present.
        for key in ThemesViewModel.paletteKeys {
            XCTAssertNotNil(colors[key], "missing palette role \(key) in preview payload")
        }
        XCTAssertEqual(colors["boost"], "#B8F35A")
        let settings = try XCTUnwrap(payload["settings"] as? [String: Any])
        XCTAssertNotNil(settings["bigDigitStaticColor"])
        XCTAssertNotNil(settings["bigDigitTextColor"])
        XCTAssertNotNil(settings["neonFont"])
        XCTAssertNotNil(settings["neonPreset"])
    }

    func testThemesViewModelSaveOptionsSendsPerThemeColorsFromServer() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        transport.responses["themes/config"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        // Load seeds themeColorEdits from the server, so an unedited save must
        // echo the server's vacuum/boost/overboost, never a black fallback.
        await vm.saveOptions(for: "dyno-cell")
        XCTAssertEqual(transport.recordedPaths.last, "themes/config")
        let body = try XCTUnwrap(transport.recordedBodies.last)
        XCTAssertEqual(body["id"] as? String, "dyno-cell")
        XCTAssertEqual(body["arcGradient"] as? Bool, false)
        let colors = try XCTUnwrap(body["colors"] as? [String: String])
        XCTAssertEqual(colors["vacuum"], "#4DD2FF")
        XCTAssertEqual(colors["boost"], "#B8F35A")
        XCTAssertEqual(colors["overboost"], "#FF4F6D")
    }

    func testThemesViewModelSaveOptionsBigDigitSendsColorsAndReflectsEdit() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        transport.responses["themes/config"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        // An edited per-theme color must win over the server value.
        vm.setColor("#112233", for: "big-digit", key: "vacuum")
        await vm.saveOptions(for: "big-digit")
        let body = try XCTUnwrap(transport.recordedBodies.last)
        XCTAssertEqual(body["id"] as? String, "big-digit")
        XCTAssertTrue(body["bigDigitStaticBg"] as? Bool ?? false)
        XCTAssertEqual(body["bigDigitStaticColor"] as? String, "#000000")
        XCTAssertEqual(body["bigDigitTextColor"] as? String, "#ffffff")
        let colors = try XCTUnwrap(body["colors"] as? [String: String])
        XCTAssertEqual(colors["vacuum"], "#112233")
    }

    func testThemesViewModelResetColorsSendsResetFlag() async throws {
        let transport = FakeTransport()
        transport.responses["themes"] = FakeTransport.resp(200, Fixtures.themesObject)
        transport.responses["themes/config"] = FakeTransport.resp(200, Fixtures.themesObject)
        let vm = ThemesViewModel()
        vm.reset(transport: transport)
        await vm.load()
        await vm.resetColors(for: "dyno-cell")
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "themes/config")
        let body = try XCTUnwrap(transport.recordedBodies.last)
        XCTAssertEqual(body["id"] as? String, "dyno-cell")
        XCTAssertEqual(body["reset"] as? Bool, true)
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
        // appBle is still decoded from GET /config for backward compat, but the
        // app no longer exposes it — toggling via BLE would trap the app with no
        // UI to re-enable, so saveConfig/display must not echo appBle.
        let transport = FakeTransport()
        var enabledConfig = Fixtures.configObject
        enabledConfig["appBle"] = true
        transport.responses["config"] = FakeTransport.resp(200, enabledConfig)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        XCTAssertTrue(vm.appBle)

        transport.responses["config"] = FakeTransport.resp(200, Fixtures.configObject)
        transport.responses["themes/config"] = FakeTransport.resp(200, Fixtures.themesObject)
        vm.brightnessHigh = 80
        await vm.saveDisplay()
        XCTAssertTrue(transport.recordedBodies.contains { $0["brightnessHigh"] as? Int == 80 })
        XCTAssertFalse(transport.recordedBodies.contains { $0["appBle"] != nil }, "Display save must not echo appBle")
    }

    func testSettingsViewModelLoadsOBDStateFromState() async throws {
        let transport = FakeTransport()
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.refreshOBDState()
        XCTAssertEqual(vm.obdState?.state, 3)
        XCTAssertEqual(vm.obdPeerName, "vlinker fd+")
        XCTAssertEqual(vm.obdPeerAddr, "11:22:33:44:55:66")
    }

    func testSettingsViewModelForgetOBDPeerSendsObdForget() async throws {
        let transport = FakeTransport()
        transport.responses["obd/forget"] = FakeTransport.resp(200, ["ok": true])
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        vm.obdPeerName = "vlinker fd+"
        vm.obdPeerAddr = "11:22:33:44:55:66"
        await vm.forgetOBDPeer()
        XCTAssertEqual(transport.recordedMethods.last, "POST")
        XCTAssertEqual(transport.recordedPaths.last, "obd/forget")
        XCTAssertNil(vm.obdPeerName, "a successful forget clears the stored peer locally")
        XCTAssertNil(vm.obdPeerAddr)
        XCTAssertFalse(vm.isForgettingOBDPeer)
    }

    func testSettingsViewModelForgetOBDPeerSurfacesFailure() async throws {
        let transport = FakeTransport()
        transport.responses["obd/peer"] = FakeTransport.resp(404, ["error": "not found"])
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        vm.obdPeerName = "vlinker fd+"
        await vm.forgetOBDPeer()
        XCTAssertNotNil(vm.errorMessage, "an unsupported/absent firmware route must not silently clear the peer")
        XCTAssertEqual(vm.obdPeerName, "vlinker fd+")
    }

    func testOBDPhaseMapping() {
        func summary(_ state: Int?, peer: String? = nil) -> OBDSummary {
            OBDSummary(state: state, lastError: 0, peer: peer, peerAddr: "aa:bb:cc:dd:ee:ff",
                       uptimeMs: nil, ageMs: nil, valid: nil, lastReply: nil, protocolName: nil,
                       rpm: nil, speedKph: nil, coolantC: nil, mapKpa: nil, iatC: nil,
                       throttlePct: nil, mafGps: nil, fuelPct: nil, batteryV: nil)
        }
        XCTAssertEqual(summary(nil).phase, .idle)
        XCTAssertEqual(summary(0).phase, .idle)
        XCTAssertEqual(summary(1).phase, .scanning)
        XCTAssertEqual(summary(2, peer: "vlinker fd+").phase, .connecting(name: "vlinker fd+"))
        XCTAssertEqual(summary(3, peer: "vlinker fd+").phase, .connecting(name: "vlinker fd+"))
        XCTAssertEqual(summary(4).phase, .connected)
        XCTAssertEqual(summary(5, peer: "vlinker fd+").phase, .connecting(name: "vlinker fd+"))
        XCTAssertEqual(summary(5).phase, .scanning)
    }

    func testSettingsViewModelSyncTimezoneSendsNoEpoch() async throws {
        let transport = FakeTransport()
        transport.responses["time"] = FakeTransport.resp(200, ["ok": true])
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.syncTimezone()
        XCTAssertEqual(transport.recordedMethods.last, "POST")
        XCTAssertEqual(transport.recordedPaths.last, "time")
        let body = try XCTUnwrap(transport.recordedBodies.last)
        XCTAssertNil(body["epochMs"], "timezone-only sync must never send the phone epoch (the gauge RTC is the time authority)")
        XCTAssertNotNil(body["timezoneOffsetMinutes"])
        XCTAssertNotNil(body["timezoneTz"])
    }

    func testSettingsViewModelApplyTimezoneOptionPostsTimeThenSavesConfig() async throws {
        let transport = FakeTransport()
        transport.responses["config"] = FakeTransport.resp(200, Fixtures.configObject)
        transport.responses["time"] = FakeTransport.resp(200, ["ok": true])
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        // The device config PUT echoes the saved values, so the fake must echo
        // the pacific timezone or applyConfig would reset the fields.
        var saved = Fixtures.configObject
        saved["timezoneOffsetMinutes"] = -480
        saved["timezoneTz"] = "PST8PDT,M3.2.0/2,M11.1.0/2"
        transport.responses["config"] = FakeTransport.resp(200, saved)
        await vm.applyTimezoneOption("pacific")
        XCTAssertEqual(vm.timezoneSelection, "pacific")
        XCTAssertEqual(vm.timezoneOffset, -480)
        XCTAssertEqual(vm.timezoneTZ, "PST8PDT,M3.2.0/2,M11.1.0/2")

        // Timezone-only POST must carry no epoch, then the normal config save.
        let timeBody = try XCTUnwrap(
            transport.recordedBodies.first { $0["timezoneTz"] as? String == "PST8PDT,M3.2.0/2,M11.1.0/2" },
            "timezone-only POST should be recorded"
        )
        XCTAssertNil(timeBody["epochMs"])
        XCTAssertEqual(timeBody["timezoneOffsetMinutes"] as? Int, -480)
        XCTAssertEqual(timeBody["timezoneTz"] as? String, "PST8PDT,M3.2.0/2,M11.1.0/2")
        XCTAssertEqual(transport.recordedMethods.last, "PUT")
        XCTAssertEqual(transport.recordedPaths.last, "config")
        XCTAssertEqual(transport.recordedBodies.last?["timezoneOffsetMinutes"] as? Int, -480)
    }

    func testSettingsViewModelTimezoneSelectionMapsFromLoadedConfig() async throws {
        let transport = FakeTransport()
        var eastern = Fixtures.configObject
        eastern["timezoneOffsetMinutes"] = -300
        eastern["timezoneTz"] = "EST5EDT,M3.2.0/2,M11.1.0/2"
        transport.responses["config"] = FakeTransport.resp(200, eastern)
        let vm = SettingsViewModel()
        vm.reset(transport: transport)
        await vm.loadAll()
        XCTAssertEqual(vm.timezoneSelection, "eastern")

        // An unlisted POSIX string falls back to the Custom path.
        var custom = Fixtures.configObject
        custom["timezoneTz"] = "MyZone5"
        transport.responses["config"] = FakeTransport.resp(200, custom)
        await vm.loadAll()
        XCTAssertEqual(vm.timezoneSelection, SettingsViewModel.customTimezoneID)
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
        transport.responses["logs?limit=1500"] = FakeTransport.resp(200, Fixtures.logsObject)
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 1500)
        XCTAssertEqual(vm.samples.count, 2)
        XCTAssertEqual(vm.samples[0].zone, "BOOST")
        XCTAssertEqual(vm.scopeLabel, "Last 5 minutes · 2 samples")

        let csv = vm.csvText()
        let lines = csv.split(separator: "\n")
        XCTAssertEqual(lines[0], "timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo")
        XCTAssertTrue(lines[1].contains(",1.50,2.00,BOOST,0"))
    }

    func testLogsViewModelFallsBackToBGL1Window() async throws {
        // The 5-minute /logs?limit=1500 window is the primary fetch; when it is
        // unavailable over a capped BLE link (no HTTP host), the 8-sample BGL1
        // diagnostic window is the last-resort error state.
        let transport = BleLogFallbackTransport()
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 1500)
        XCTAssertEqual(vm.samples.count, 8)
        XCTAssertTrue(vm.scopeLabel.hasPrefix("BLE diagnostic window"))
        XCTAssertNotNil(vm.errorMessage, "degraded fallback must be reported as an error state")
    }

    func testLogsViewModelReusesDecodeCacheForUnchangedPayload() async throws {
        // Switching chips back to a window whose device payload is byte-identical
        // must skip the JSON decode: only the raw-body fetch happens again.
        let transport = FakeTransport()
        transport.responses["logs?limit=1500"] = FakeTransport.resp(200, Fixtures.logsObject)
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 1500)
        XCTAssertFalse(vm.lastLoadUsedCache, "first load must decode")
        XCTAssertEqual(vm.dataRevision, 1)
        await vm.load(limit: 1500)
        XCTAssertTrue(vm.lastLoadUsedCache, "identical payload must reuse the previous decode")
        XCTAssertEqual(vm.dataRevision, 2, "revision still bumps so the chart invalidates")
        XCTAssertEqual(vm.samples.count, 2)
    }

    func testLogsViewModelInvalidatesDecodeCacheWhenPayloadChanges() async throws {
        // A changed payload for the same limit must re-decode (fresh data wins).
        let transport = FakeTransport()
        transport.responses["logs?limit=1500"] = FakeTransport.resp(200, Fixtures.logsObject)
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 1500)
        XCTAssertFalse(vm.lastLoadUsedCache)
        var changed = Fixtures.logsObject
        var samples = changed["samples"] as! [[String: Any]]
        samples.append(["tMs": 3000, "psi": 1.1, "peakPsi": 2.0, "zone": "BOOST", "demo": false])
        changed["samples"] = samples
        transport.responses["logs?limit=1500"] = FakeTransport.resp(200, changed)
        await vm.load(limit: 1500)
        XCTAssertFalse(vm.lastLoadUsedCache, "changed payload must invalidate the cache")
        XCTAssertEqual(vm.samples.count, 3)
    }

    func testLogsViewModelClearsDecodeCacheOnTransportChange() async throws {
        // A different transport/device is a different identity: the cache must
        // not survive reset even for a byte-identical payload.
        let transport = FakeTransport()
        transport.responses["logs?limit=1500"] = FakeTransport.resp(200, Fixtures.logsObject)
        transport.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        let vm = LogsViewModel()
        vm.reset(transport: transport)
        await vm.load(limit: 1500)
        let second = FakeTransport()
        second.responses["logs?limit=1500"] = FakeTransport.resp(200, Fixtures.logsObject)
        second.responses["state"] = FakeTransport.resp(200, Fixtures.stateObject)
        vm.reset(transport: second)
        await vm.load(limit: 1500)
        XCTAssertFalse(vm.lastLoadUsedCache, "transport change must clear the decode cache")
    }

    func testLogPressureChartColumnCacheRebuildsOnlyOnKeyChange() {
        // The crosshair path must read the cached columns: a drag re-render with
        // an unchanged (revision, column count) must not rebuild the series.
        let cache = LogPressureChart.ColumnCache()
        let samples = (0..<100).map { index in
            LogSample(tMs: Int64(index), epochTs: nil, psi: Double(index), peakPsi: nil, zone: "BOOST", demo: true)
        }
        cache.update(revision: 1, samples: samples, columns: 10)
        XCTAssertEqual(cache.rebuildCount, 1)
        XCTAssertEqual(cache.series.count, 10)
        XCTAssertEqual(cache.series, LogPressureChart.series(samples.map(\.psi), columns: 10))

        cache.update(revision: 1, samples: samples, columns: 10)
        XCTAssertEqual(cache.rebuildCount, 1, "unchanged key must reuse cached columns")

        cache.update(revision: 2, samples: samples, columns: 10)
        XCTAssertEqual(cache.rebuildCount, 2, "revision bump (fresh data) must rebuild at the same column count")

        cache.update(revision: 2, samples: samples, columns: 5)
        XCTAssertEqual(cache.rebuildCount, 3, "column count change (rotation/width) must rebuild")
        XCTAssertEqual(cache.series.count, 5)
    }

    func testLogPressureChartColumnCacheTracksDomainExtrema() {
        let cache = LogPressureChart.ColumnCache()
        cache.update(revision: 1, samples: [
            LogSample(tMs: 0, epochTs: nil, psi: -12.5, peakPsi: nil, zone: "VAC", demo: true),
            LogSample(tMs: 1, epochTs: nil, psi: 9.5, peakPsi: nil, zone: "BOOST", demo: true),
        ], columns: 4)
        XCTAssertEqual(cache.minimum, -12.5)
        XCTAssertEqual(cache.maximum, 9.5)
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
    override func get(_ path: String) async throws -> Resp {
        if path.hasPrefix("logs") { throw TransportError.badLogFormat }
        return try await super.get(path)
    }

    override func readLogSamples(limit: Int) async throws -> [LogSample] {
        // The capped BLE Log characteristic: the last 8 BGL1 samples.
        (0..<8).map { index in
            LogSample(tMs: Int64(index) * 200, epochTs: nil, psi: 2.0, peakPsi: 3.0, zone: "BOOST", demo: true)
        }
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
        "bigDigitStaticBg": true,
        "bigDigitColorText": false,
        "bigDigitStaticColor": "#000000",
        "bigDigitTextColor": "#ffffff",
        "arcGradient": false,
        "hudGradient": false,
        "hudTrueBlack": false,
        "vaultFace": "#05281a",
        "vaultVignette": 60,
        "vaultNeedleRed": false,
        "vaultNeedleTail": false,
        "neonLayout": 1,
        "neonFont": 1,
        "neonPreset": 2,
        "neonMarqueeSpin": false,
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
