import XCTest

/// Simulator-only acceptance for the BLE-mode UI driven by `SimBleTransport`
/// (`-e2eSimBle`). No board, no physical phone: the sim transport serves the
/// fixture themes/config/state and a dense one-hour log ring, so the Themes
/// palette, native Status TPMS card and the dense Logs graph can be iterated
/// on and verified in the simulator.
final class SimAcceptanceUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        // Orientation persists device-wide across launches; every test here
        // assumes portrait, so force it back even if a screenshot suite left
        // the simulator landscape.
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    func testThemesShowsFullPaletteAndApplyDoesNotOpenLayoutPicker() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "themes"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 15))

        // Full five-theme fixture list (scroll the lazy List to reach the tail).
        for name in ["Dyno Cell", "Vault-Tec", "Night City", "Big Digit", "Neon"] {
            let row = app.cells.containing(.staticText, identifier: name).firstMatch
            XCTAssertTrue(waitForElement(row, app: app), "theme row \(name)")
            bringIntoView(row, app: app)
        }

        // Expand Dyno Cell: only the three editable zone pickers remain inside
        // the "Zone colors" group — the static Face/Track/Text/Muted rows are
        // gone, and the group is retitled.
        let dynoRow = app.cells.containing(.staticText, identifier: "Dyno Cell").firstMatch
        XCTAssertTrue(dynoRow.waitForExistence(timeout: 8))
        bringIntoView(dynoRow, app: app)
        let dynoChevron = dynoRow.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(dynoChevron.waitForExistence(timeout: 6))
        dynoChevron.tap()
        let zoneGroup = app.staticTexts["Zone colors"]
        XCTAssertTrue(zoneGroup.waitForExistence(timeout: 6), "group retitled to Zone colors")
        for key in ["vacuum", "boost", "overboost"] {
            let picker = app.descendants(matching: .any)
                .matching(NSPredicate(format: "identifier == %@", "theme.dyno-cell.color.\(key)"))
                .firstMatch
            XCTAssertTrue(waitForElement(picker, app: app), "dyno-cell color well \(key)")
        }
        XCTAssertFalse(app.staticTexts["Face"].exists, "static Face row removed")
        XCTAssertFalse(app.staticTexts["Muted"].exists, "static Muted row removed")
        XCTAssertFalse(app.staticTexts["Track"].exists, "static Track row removed")
        attach("sim-themes-dyno-cell-editor", app.screenshot())
        let reDynoChevron = app.cells.containing(.staticText, identifier: "Dyno Cell").firstMatch
            .buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        if reDynoChevron.exists { reDynoChevron.tap() }

        // Vault-Tec's editor dropdown owns the two needle toggles (they must
        // live ONLY here, never in Settings → Theme & demo).
        let vaultRow = app.cells.containing(.staticText, identifier: "Vault-Tec").firstMatch
        XCTAssertTrue(vaultRow.waitForExistence(timeout: 8))
        bringIntoView(vaultRow, app: app)
        let vaultChevron = vaultRow.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(vaultChevron.waitForExistence(timeout: 6))
        vaultChevron.tap()
        XCTAssertTrue(app.switches["Red needle"].waitForExistence(timeout: 6), "vault-tec: Red needle toggle")
        XCTAssertTrue(app.switches["Counterweight tail"].exists, "vault-tec: Counterweight tail toggle")
        attach("sim-themes-vault-tec-editor", app.screenshot())
        let reVaultChevron = app.cells.containing(.staticText, identifier: "Vault-Tec").firstMatch
            .buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        if reVaultChevron.exists { reVaultChevron.tap() }

        // Big Digit's editor dropdown owns the static-background toggle.
        let bigDigitRow = app.cells.containing(.staticText, identifier: "Big Digit").firstMatch
        XCTAssertTrue(bigDigitRow.waitForExistence(timeout: 8))
        bringIntoView(bigDigitRow, app: app)
        let bigDigitChevron = bigDigitRow.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(bigDigitChevron.waitForExistence(timeout: 6))
        bigDigitChevron.tap()
        XCTAssertTrue(app.switches["Static background"].waitForExistence(timeout: 6), "big-digit: Static background toggle")
        attach("sim-themes-big-digit-editor", app.screenshot())
        let reBigDigitChevron = app.cells.containing(.staticText, identifier: "Big Digit").firstMatch
            .buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        if reBigDigitChevron.exists { reBigDigitChevron.tap() }

        // Expand Neon and reach its editor.
        let neonRow = app.cells.containing(.staticText, identifier: "Neon").firstMatch
        XCTAssertTrue(neonRow.waitForExistence(timeout: 8))
        bringIntoView(neonRow, app: app)
        let chevron = neonRow.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(chevron.waitForExistence(timeout: 6))
        chevron.tap()

        // Color wells are exposed as ColorWell/Button by identifier.
        for key in ["vacuum", "boost", "overboost"] {
            let picker = app.descendants(matching: .any)
                .matching(NSPredicate(format: "identifier == %@", "theme.neon.color.\(key)"))
                .firstMatch
            XCTAssertTrue(waitForElement(picker, app: app), "neon color well \(key)")
        }

        let apply = app.buttons["Apply Neon options"]
        XCTAssertTrue(apply.waitForExistence(timeout: 6))
        bringIntoView(apply, app: app)
        XCTAssertTrue(apply.isHittable, "Apply button must be a real, hittable button")
        apply.tap()

        // saveOptions fired (a PUT to the sim) and the Layout Picker above did
        // NOT open its menu: none of its option rows may appear as a button.
        sleep(1)
        XCTAssertTrue(app.buttons["Apply Neon options"].exists, "apply button still visible after save")
        XCTAssertFalse(app.buttons["Tube"].exists, "Layout picker menu must not open on Apply tap")
        XCTAssertFalse(app.buttons["Segments"].exists, "Layout picker menu must not open on Apply tap")
        XCTAssertFalse(app.buttons["Marquee"].exists, "Layout picker menu must not open on Apply tap")
        attach("sim-themes-neon-editor", app.screenshot())
    }

    func testStatusShowsNativeTPMSCard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "status"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 15))

        // Native card must expose the four wheel labels and the low-pressure
        // footer, proving it is NOT a web view (a WKWebView exposes no SwiftUI
        // accessibility elements). The 2x2 grid sits below the gauge card, so
        // scroll the ScrollView to bring the capsules into view.
        for label in ["FL", "FR", "RL", "RR"] {
            let capsule = app.staticTexts[label].firstMatch
            XCTAssertTrue(waitForElement(capsule, app: app, scroll: .down), "TPMS capsule \(label)")
        }
        XCTAssertTrue(app.staticTexts["low 26.0"].exists, "TPMS low-pressure threshold footer")
        XCTAssertTrue(app.staticTexts["TPMS status 0"].exists, "TPMS status footer")
        attach("sim-status-native-tpms", app.screenshot())
    }

    func testLogsShowsFiveMinuteWindow() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "logs"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 15))

        // The graph fetches /logs?limit=1500 (last 5 minutes at the 5 Hz log
        // rate) on both transports — never the full-hour ring, whose payload
        // times out over BLE and renders as meaningless top/bottom bands. The
        // min/max row is computed over the 5-minute waveform window.
        XCTAssertTrue(app.staticTexts["Last 5 minutes · 1500 samples"].waitForExistence(timeout: 25),
                      "scope label shows the 5-minute window")
        // The taller axis-labeled chart pushes the min/max caption row below
        // the fold; reveal it before asserting (Lazy List materializes rows).
        for _ in 0..<3 where !app.staticTexts["Min -10.12 psi"].exists {
            app.swipeUp()
            usleep(250_000)
        }
        XCTAssertTrue(app.staticTexts["Min -10.12 psi"].waitForExistence(timeout: 8), "graph min over 5-minute window")
        XCTAssertTrue(app.staticTexts["Max 7.02 psi"].exists, "graph max over 5-minute window")
        XCTAssertFalse(app.staticTexts["Samples · newest first"].exists, "per-sample list must be removed")
        attach("sim-logs-five-minute-window", app.screenshot())
    }

    func testLogsWindowChipsSwitchScopeAndReload() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "logs"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 15))

        // Default window is 5m/1500 (never the full-hour ring).
        XCTAssertTrue(app.staticTexts["Last 5 minutes · 1500 samples"].waitForExistence(timeout: 25),
                      "default scope is the 5-minute window")

        // 1m chip → /logs?limit=300.
        let oneMinute = app.buttons["logsWindow.300"]
        XCTAssertTrue(oneMinute.waitForExistence(timeout: 8), "1m chip")
        oneMinute.tap()
        XCTAssertTrue(app.staticTexts["Last 1 minute · 300 samples"].waitForExistence(timeout: 25),
                      "1m chip reloads to the 1-minute window")
        attach("sim-logs-window-1m", app.screenshot())

        // 15m chip → /logs?limit=4500 (well under the 18,000 full-hour ring).
        let fifteenMinutes = app.buttons["logsWindow.4500"]
        XCTAssertTrue(fifteenMinutes.waitForExistence(timeout: 8), "15m chip")
        fifteenMinutes.tap()
        XCTAssertTrue(app.staticTexts["Last 15 minutes · 4500 samples"].waitForExistence(timeout: 25),
                      "15m chip reloads to the 15-minute window")
        attach("sim-logs-window-15m", app.screenshot())
    }

    func testLogsChartCrosshairAppearsOnPressAndClearsOnRelease() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "logs", "-e2eCrosshair"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["Last 5 minutes · 1500 samples"].waitForExistence(timeout: 25))

        // The -e2eCrosshair fixture pins the readout pill at the newest sample
        // (deterministic to capture); its label carries psi + timestamp.
        let readout = app.staticTexts.matching(
            NSPredicate(format: "label MATCHES %@", "-?[0-9.]+ psi · .+")
        ).firstMatch
        XCTAssertTrue(readout.waitForExistence(timeout: 8), "crosshair readout must show psi + timestamp")
        attach("sim-logs-crosshair", app.screenshot())

        // Pressing the chart moves the finger-owned crosshair (the fixture
        // yields to a real touch); releasing clears it again.
        let chart = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'Pressure history graph'"))
            .firstMatch
        XCTAssertTrue(chart.waitForExistence(timeout: 8), "chart accessibility element")
        let point = chart.coordinate(withNormalizedOffset: CGVector(dx: 0.5, dy: 0.5))
        point.press(forDuration: 0.8)
        usleep(400_000)
        XCTAssertFalse(readout.exists, "crosshair readout must clear on release")
    }

    private enum ScrollDirection { case down, up }

    private func waitForElement(_ element: XCUIElement, app: XCUIApplication, timeout: TimeInterval = 10,
                                scroll: ScrollDirection? = nil) -> Bool {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if element.exists { return true }
            switch scroll {
            case .down: app.swipeUp()
            case .up: app.swipeDown()
            case nil: break
            }
            usleep(200_000)
        }
        return element.exists
    }

    func testStatusTPMSValuesUpdateLiveAndShowLowPressure() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "status"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 15))

        // The native capsules expose their psi in the accessibility label.
        let capsule = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'FL '")).firstMatch
        XCTAssertTrue(capsule.waitForExistence(timeout: 12))
        let first = capsule.label
        // The sim rotates WHICH wheel dips low each cycle, so watch every
        // capsule — any amber state proves the low-pressure rendering.
        let capsules = ["FL ", "FR ", "RL ", "RR "].map { prefix in
            app.descendants(matching: .any)
                .matching(NSPredicate(format: "label BEGINSWITH %@", prefix)).firstMatch
        }

        // ~40% of each 14 s cycle one wheel reads at/below lowPsi; over a
        // ~20 s window the label must change (live stream) and must show the
        // low-pressure marker at least once.
        var sawChange = false
        var sawLow = capsules.contains { $0.label.contains("low pressure") }
        let deadline = Date().addingTimeInterval(20)
        while Date() < deadline {
            sleep(2)
            if capsule.label != first { sawChange = true }
            for c in capsules where c.exists && c.label.contains("low pressure") {
                sawLow = true
                attach("sim-status-amber-moment", app.screenshot())
            }
            if sawChange && sawLow { break }
        }
        XCTAssertTrue(sawChange, "TPMS psi must update live: first='\(first)'")
        XCTAssertTrue(sawLow, "the amber low-pressure state must appear: first='\(first)'")
        attach("sim-status-tpms-live", app.screenshot())
    }

    func testStatusHoldForHostCapture() throws {
        // Keeps the scrolled-to native TPMS capsules on screen for ~30 s so
        // host-side `xcrun simctl io booted screenshot` frames can capture the
        // amber low-pressure wheel during its ~40% duty-cycle window.
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "status"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 15))
        // Force-scroll the ScrollView (off-screen ScrollView children already
        // exist in the AX tree, so existence-based scrolling never moves).
        for _ in 0..<6 {
            app.swipeUp()
            usleep(400_000)
        }
        let capsule = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'FL '")).firstMatch
        XCTAssertTrue(capsule.waitForExistence(timeout: 8))
        sleep(30)
        XCTAssertTrue(capsule.exists)
    }

    func testSelectBigDigitPreviewShowsCircularBezel() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "themes"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 15))

        // Activate Big Digit so its black face is the preview: tapping the
        // row's title hits the select Button inside the DisclosureGroup label.
        let bigDigit = app.staticTexts["Big Digit"].firstMatch
        XCTAssertTrue(waitForElement(bigDigit, app: app), "Big Digit theme row")
        bringIntoView(bigDigit, app: app)
        bigDigit.tap()
        sleep(2)

        let preview = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'Exact dashboard preview of Big Digit'"))
            .firstMatch
        XCTAssertTrue(preview.waitForExistence(timeout: 8), "big-digit preview should render after select")
        // The tap scrolled the theme row into view; scroll fully back to the top
        // so the whole pod (ring included) sits below the nav bar, then shoot.
        for _ in 0..<10 { app.swipeDown() }
        usleep(300_000)
        attach("sim-themes-preview-bigdigit-bezel", app.screenshot())
    }

    func testSettingsSubPagesAndTimezonePicker() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "settings"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))
        attach("sim-settings-subpages", app.screenshot())

        // Root rows must appear in canonical order (PARITY.md).
        let order = ["Connection", "Display", "Range", "Theme & demo", "Clock & timezone", "TPMS", "OBD2 Scanner"]
        var previousY: CGFloat = 0
        for title in order {
            let row = app.cells.containing(.staticText, identifier: title).firstMatch
            XCTAssertTrue(row.waitForExistence(timeout: 8), "root row \(title)")
            XCTAssertGreaterThan(row.frame.midY, previousY - 1, "root row \(title) must come after its predecessor")
            previousY = row.frame.midY
        }

        // Display sub-page (brightness + dim schedule; no psi fields).
        let displayRow = app.cells.containing(.staticText, identifier: "Display").firstMatch
        XCTAssertTrue(displayRow.waitForExistence(timeout: 8), "Display sub-page row")
        displayRow.tap()
        XCTAssertTrue(app.navigationBars["Display"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.switches["Dim schedule"].exists, "Dim schedule toggle on Display")
        XCTAssertFalse(app.staticTexts["psiMin"].exists, "psiMin must not live on Display")
        attach("sim-settings-display-page", app.screenshot())
        app.navigationBars["Display"].buttons.firstMatch.tap()

        // Range sub-page (the four psi fields).
        let rangeRow = app.cells.containing(.staticText, identifier: "Range").firstMatch
        XCTAssertTrue(rangeRow.waitForExistence(timeout: 8), "Range sub-page row")
        rangeRow.tap()
        XCTAssertTrue(app.navigationBars["Range"].waitForExistence(timeout: 8))
        for key in ["psiMin", "psiMax", "psiOverboost", "zeroAngle"] {
            XCTAssertTrue(app.staticTexts[key].exists, "\(key) field on Range")
        }
        attach("sim-settings-range-page", app.screenshot())
        app.navigationBars["Range"].buttons.firstMatch.tap()

        // Theme & demo sub-page must NOT carry the TPMS BLE link toggle.
        let themeRow = app.cells.containing(.staticText, identifier: "Theme & demo").firstMatch
        XCTAssertTrue(themeRow.waitForExistence(timeout: 8), "Theme & demo sub-page row")
        themeRow.tap()
        XCTAssertTrue(app.navigationBars["Theme & demo"].waitForExistence(timeout: 8))
        XCTAssertFalse(app.switches["TPMS BLE link"].exists, "TPMS BLE link must not live on Theme & demo")
        // Global/demo/debug flags only (PARITY.md): the theme-specific settings
        // must NEVER appear here — they live in the Themes tab editor dropdowns.
        XCTAssertTrue(app.switches["Demo mode"].exists, "Demo mode is a global flag")
        XCTAssertFalse(app.switches["Demo fast sweep"].exists, "Demo fast sweep toggle was replaced by the Demo waveform picker")
        XCTAssertTrue(app.staticTexts["Demo waveform"].exists, "Demo waveform picker lives on Theme & demo")
        XCTAssertTrue(app.switches["Region double-buffer"].exists, "Region double-buffer is a global flag")
        XCTAssertTrue(app.switches["TE sync"].exists, "TE sync is a global flag")
        XCTAssertTrue(app.switches["TE scanline"].exists, "TE scanline is a global flag")
        XCTAssertFalse(app.switches["Vault needle red"].exists, "Vault needle red must not live on Theme & demo")
        XCTAssertFalse(app.switches["Vault needle tail"].exists, "Vault needle tail must not live on Theme & demo")
        XCTAssertFalse(app.switches["Big digit static bg"].exists, "Big digit static bg must not live on Theme & demo")
        attach("sim-settings-themedemo-page", app.screenshot())
        app.navigationBars["Theme & demo"].buttons.firstMatch.tap()

        // TPMS sub-page owns the TPMS BLE link toggle.
        let tpmsRow = app.cells.containing(.staticText, identifier: "TPMS").firstMatch
        XCTAssertTrue(tpmsRow.waitForExistence(timeout: 8), "TPMS sub-page row")
        tpmsRow.tap()
        XCTAssertTrue(app.navigationBars["TPMS"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.switches["TPMS BLE link"].exists, "TPMS BLE link must live on the TPMS page")
        attach("sim-settings-tpms-page", app.screenshot())
        app.navigationBars["TPMS"].buttons.firstMatch.tap()

        // OBD2 Scanner sub-page: live pill, peer row, Forget button, helper.
        let obdRow = app.cells.containing(.staticText, identifier: "OBD2 Scanner").firstMatch
        XCTAssertTrue(obdRow.waitForExistence(timeout: 8), "OBD2 Scanner sub-page row")
        obdRow.tap()
        XCTAssertTrue(app.navigationBars["OBD2 Scanner"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.staticTexts["Gauge → OBD2 dongle link"].exists, "OBD2 helper caption")
        // Sim fixture reports DISCOVERING (3) with a known peer → Connecting to <name>.
        XCTAssertTrue(app.staticTexts["Connecting to vlinker fd+"].exists, "live pill shows the OBD phase")
        XCTAssertTrue(app.staticTexts["vlinker fd+"].exists, "peer name row")
        XCTAssertTrue(app.staticTexts["11:22:33:44:55:66"].exists, "peer address row")
        XCTAssertTrue(app.buttons["Forget"].exists, "Forget button")
        attach("sim-settings-obd2-page", app.screenshot())
        app.navigationBars["OBD2 Scanner"].buttons.firstMatch.tap()

        // Clock & timezone sub-page with the manual timezone menu.
        let clockRow = app.cells.containing(.staticText, identifier: "Clock & timezone").firstMatch
        XCTAssertTrue(clockRow.waitForExistence(timeout: 8), "Clock & timezone sub-page row")
        clockRow.tap()
        XCTAssertTrue(app.navigationBars["Clock & timezone"].waitForExistence(timeout: 8))
        attach("sim-settings-clock-page", app.screenshot())

        let menu = app.buttons.matching(NSPredicate(format: "label BEGINSWITH 'Timezone'")).firstMatch
        XCTAssertTrue(menu.waitForExistence(timeout: 8), "timezone menu")
        menu.tap()
        sleep(1)
        attach("sim-settings-timezone-menu", app.screenshot())

        // Selecting a curated zone applies it (checkmark moves to the row).
        let pacific = app.buttons["US Pacific"]
        XCTAssertTrue(pacific.waitForExistence(timeout: 6), "curated zone list should include US Pacific")
        pacific.tap()
        sleep(1)
        attach("sim-settings-timezone-pacific", app.screenshot())
    }

    func testConnectionSavedGaugeRowFollowsParityVisibilityMatrix() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eSavedGauge", "Saved Test Gauge", "-e2eTab", "settings"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))
        let connectionRow = app.cells.containing(.staticText, identifier: "Connection").firstMatch
        XCTAssertTrue(connectionRow.waitForExistence(timeout: 8), "Connection sub-page row")
        connectionRow.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))

        // While connected: the saved row is HIDDEN (PARITY.md row 1: the row is
        // visible only when a peer is remembered AND not connected).
        XCTAssertFalse(app.staticTexts["Saved gauge"].exists, "saved row hidden while connected")
        XCTAssertFalse(app.staticTexts["Saved Test Gauge"].exists, "saved gauge name hidden while connected")
        XCTAssertTrue(app.staticTexts["Connected to Saved Test Gauge"].waitForExistence(timeout: 8),
                      "single status pill shows the live link")
        XCTAssertEqual(statusStringCount(in: app), 1, "exactly ONE status string while connected")

        // Disconnect: the row reappears with a Connect action; the pill becomes
        // "Not connected". Still exactly one status string — no contradiction.
        let disconnect = app.buttons["Disconnect"]
        XCTAssertTrue(disconnect.waitForExistence(timeout: 8), "Disconnect button")
        disconnect.tap()
        XCTAssertTrue(app.staticTexts["Saved gauge"].waitForExistence(timeout: 8), "saved row persists after disconnect")
        XCTAssertTrue(app.staticTexts["Saved Test Gauge"].exists, "saved gauge name after disconnect")
        XCTAssertTrue(app.buttons["Connect"].exists, "saved row offers a Connect action while disconnected")
        XCTAssertEqual(statusStringCount(in: app), 1, "exactly ONE status string after disconnect")
        XCTAssertFalse(app.staticTexts["No gauge found. Make sure the gauge is advertising."].exists,
                       "never contradict a remembered peer with 'No gauge found'")
        attach("sim-settings-saved-gauge", app.screenshot())
    }

    func testStatusPageRendersExactlyOneStatusString() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "status"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["Live · BLE notify"].waitForExistence(timeout: 12),
                      "the transport footer is the status surface on Status")
        XCTAssertEqual(statusStringCount(in: app), 1,
                       "the transport footer is the ONLY status string on the Status page")
    }

    /// AUDIT 3: exactly one connection-state string per surface. The Connection
    /// pill and the Status transport footer are the two status strings; the
    /// saved row shows identity only, scan results and "No gauge found" are not
    /// connection-state wording.
    private func statusStringCount(in app: XCUIApplication) -> Int {
        app.staticTexts.matching(
            NSPredicate(
                format: "label MATCHES %@",
                "(Connected to .+)|(Not connected)|(Connecting…)|(Reconnecting… \\(attempt \\d+\\))|(Live · BLE notify)|(Live · 1 Hz)|(Unreachable — .+)|(Not configured — .+)"
            )
        ).count
    }

    private func bringIntoView(_ element: XCUIElement, app: XCUIApplication) {
        var attempts = 0
        while !element.isHittable && attempts < 12 {
            app.collectionViews.firstMatch.swipeUp()
            attempts += 1
        }
    }

    private func attach(_ name: String, _ screenshot: XCUIScreenshot) {
        let attachment = XCTAttachment(screenshot: screenshot)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }

    /// Regression for the iOS 26 floating tab bar riding over the Themes list:
    /// after scrolling to the bottom, the last theme row must sit fully above
    /// the bar (contentMargins(.bottom) clearance), not underneath it.
    func testThemesListScrollsClearOfFloatingTabBar() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "themes"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 15))
        let neon = app.staticTexts["Neon"].firstMatch
        // Lazy lists only materialize off-screen rows into the AX tree once
        // they are scrolled near the viewport — swipe until the row exists.
        for _ in 0..<10 where !neon.exists {
            app.swipeUp()
            usleep(400_000)
        }
        XCTAssertTrue(neon.exists, "last theme row must exist after scrolling")
        for _ in 0..<8 where !neon.isHittable {
            app.swipeUp()
            usleep(300_000)
        }
        XCTAssertTrue(neon.isHittable, "last theme row must be reachable by scrolling")
        let tabBar = app.tabBars.firstMatch
        XCTAssertTrue(tabBar.exists)
        XCTAssertFalse(
            neon.frame.intersects(tabBar.frame),
            "last theme row \(neon.frame) must not intersect the floating tab bar \(tabBar.frame)"
        )
    }
}
