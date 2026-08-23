import XCTest

final class SettingsAppBleUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testSettingsShowsAppBleToggleOffByDefault() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eHTTPURL", "http://127.0.0.1:18099", "-e2eTab", "settings"]
        app.launch()

        let toggle = app.switches["Companion BLE advertising"]
        var swipes = 0
        while !toggle.exists && swipes < 6 {
            app.swipeUp()
            swipes += 1
        }
        XCTAssertTrue(toggle.exists, "Companion BLE advertising toggle should exist in Gauge settings")
        XCTAssertEqual(toggle.value as? String, "0", "appBle should default OFF on a fresh config")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "settings-appble-off"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
