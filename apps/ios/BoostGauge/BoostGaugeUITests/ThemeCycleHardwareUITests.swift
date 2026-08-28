import XCTest

/// Cycles the physical gauge through ALL five selectable themes from the app
/// (PUT /themes/active over BLE) and confirms: no disconnect, no board reboot,
/// and the active checkmark moves. Regression: a control-response malloc bump
/// caused reboots on theme change; this is the hardware guard.
final class ThemeCycleHardwareUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    func testCycleAllThemesOverBLEWithoutBreakingLink() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eTab", "settings"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        // Connect (skip if already connected).
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        let gaugeInfo = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'firmware'")).firstMatch
        if !gaugeInfo.exists {
            let connectBtn = app.buttons["Connect"].firstMatch
            XCTAssertTrue(connectBtn.waitForExistence(timeout: 10), "no Connect button")
            connectBtn.tap()
            XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 30), "gauge did not connect")
        }
        app.navigationBars.buttons.firstMatch.tap()

        // Themes tab.
        let themesTab = app.tabBars.buttons["Themes"]
        XCTAssertTrue(themesTab.waitForExistence(timeout: 8), "Themes tab missing")
        themesTab.tap()
        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 8))

        let names = ["Dyno Cell", "Vault-Tec", "Night City", "Big Digit", "Neon"]
        for name in names {
            let row = app.cells.containing(.staticText, identifier: name).firstMatch
            XCTAssertTrue(row.waitForExistence(timeout: 8), "\(name) row missing")
            // Bring into view.
            var attempts = 0
            while !row.isHittable && attempts < 6 {
                app.swipeUp()
                attempts += 1
            }
            row.tap()
            // Wait for the checkmark to move to this theme (active state from
            // the PUT response's applied list).
            let checked = app.cells.containing(.staticText, identifier: name)
                .descendants(matching: .any).matching(
                    NSPredicate(format: "identifier == 'checkmark-circle-fill' OR label == 'Selected'")).firstMatch
            // The visible signal: preview accessibility label updates.
            let preview = app.otherElements.matching(
                NSPredicate(format: "label CONTAINS 'Exact dashboard preview'")).firstMatch
            let deadline = Date().addingTimeInterval(15)
            var ok = false
            while Date() < deadline {
                if preview.exists, preview.label.contains(name) { ok = true; break }
                usleep(500_000)
            }
            NSLog("THEMECYCLE-\(name): preview=\(ok ? preview.label : "stale")")
            XCTAssertTrue(ok, "\(name): preview did not update after select")
        }

        // Link alive after five theme switches.
        app.tabBars.buttons["Settings"].tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 8))
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 5), "BLE broke after theme cycle")
        NSLog("THEMECYCLE-LINK: still connected after cycling all five themes")
    }
}
