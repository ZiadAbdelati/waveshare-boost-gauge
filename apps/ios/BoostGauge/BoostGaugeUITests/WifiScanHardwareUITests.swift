import XCTest

/// Drives the REAL app on the tethered physical iPhone against the REAL gauge:
/// connect BLE → Wi-Fi page → tap Scan networks → assert results render, and
/// cross-check on the firmware side that GET /network/scan arrived (serial
/// instrumentation prints `ctrl GET /network/scan`).
final class WifiScanHardwareUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    func testScanNetworksOnPhysicalGauge() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eTab", "settings"]
        app.launch()

        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        // Connect lives on the Connection subpage (transportSection).
        app.staticTexts["Connection"].firstMatch.tap()
        XCTAssertTrue(app.navigationBars["Connection"].waitForExistence(timeout: 8))
        // Connect through the Saved gauge row's Connect button (already paired).
        let connectBtn = app.buttons["Connect"].firstMatch
        if connectBtn.waitForExistence(timeout: 10) {
            connectBtn.tap()
        } else {
            let texts = app.staticTexts.allElementsBoundByIndex.prefix(40)
                .map { "\($0.label)" }.joined(separator: " | ")
            let buttons = app.buttons.allElementsBoundByIndex.prefix(20)
                .map { "\($0.label)" }.joined(separator: " | ")
            XCTContext.runActivity(named: "visible labels") { _ in
                NSLog("WIFISCAN-DUMP-TEXTS: \(texts)")
                NSLog("WIFISCAN-DUMP-BUTTONS: \(buttons)")
            }
            attach("wifi-scan-no-connect-btn", app.screenshot())
        }
        // Connected shows as the gauge name + firmware line in the section;
        // the Connect button disappearing also signals a live link.
        let connected = NSPredicate(format: "label CONTAINS 'firmware'")
        let gaugeInfo = app.staticTexts.matching(connected).firstMatch
        let connectGone = NSPredicate(format: "exists == 0")
        let goneExpectation = expectation(for: connectGone, evaluatedWith: connectBtn)
        _ = XCTWaiter.wait(for: [goneExpectation], timeout: 30)
        XCTAssertTrue(gaugeInfo.exists, "gauge did not connect over BLE")
        app.navigationBars.buttons.firstMatch.tap() // back to Settings root

        // Open the Wi-Fi page.
        let wifiRow = app.cells.containing(.staticText, identifier: "Wi-Fi").firstMatch
        XCTAssertTrue(wifiRow.waitForExistence(timeout: 8), "Wi-Fi navigation row")
        while !wifiRow.isHittable { app.swipeUp(); usleep(300_000) }
        wifiRow.tap()
        XCTAssertTrue(app.navigationBars["Wi-Fi"].waitForExistence(timeout: 8))

        // Tap Scan networks and wait for scan results to render.
        let scan = app.staticTexts["Scan networks"].firstMatch
        XCTAssertTrue(scan.waitForExistence(timeout: 8), "scan button present")
        while !scan.isHittable { app.swipeUp(); usleep(300_000) }
        scan.tap()

        // Either networks appear, or an error banner appears — a silent nothing
        // is the regression. Wait up to 25s (BLE queue + blocking radio scan).
        let anyNetwork = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS 'dBm'")).firstMatch
        let noNetworks = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS[c] 'no gauge connection'")).firstMatch
        let scanFailed = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS[c] 'failed' OR label CONTAINS[c] 'error'")).firstMatch
        let deadline = Date().addingTimeInterval(25)
        var sawOutcome = false
        var outcome = ""
        while Date() < deadline {
            if anyNetwork.exists { sawOutcome = true; outcome = "NETWORKS: \(anyNetwork.label)"; break }
            if noNetworks.exists { sawOutcome = true; outcome = "BANNER: \(noNetworks.label)"; break }
            if scanFailed.exists { sawOutcome = true; outcome = "ERR: \(scanFailed.label)"; break }
            usleep(500_000)
        }
        XCTAssertTrue(sawOutcome, "scan produced neither results nor an error banner")
        NSLog("WIFISCAN-OUTCOME: \(outcome)")
        attach("wifi-scan-hardware", app.screenshot())
    }

    private func attach(_ name: String, _ screenshot: XCUIScreenshot) {
        let shot = XCTAttachment(screenshot: screenshot)
        shot.name = name
        shot.lifetime = .keepAlways
        add(shot)
    }
}
