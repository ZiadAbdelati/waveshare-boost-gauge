import XCTest

final class SettingsAppBleUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testSettingsShowsAppBleToggleOffByDefault() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eHTTPURL", "http://127.0.0.1:18099", "-e2eTab", "settings"]
        app.launch()

        // Settings is a sub-page index: the appBle toggle lives on the
        // Display sub-page behind a NavigationLink.
        let displayRow = app.cells.containing(.staticText, identifier: "Display").firstMatch
        XCTAssertTrue(displayRow.waitForExistence(timeout: 15), "Settings should list a Display sub-page")
        displayRow.tap()

        let toggle = app.switches["Companion app advertising (phone → gauge)"]
        var swipes = 0
        while !toggle.exists && swipes < 6 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(toggle.exists, "Companion app advertising toggle should exist in Display settings")
        XCTAssertEqual(toggle.value as? String, "0", "appBle should default OFF on a fresh config")

        let caption = app.staticTexts["Gauge advertises over BLE so companion apps can find it"]
        XCTAssertTrue(caption.waitForExistence(timeout: 5), "helper caption should sit under the appBle toggle")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "settings-appble-off"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
