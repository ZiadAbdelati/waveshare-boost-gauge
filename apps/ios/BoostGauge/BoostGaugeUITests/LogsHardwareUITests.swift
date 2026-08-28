import XCTest

/// Exercises the REAL gauge's BLE log path: connect → Logs tab → load and verify samples render without breaking BLE.
final class LogsHardwareUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    func testLogsRenderWithoutBreakingBLE() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eTab", "settings"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        // Connect via BLE (same as WiFi test) — skip if already connected
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        let connectBtn = app.buttons["Connect"].firstMatch
        let firmware = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'firmware'")).firstMatch
        if firmware.exists {
            NSLog("LOGS-ALREADY-CONNECTED")
        } else if connectBtn.waitForExistence(timeout: 10) {
            connectBtn.tap()
        }
        let connected = NSPredicate(format: "label CONTAINS 'firmware'")
        let gaugeInfo = app.staticTexts.matching(connected).firstMatch
        let goneExpectation = expectation(for: NSPredicate(format: "exists == 0"), evaluatedWith: connectBtn)
        _ = XCTWaiter.wait(for: [goneExpectation], timeout: 30)
        XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 5), "gauge did not connect over BLE")
        app.navigationBars.buttons.firstMatch.tap() // back to Settings

        // Switch to Logs tab via tab bar
        let logsTab = app.tabBars.buttons["Logs"]
        XCTAssertTrue(logsTab.waitForExistence(timeout: 8), "Logs tab missing")
        logsTab.tap()
        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 8))

        // Wait for either samples or empty state, but not a hang. Logs auto-loads on appear.
        let sampleCount = app.staticTexts["logsSampleCount"]
        let noSamples = app.staticTexts["No log samples yet"]
        let noSamplesAlt = app.staticTexts["No samples"]
        let deadline = Date().addingTimeInterval(20)
        var sawSamples = false
        var sawEmpty = false
        var label = ""
        while Date() < deadline {
            if sampleCount.exists {
                label = sampleCount.label
                // Valid is "Last 5 minutes · 42 samples" etc; empty is "0 samples" or "No samples"
                if label.contains("·") && label.contains("samples") && !label.contains("0 samples") && !label.contains("No samples") {
                    sawSamples = true
                    break
                }
                // If it shows 0 samples, that's empty
                if label.contains("0 samples") {
                    sawEmpty = true
                    break
                }
            }
            if noSamples.exists || noSamplesAlt.exists {
                sawEmpty = true
                label = noSamples.exists ? "EMPTY-NoLogSamplesYet" : "EMPTY-NoSamples"
                break
            }
            usleep(500_000)
        }
        if label.isEmpty && sampleCount.exists { label = sampleCount.label }
        // Attach outcome
        NSLog("LOGS-OUTCOME: \(label) empty=\(sawEmpty) samples=\(sawSamples) sampleCountExists=\(sampleCount.exists) noSamplesExists=\(noSamples.exists)")
        let screenshot = app.screenshot()
        let shot = XCTAttachment(screenshot: screenshot)
        shot.name = "logs-hardware"
        shot.lifetime = .keepAlways
        add(shot)

        // Visiting Logs must not break BLE — gauge should still be connected when we return to Settings
        app.tabBars.buttons["Settings"].tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 8))
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 5), "BLE broke after visiting Logs")
        XCTAssertTrue(sawSamples, "Logs should show samples, got: \(label) (empty=\(sawEmpty))")
    }
}
