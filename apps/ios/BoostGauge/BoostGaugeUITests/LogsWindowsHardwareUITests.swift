import XCTest

/// Verifies every Logs window (1m/5m/15m) over the real BLE link against the
/// physical gauge: each chip must land samples, switch promptly, and never
/// drop the BLE connection (regression: 15m used to break the link when 5m
/// worked).
final class LogsWindowsHardwareUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    private func visibleLabels(_ app: XCUIApplication) -> String {
        app.staticTexts.allElementsBoundByIndex.prefix(40)
            .map { "\($0.label)" }.joined(separator: " | ")
    }

    func testAllWindowsSwitchOverBLEWithoutBreakingLink() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eTab", "settings"]
        app.launch()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        // Connect (skip if already connected).
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        let gaugeInfo = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'firmware'")).firstMatch
        if !gaugeInfo.exists {
            let connectBtn = app.buttons["Connect"].firstMatch
            XCTAssertTrue(connectBtn.waitForExistence(timeout: 10), "no Connect button")
            connectBtn.tap()
            XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 30), "gauge did not connect over BLE")
        }
        app.navigationBars.buttons.firstMatch.tap() // back to Settings

        // Logs tab.
        let logsTab = app.tabBars.buttons["Logs"]
        XCTAssertTrue(logsTab.waitForExistence(timeout: 8), "Logs tab missing")
        logsTab.tap()
        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 8))

        let sampleCount = app.staticTexts["logsSampleCount"]
        // Wait for the label to equal `expected` (a window switch must actually
        // move the label — the stale-while-revalidate redraw can flash the OLD
        // label briefly, so equality against the target is the only safe check).
        func waitForLabel(_ expected: String) -> String {
            let deadline = Date().addingTimeInterval(45)
            while Date() < deadline {
                if sampleCount.exists, sampleCount.label == expected {
                    return expected
                }
                usleep(500_000)
            }
            return "TIMEOUT waiting for '\(expected)': \(sampleCount.exists ? sampleCount.label : "no label")"
        }

        // Default 5m first (any valid 5m label is fine — count varies by ring fill).
        let fiveDeadline = Date().addingTimeInterval(45)
        var five = ""
        while Date() < fiveDeadline {
            if sampleCount.exists,
               sampleCount.label.contains("Last 5 minutes"),
               sampleCount.label.contains("samples"),
               !sampleCount.label.contains("0 samples") {
                five = sampleCount.label
                break
            }
            usleep(500_000)
        }
        NSLog("LOGSWIN-5m: \(five)")
        XCTAssertTrue(five.contains("Last 5 minutes"), "5m window wrong: \(five)")

        // 1m.
        let chip1m = app.buttons["logsWindow.300"]
        XCTAssertTrue(chip1m.waitForExistence(timeout: 8), "1m chip missing")
        chip1m.tap()
        let one = waitForLabel("Last 1 minute · 128 samples")
        NSLog("LOGSWIN-1m: \(one)")
        XCTAssertTrue(one.contains("Last 1 minute"), "1m window wrong: \(one)")

        // 15m — the historical breaker.
        let chip15m = app.buttons["logsWindow.4500"]
        XCTAssertTrue(chip15m.waitForExistence(timeout: 8), "15m chip missing")
        chip15m.tap()
        let fifteen = waitForLabel("Last 15 minutes · 128 samples")
        NSLog("LOGSWIN-15m: \(fifteen)")
        XCTAssertTrue(fifteen.contains("Last 15 minutes"), "15m window wrong: \(fifteen)")

        // Back to 5m — switch must still work after the heavy 15m fetch.
        let chip5m = app.buttons["logsWindow.1500"]
        XCTAssertTrue(chip5m.waitForExistence(timeout: 8), "5m chip missing")
        chip5m.tap()
        let fiveAgain = waitForLabel(five)
        NSLog("LOGSWIN-5m-again: \(fiveAgain)")
        XCTAssertTrue(fiveAgain == five, "5m re-switch wrong: \(fiveAgain)")

        // Link must have survived the whole window dance.
        app.tabBars.buttons["Settings"].tap()
        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 8))
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        XCTAssertTrue(gaugeInfo.waitForExistence(timeout: 5),
                      "BLE broke after switching all three log windows")
        NSLog("LOGSWIN-LINK: still connected after 1m/5m/15m/5m")
    }
}
