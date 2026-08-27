import XCTest

/// Round-5b fresh acceptance screenshots for vision QA: portrait AND landscape
/// captures of Status, Themes and Logs against the sim BLE transport
/// (`-e2eSimBle`), plus a deterministic crosshair capture. The device tag comes
/// from the `DEVICE_TAG` environment variable (16e | promax) so both sims can
/// write to the same host dir. PNGs go to
/// `/tmp/boostgauge-shots/ios-round5b-{screen}-{device}-{orientation}.png`.
///
/// Landscape quirk: `XCUIDevice.orientation` lays the app out landscape (AX
/// window is landscape) but the simulator framebuffer stays a portrait buffer
/// rotated 90°, so raw landscape captures are saved here and a host-side step
/// rotates them by 90° before review.
final class Round5BScreenshotUITests: XCTestCase {
    func testCaptureStatusThemesLogsPortraitAndLandscape() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle"]
        app.launch()
        // Classify the device by logical portrait width (16e = 390, Pro Max =
        // 440) so both sims write distinguishable filenames without an
        // external device tag (shell env does not propagate to the runner).
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
        let width = app.windows.firstMatch.frame.width
        let deviceTag = width <= 400 ? "16e" : "promax"

        let screens: [(tab: String, nav: String, name: String)] = [
            ("Status", "Boost Gauge", "status"),
            ("Themes", "Themes", "themes"),
            ("Logs", "Logs", "logs"),
        ]
        let dir = "/tmp/boostgauge-shots"
        try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let destination = { (name: String, orientation: String) -> URL in
            URL(fileURLWithPath: "\(dir)/ios-round5b-\(name)-\(deviceTag)-\(orientation).png")
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

        // Landscape pass (raw window capture; host rotates 90°).
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

        // Leave the device portrait: the orientation persists device-wide
        // across launches, so a suite that leaves the sim landscape breaks the
        // portrait assumptions of every later test in the same run.
        XCUIDevice.shared.orientation = .portrait
        usleep(500_000)
    }

    /// Crosshair fixture capture: the -e2eCrosshair launch arg pins the chart
    /// readout so the graph single-line + axis labels + crosshair pill land in
    /// one deterministic portrait screenshot for vision QA.
    func testCaptureLogsChartWithCrosshair() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "logs", "-e2eCrosshair"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 15))
        XCTAssertTrue(app.staticTexts["Last 5 minutes · 1500 samples"].waitForExistence(timeout: 25))
        sleep(1)
        let width = app.windows.firstMatch.frame.width
        let deviceTag = width <= 400 ? "16e" : "promax"
        try app.screenshot().pngRepresentation.write(
            to: URL(fileURLWithPath: "/tmp/boostgauge-shots/ios-round5b-logs-crosshair-\(deviceTag).png")
        )
    }
}
