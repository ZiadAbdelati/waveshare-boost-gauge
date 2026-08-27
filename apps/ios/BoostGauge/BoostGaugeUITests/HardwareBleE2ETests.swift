import XCTest

/// Runs the real companion BLE request matrix on a tethered physical iPhone.
///
/// The app's hardware-E2E surface is intentionally accessibility-driven so the
/// test runner does not need to infer BLE health from ordinary, changing UI.
final class HardwareBleE2ETests: XCTestCase {
    private let matrixTimeout: TimeInterval = 180

    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    func testPhysicalGaugeCompletesFullBleMatrix() throws {
        let app = XCUIApplication()
        app.launchArguments = ["-hardwareBleE2E", "1", "-e2eTab", "status"]

        addUIInterruptionMonitor(withDescription: "Bluetooth permission") { alert in
            // This keeps a first install hands-free. Normally the permission has
            // already been granted and the monitor is never invoked.
            for title in ["Allow New Connections", "Allow", "OK"] where alert.buttons[title].exists {
                alert.buttons[title].tap()
                return true
            }
            let positive = alert.buttons.matching(
                NSPredicate(format: "label CONTAINS[c] 'allow' AND NOT label CONTAINS[c] 'don'")
            ).firstMatch
            if positive.exists {
                positive.tap()
                return true
            }
            if alert.buttons["Settings"].exists {
                alert.buttons["Settings"].tap()
                let settings = XCUIApplication(bundleIdentifier: "com.apple.Preferences")
                guard settings.wait(for: .runningForeground, timeout: 8) else { return false }
                let bluetooth = settings.switches["Bluetooth"]
                guard bluetooth.waitForExistence(timeout: 8) else { return false }
                if (bluetooth.value as? String) == "0" {
                    bluetooth.tap()
                }
                app.activate()
                return true
            }
            return false
        }

        app.launch()
        app.tap() // Gives XCTest a chance to service a pending system alert.

        addTeardownBlock { [weak self, weak app] in
            guard let self, let app else { return }
            self.attachDiagnostics(app: app, name: "hardware-ble-final")
        }

        let status = element("hardwareBLEE2EStatus", in: app)
        XCTAssertTrue(
            status.waitForExistence(timeout: 15),
            "Hardware BLE E2E status surface did not appear; verify -hardwareBleE2E is handled"
        )
        let outcome = waitForOutcome(in: status, timeout: matrixTimeout)
        XCTAssertEqual(
            outcome,
            .passed,
            "Physical BLE request matrix did not pass; last status: \(description(of: status))"
        )

        let requiredSteps = [
            "getState",
            "getStatePayload",
            "getConfig",
            "getThemes",
            "getThemesPayload",
            "putPage0Start",
            "putPage1",
            "putPage0Restore",
            "putThemeNeon",
            "putThemeRestore",
            "readDeviceInfo",
            "readDeviceInfoIP",
            "readStatus",
            "readStatusPayload",
            "readLog",
        ]
        for step in requiredSteps {
            let stepElement = element("hardwareBLEE2EStep.\(step)", in: app)
            XCTAssertTrue(stepElement.exists, "Missing required BLE matrix result for \(step)")
            XCTAssertTrue(
                contains("pass", in: stepElement),
                "Required BLE matrix step \(step) did not pass; value: \(description(of: stepElement))"
            )
        }

        attachDiagnostics(app: app, name: "hardware-ble-matrix-pass")

        // The matrix proved the BLE API carries the FULL /themes payload. Now
        // verify the user-facing gap: the Themes editor must show palette
        // colors/components over BLE (previously BLE returned id/name/style
        // only, so the editor was empty).
        verifyThemesEditorOverBle(app)
        verifyLogsFiveMinuteWindowOverBle(app)
    }

    private func verifyLogsFiveMinuteWindowOverBle(_ app: XCUIApplication) {
        let logsTab = app.tabBars.buttons["Logs"]
        XCTAssertTrue(logsTab.waitForExistence(timeout: 8))
        logsTab.tap()
        let count = element("logsSampleCount", in: app)
        XCTAssertTrue(count.waitForExistence(timeout: 8), "Logs sample-count label must exist")
        // The graph fetches /logs?limit=1500 (last 5 minutes at 5 Hz) — never
        // the full-hour ring, whose payload times out over BLE into the 8-sample
        // diagnostic fallback. The label reads "Last 5 minutes · N samples".
        let deadline = Date().addingTimeInterval(30)
        var seenWindow = false
        while Date() < deadline && !seenWindow {
            if contains("Last 5 minutes", in: count) {
                seenWindow = true
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.5))
        }
        let errors = app.staticTexts.matching(
            NSPredicate(format: "label CONTAINS[c] 'unavailable' OR label CONTAINS[c] 'error' OR label CONTAINS[c] 'failed'")
        ).allElementsBoundByIndex.map { $0.label }
        XCTAssertTrue(
            seenWindow,
            "Logs graph must load the 5-minute window over BLE; label: \(description(of: count)) | errors: \(errors)"
        )
        let attach = XCTAttachment(screenshot: app.screenshot())
        attach.name = "ble-logs-five-minute-window"
        attach.lifetime = .keepAlways
        add(attach)
    }

    private func verifyThemesEditorOverBle(_ app: XCUIApplication) {
        let themesTab = app.tabBars.buttons["Themes"]
        XCTAssertTrue(themesTab.waitForExistence(timeout: 8))
        themesTab.tap()
        let row = app.cells.containing(.staticText, identifier: "Dyno Cell").firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 8), "themes list must load over BLE")
        let chevron = row.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        if chevron.exists { chevron.tap() }
        let vacuum = app.descendants(matching: .any).matching(
            NSPredicate(format: "identifier == %@", "theme.dyno-cell.color.vacuum")
        ).firstMatch
        XCTAssertTrue(vacuum.waitForExistence(timeout: 6), "Themes editor must show palette colors over BLE")
        let attach = XCTAttachment(screenshot: app.screenshot())
        attach.name = "ble-themes-editor-dyno-cell"
        attach.lifetime = .keepAlways
        add(attach)
    }

    private enum Outcome: Equatable {
        case passed
        case failed
        case timedOut
    }

    private func element(_ identifier: String, in app: XCUIApplication) -> XCUIElement {
        app.descendants(matching: .any).matching(identifier: identifier).firstMatch
    }

    private func waitForOutcome(in element: XCUIElement, timeout: TimeInterval) -> Outcome {
        let deadline = Date().addingTimeInterval(timeout)
        repeat {
            if element.exists {
                if contains("pass", in: element) { return .passed }
                if contains("fail", in: element) { return .failed }
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.25))
        } while Date() < deadline
        return .timedOut
    }

    private func contains(_ token: String, in element: XCUIElement) -> Bool {
        description(of: element).localizedCaseInsensitiveContains(token)
    }

    private func description(of element: XCUIElement) -> String {
        let value = (element.value as? String) ?? ""
        return [element.label, value].filter { !$0.isEmpty }.joined(separator: " | ")
    }

    private func attachDiagnostics(app: XCUIApplication, name: String) {
        let screenshot = XCTAttachment(screenshot: app.screenshot())
        screenshot.name = name
        screenshot.lifetime = .keepAlways
        add(screenshot)

        let hierarchy = XCTAttachment(string: app.debugDescription)
        hierarchy.name = "\(name)-accessibility-hierarchy"
        hierarchy.lifetime = .keepAlways
        add(hierarchy)
    }
}
