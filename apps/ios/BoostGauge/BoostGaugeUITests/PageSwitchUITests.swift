import XCTest

final class PageSwitchUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    /// The redundant Boost/TPMS segmented toggle was removed from Status: the
    /// boost page already shows both the gauge and the TPMS card. The physical
    /// page is still exercised by the AppSession hardware-BLE matrix steps
    /// (putPage0/putPage1/putPage0Restore); this guards the UI surface instead.
    func testStatusHasNoPageToggleAndShowsTPMSCard() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eHTTPURL", "http://127.0.0.1:18099", "-e2eTab", "status"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 15))
        // The segmented picker exposed these as buttons; neither may exist now.
        XCTAssertFalse(app.buttons["Boost"].exists, "page toggle must be removed from Status")
        XCTAssertFalse(app.buttons["TPMS"].exists, "page toggle must be removed from Status")
        // The TPMS card is shown unconditionally below the gauge card.
        XCTAssertTrue(app.staticTexts["TPMS"].firstMatch.exists, "TPMS card section title")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "status-no-page-toggle"
        shot.lifetime = .keepAlways
        add(shot)
    }
}