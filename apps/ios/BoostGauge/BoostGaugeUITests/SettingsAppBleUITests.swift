import XCTest

final class SettingsAppBleUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testSettingsShowsAppBleToggleOffByDefault() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eHTTPURL", "http://127.0.0.1:18099", "-e2eTab", "settings"]
        app.launch()

        let displayRow = app.cells.containing(.staticText, identifier: "Display").firstMatch
        XCTAssertTrue(displayRow.waitForExistence(timeout: 15), "Settings should list a Display sub-page")
        displayRow.tap()

        // appBle toggle was removed: toggling via BLE would trap the app
        // disconnected with no UI to re-enable (firmware-only via PUT /config).
        let toggle = app.switches["Companion app advertising (phone → gauge)"]
        XCTAssertFalse(toggle.waitForExistence(timeout: 3), "Companion app advertising toggle should not exist after removal")
        let caption = app.staticTexts["Gauge advertises over BLE so companion apps can find it"]
        XCTAssertFalse(caption.exists, "helper caption should not exist after toggle removal")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "settings-display-no-appble"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
