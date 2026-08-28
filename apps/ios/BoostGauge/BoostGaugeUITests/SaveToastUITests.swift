import XCTest

/// Regression: sub-page save buttons felt dead and the toast never showed on
/// the page where the button lives. Drives real taps in the sim (SimBle)
/// across three pages and asserts each page's own toast appears.
final class SaveToastUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    private func launch() -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "settings"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))
        return app
    }

    private func open(_ title: String, app: XCUIApplication) {
        let row = app.staticTexts[title].firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 8), "\(title) row")
        while !row.isHittable { app.swipeUp(); usleep(250_000) }
        row.tap()
        XCTAssertTrue(app.navigationBars[title].waitForExistence(timeout: 8))
    }

    private func tapAndExpectToast(_ label: String, app: XCUIApplication) {
        let btn = app.buttons[label].firstMatch
        var found = btn.waitForExistence(timeout: 4)
        var swipes = 0
        while !found && swipes < 6 {
            app.swipeUp()
            usleep(300_000)
            found = btn.waitForExistence(timeout: 1)
            swipes += 1
        }
        XCTAssertTrue(found, "\(label) button")
        while !btn.isHittable { app.swipeUp(); usleep(250_000) }
        let toast = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'Saved' OR label CONTAINS 'saved' OR label CONTAINS 'synced' OR label CONTAINS 'applied' OR label CONTAINS 'gauge'")).firstMatch
        btn.tap()
        let appeared = toast.waitForExistence(timeout: 6)
        if !appeared {
            let texts = app.staticTexts.allElementsBoundByIndex.prefix(30)
                .map { "\($0.label)" }.joined(separator: " | ")
            NSLog("SAVETOAST-DUMP after \(label): \(texts)")
        }
        XCTAssertTrue(appeared, "no toast appeared after tapping \(label)")
    }

    func testToastDoesNotResurfaceOnWifiNavigation() throws {
        let app = launch()
        open("Clock & timezone", app: app)
        tapAndExpectToast("Sync timezone to gauge", app: app)
        // Let the toast expire fully.
        usleep(2_500_000)
        app.navigationBars.buttons.firstMatch.tap()
        // Round-trip the Wi-Fi page.
        open("Wi-Fi", app: app)
        usleep(1_000_000)
        app.navigationBars.buttons.firstMatch.tap()
        usleep(1_000_000)
        // Switch away and back via the tab bar (the user's exact repro).
        app.tabBars.buttons.firstMatch.tap()
        usleep(800_000)
        app.tabBars.buttons["Settings"].tap()
        usleep(800_000)
        let ghost = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'synced' OR label CONTAINS 'Synced'")).firstMatch
        XCTAssertFalse(ghost.exists, "stale timezone toast resurfaced on tab switch")
    }

    func testSubPageSavesShowToastOnTheirOwnPage() throws {
        let app = launch()

        open("Clock & timezone", app: app)
        tapAndExpectToast("Sync timezone to gauge", app: app)
        app.navigationBars.buttons.firstMatch.tap()

        open("Display", app: app)
        tapAndExpectToast("Save display settings", app: app)
        app.navigationBars.buttons.firstMatch.tap()

        open("Demo mode", app: app)
        tapAndExpectToast("Save demo settings", app: app)
    }
}
