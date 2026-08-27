import XCTest

/// Round-4 acceptance screenshots: portrait AND landscape captures of Status,
/// Themes, Logs and Settings root against the sim BLE transport
/// (`-e2eSimBle`). PNGs are written to `/tmp/boostgauge-shots/` for the vision
/// QA agent: `ios-round4-{screen}-{portrait|landscape}.png`.
final class Round4ScreenshotUITests: XCTestCase {
    func testCapturePortraitAndLandscapeScreens() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle"]
        app.launch()

        let screens: [(tab: String, nav: String, name: String)] = [
            ("Status", "Boost Gauge", "status"),
            ("Themes", "Themes", "themes"),
            ("Logs", "Logs", "logs"),
            ("Settings", "Settings", "settings"),
        ]
        let dir = "/tmp/boostgauge-shots"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let destination = { (name: String, orientation: String) -> URL in
            URL(fileURLWithPath: "\(dir)/ios-round4-\(name)-\(orientation).png")
        }

        // Portrait pass.
        XCUIDevice.shared.orientation = .portrait
        usleep(600_000)
        for screen in screens {
            let tab = app.tabBars.buttons[screen.tab]
            XCTAssertTrue(tab.waitForExistence(timeout: 10), "\(screen.tab) tab")
            tab.tap()
            XCTAssertTrue(app.navigationBars[screen.nav].waitForExistence(timeout: 15), "\(screen.nav) navigation")
            sleep(2)
            try app.screenshot().pngRepresentation.write(to: destination(screen.name, "portrait"))
        }

        // Landscape pass (XCUIDevice rotates the simulator's layout; the
        // framebuffer stores the landscape UI rotated 90° in a portrait
        // buffer, so the raw window capture is saved here and a host-side
        // step re-orients it to the true landscape screenshot).
        XCUIDevice.shared.orientation = .landscapeLeft
        usleep(800_000)
        for screen in screens {
            let tab = app.tabBars.buttons[screen.tab]
            XCTAssertTrue(tab.waitForExistence(timeout: 10), "\(screen.tab) tab (landscape)")
            tab.tap()
            XCTAssertTrue(app.navigationBars[screen.nav].waitForExistence(timeout: 15), "\(screen.nav) navigation (landscape)")
            sleep(2)
            try app.windows.firstMatch.screenshot().pngRepresentation.write(to: destination(screen.name, "landscape"))
        }
    }
}