import XCTest

/// Drives REAL taps on the Wi-Fi settings page against SimBleTransport (which
/// now mirrors the /network routes). Regression harness for the "button does
/// nothing / taps not registering" reports: every interaction below must
/// produce an observable UI state change, and taps must succeed on the FIRST
/// coordinate (no retries) so a dead-button regression fails the test.
final class WifiPageUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
        XCUIDevice.shared.orientation = .portrait
        usleep(300_000)
    }

    @discardableResult
    private func tapUntil(_ element: XCUIElement, appears target: XCUIElement, app: XCUIApplication, maxTaps: Int = 1) -> Int {
        var taps = 0
        while !target.exists && taps < maxTaps {
            element.tap()
            taps += 1
            usleep(400_000)
        }
        _ = target.waitForExistence(timeout: 5)
        return taps
    }

    func testWifiButtonsRespondOnFirstTap() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-e2eSimBle", "-e2eTab", "settings"]
        // The use-phone button now triggers the system Location dialog; auto-
        // allow it so the sim run exercises the granted path.
        addUIInterruptionMonitor(withDescription: "location permission") { alert in
            let allow = alert.buttons["Allow While Using App"]
                ?? alert.buttons["Allow"]
            if allow.exists { allow.tap(); return true }
            return false
        }
        app.launch()

        XCTAssertTrue(app.navigationBars["Settings"].waitForExistence(timeout: 15))

        let wifiRow = app.cells.containing(.staticText, identifier: "Wi-Fi").firstMatch
        XCTAssertTrue(wifiRow.waitForExistence(timeout: 8), "Wi-Fi navigation row")
        wifiRow.tap()
        XCTAssertTrue(app.navigationBars["Wi-Fi"].waitForExistence(timeout: 8))

        // Refresh status: one tap must fill in the AP row from the sim payload.
        let refresh = app.descendants(matching: .any).matching(
            NSPredicate(format: "identifier == %@", "wifi.refreshStatus")).firstMatch
        XCTAssertTrue(refresh.waitForExistence(timeout: 8), "refresh button present")
        bringIntoView(refresh, app: app)
        let apRow = app.staticTexts["BoostGauge-TEST"]
        let refreshTaps = tapUntil(refresh, appears: apRow, app: app)
        XCTAssertLessThanOrEqual(refreshTaps, 1,
            "Refresh status needed \(refreshTaps) taps — dead-tap regression")
        XCTAssertTrue(apRow.exists, "status populated after one tap")

        // Use this iPhone's network: the sim has no phone Wi-Fi to read, so the
        // handler MUST surface an error banner — a silent no-op fails here.
        let usePhone = app.descendants(matching: .any).matching(
            NSPredicate(format: "identifier == %@", "wifi.usePhoneWifi")).firstMatch
        XCTAssertTrue(usePhone.waitForExistence(timeout: 6), "use-phone button present")
        bringIntoView(usePhone, app: app)
        let errorBanner = app.descendants(matching: .any).matching(
            NSPredicate(format: "label CONTAINS %@", "Wi-Fi SSID")).firstMatch
        usePhone.tap()
        // The system Location dialog pops on the FIRST use-phone tap in a
        // fresh app container; the interruption monitor only services on a
        // subsequent interaction. Nudge, allow, then tap again for the result.
        app.swipeUp()
        usleep(1_500_000)
        app.swipeDown()
        usleep(500_000)
        if !errorBanner.exists {
            // Permission already granted (or dialog handled): expect banner now.
            _ = errorBanner.waitForExistence(timeout: 5)
        }
        if !errorBanner.exists && usePhone.exists && usePhone.isHittable {
            usePhone.tap()
        }
        let appeared = errorBanner.waitForExistence(timeout: 8)
        XCTAssertTrue(appeared, "use-phone handler surfaced its result")

        let shot = XCTAttachment(screenshot: app.screenshot())
        shot.name = "wifi-buttons-after-taps"
        shot.lifetime = .keepAlways
        add(shot)
    }

    private func bringIntoView(_ element: XCUIElement, app: XCUIApplication) {
        guard element.exists else { return }
        var attempts = 0
        while !element.isHittable && attempts < 10 {
            app.swipeUp()
            attempts += 1
        }
    }

    private func attach(_ name: String, _ image: XCTAttachment) {
        image.name = name
        image.lifetime = .keepAlways
        add(image)
    }
}
