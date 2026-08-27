import XCTest

/// Settings-tweaks acceptance screenshots (PARITY round): Display helper
/// caption, Theme & demo Demo waveform dropdown, and the new OBD2 Scanner
/// page. PNGs go to `/tmp/boostgauge-shots/ios-tweaks-{display,theme-demo,obd2}.png`.
final class SettingsTweaksScreenshotUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testCaptureSettingsTweaks() throws {
        let dir = "/tmp/boostgauge-shots"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "settings"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        let capture = { (name: String) throws in
            sleep(1)
            try app.screenshot().pngRepresentation.write(
                to: URL(fileURLWithPath: "\(dir)/ios-tweaks-\(name).png")
            )
        }

        // Display: renamed appBle toggle + helper caption underneath.
        let displayRow = app.cells.containing(.staticText, identifier: "Display").firstMatch
        XCTAssertTrue(displayRow.waitForExistence(timeout: 8), "Display row")
        displayRow.tap()
        XCTAssertTrue(app.navigationBars["Display"].waitForExistence(timeout: 8))
        let toggle = app.switches["Companion app advertising (phone → gauge)"]
        XCTAssertTrue(toggle.waitForExistence(timeout: 8), "renamed appBle toggle")
        bringIntoView(toggle, app: app)
        XCTAssertTrue(app.staticTexts["Gauge advertises over BLE so companion apps can find it"].exists, "helper caption")
        try capture("display")
        app.navigationBars["Display"].buttons.firstMatch.tap()

        // Theme & demo: Demo mode toggle + Demo waveform dropdown (sim demoMode ON).
        let themeRow = app.cells.containing(.staticText, identifier: "Theme & demo").firstMatch
        XCTAssertTrue(themeRow.waitForExistence(timeout: 8), "Theme & demo row")
        themeRow.tap()
        XCTAssertTrue(app.navigationBars["Theme & demo"].waitForExistence(timeout: 8))
        XCTAssertFalse(app.switches["Demo fast sweep"].exists, "second demo toggle removed")
        let waveform = app.staticTexts["Demo waveform"]
        XCTAssertTrue(waveform.waitForExistence(timeout: 8), "Demo waveform dropdown row")
        try capture("theme-demo")
        app.navigationBars["Theme & demo"].buttons.firstMatch.tap()

        // OBD2 Scanner: live pill + peer row + helper.
        let obdRow = app.cells.containing(.staticText, identifier: "OBD2 Scanner").firstMatch
        XCTAssertTrue(obdRow.waitForExistence(timeout: 8), "OBD2 Scanner row")
        obdRow.tap()
        XCTAssertTrue(app.navigationBars["OBD2 Scanner"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.staticTexts["Connecting to vlinker fd+"].waitForExistence(timeout: 8), "live pill")
        XCTAssertTrue(app.staticTexts["Gauge → OBD2 dongle link"].exists, "helper")
        try capture("obd2")
    }

    private func bringIntoView(_ element: XCUIElement, app: XCUIApplication) {
        var attempts = 0
        while !element.isHittable && attempts < 12 {
            app.swipeUp()
            attempts += 1
        }
    }
}