import XCTest

/// Round 6, slice 2 — BUG A verification. The Big Digit preview must render
/// its correct readout font on FIRST load (not only after switching away and
/// back). Launches with `-e2eSimBle` on the themes tab, selects Big Digit,
/// captures the first-load preview, then switches to Neon and back to Big
/// Digit and captures a second preview. PNGs go to
/// `/tmp/boostgauge-shots/ios-round6-bigdigit-{1st,2nd}.png` for vision QA.
final class Round6BigDigitScreenshotUITests: XCTestCase {
    func testCaptureBigDigitFirstAndSecondLoad() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "themes"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 15))

        let bigDigit = app.staticTexts["Big Digit"].firstMatch
        XCTAssertTrue(waitForElement(bigDigit, app: app), "Big Digit theme row")
        bringIntoView(bigDigit, app: app)
        bigDigit.tap()
        sleep(2)

        let preview = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'Exact dashboard preview of Big Digit'"))
            .firstMatch
        XCTAssertTrue(preview.waitForExistence(timeout: 8), "big-digit preview should render after select")

        // Scroll back to the top so the whole pod sits below the nav bar, then
        // capture the FIRST-load preview (the font-race path under test).
        for _ in 0..<10 { app.swipeDown() }
        usleep(300_000)
        let dir = "/tmp/boostgauge-shots"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        try app.screenshot().pngRepresentation.write(
            to: URL(fileURLWithPath: "\(dir)/ios-round6-bigdigit-1st.png")
        )

        // Switch away and back — the second load is the "fixed by re-selecting"
        // reference and must match the first.
        let neon = app.staticTexts["Neon"].firstMatch
        bringIntoView(neon, app: app)
        neon.tap()
        sleep(2)
        let neonPreview = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'Exact dashboard preview of Neon'"))
            .firstMatch
        XCTAssertTrue(neonPreview.waitForExistence(timeout: 8), "neon preview should render after switch")

        bringIntoView(bigDigit, app: app)
        bigDigit.tap()
        sleep(2)
        XCTAssertTrue(preview.waitForExistence(timeout: 8), "big-digit preview should re-render after switch back")
        for _ in 0..<10 { app.swipeDown() }
        usleep(300_000)
        try app.screenshot().pngRepresentation.write(
            to: URL(fileURLWithPath: "\(dir)/ios-round6-bigdigit-2nd.png")
        )
    }

    private func waitForElement(_ element: XCUIElement, app: XCUIApplication) -> Bool {
        var attempts = 0
        while !element.isHittable && attempts < 12 {
            app.swipeUp()
            usleep(400_000)
            attempts += 1
        }
        return element.isHittable
    }

    private func bringIntoView(_ element: XCUIElement, app: XCUIApplication) {
        if !element.isHittable { waitForElement(element, app: app) }
    }
}
