import XCTest

final class PageSwitchUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testStatusPageControlPutsPageAndReflectsState() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eHTTPURL", "http://127.0.0.1:18099", "-e2eTab", "status"]
        app.launch()

        let boostButton = app.buttons["Boost"]
        let tpmsButton = app.buttons["TPMS"]
        XCTAssertTrue(boostButton.waitForExistence(timeout: 15), "Boost segment should appear on Status")
        // Accept either page as the starting point (the mock persists the last
        // page across launches); tap whichever is not currently selected.
        let originallyTPMS = tpmsButton.isSelected
        let target = originallyTPMS ? boostButton : tpmsButton
        let other = originallyTPMS ? tpmsButton : boostButton
        XCTAssertTrue(other.isSelected, "One page segment should be selected")

        target.tap()

        // The picker optimistically reflects the tap, then the 1 Hz state poll
        // confirms the server-side activePage flip.
        let selected = NSPredicate(format: "isSelected == true")
        let expectation = XCTNSPredicateExpectation(predicate: selected, object: target)
        let result = XCTWaiter.wait(for: [expectation], timeout: 10)
        XCTAssertEqual(result, .completed, "Tapped segment should become selected via /page PUT + state poll")
        XCTAssertFalse(other.isSelected)

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "status-page-\(originallyTPMS ? "boost" : "tpms")"
        shot.lifetime = .keepAlways
        add(shot)
    }
}
