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

        // Display: 3 grouped sections (Brightness / Dim schedule / Display), single Save.
        let displayRow = app.cells.containing(.staticText, identifier: "Display").firstMatch
        XCTAssertTrue(displayRow.waitForExistence(timeout: 8), "Display row")
        displayRow.tap()
        XCTAssertTrue(app.navigationBars["Display"].waitForExistence(timeout: 8))
        XCTAssertTrue(app.staticTexts["Brightness"].waitForExistence(timeout: 8), "Brightness subheader")
        XCTAssertTrue(app.staticTexts["Dim schedule"].waitForExistence(timeout: 8), "Dim schedule subheader")
        // appBle toggle removed — ensure it no longer exists.
        XCTAssertFalse(app.switches["Companion app advertising (phone → gauge)"].exists, "appBle toggle removed")
        // Display flags moved here from Theme & demo.
        XCTAssertTrue(app.switches["Region double-buffer"].waitForExistence(timeout: 8), "display flags moved to Display")
        try capture("display")
        app.navigationBars["Display"].buttons.firstMatch.tap()

        // Demo mode: Demo mode toggle + Demo waveform dropdown (sim demoMode ON).
        let themeRow = app.cells.containing(.staticText, identifier: "Demo mode").firstMatch
        XCTAssertTrue(themeRow.waitForExistence(timeout: 8), "Demo mode row")
        themeRow.tap()
        XCTAssertTrue(app.navigationBars["Demo mode"].waitForExistence(timeout: 8))
        XCTAssertFalse(app.switches["Demo fast sweep"].exists, "second demo toggle removed")
        let waveform = app.staticTexts["Demo waveform"]
        XCTAssertTrue(waveform.waitForExistence(timeout: 8), "Demo waveform dropdown row")
        try capture("demo-mode")
        app.navigationBars["Demo mode"].buttons.firstMatch.tap()

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