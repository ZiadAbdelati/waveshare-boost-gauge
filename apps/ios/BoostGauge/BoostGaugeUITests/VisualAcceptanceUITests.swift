import XCTest

final class VisualAcceptanceUITests: XCTestCase {
    func testUpdatedThemesTPMSAndLogs() {
        let app = XCUIApplication()
        app.launchArguments += ["-e2eHTTPURL", "http://192.168.50.102"]
        app.launch()

        let themes = app.tabBars.buttons["Themes"]
        XCTAssertTrue(themes.waitForExistence(timeout: 15))
        themes.tap()
        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 8))
        attach("01-themes", app.screenshot())

        app.tabBars.buttons["Status"].tap()
        XCTAssertTrue(app.navigationBars["Boost Gauge"].waitForExistence(timeout: 8))
        // No page toggle anymore: the TPMS card is unconditional below the
        // gauge, so wait for a wheel capsule instead of tapping a segment.
        _ = app.staticTexts["FL"].firstMatch.waitForExistence(timeout: 8)
        attach("02-status-tpms", app.screenshot())

        app.tabBars.buttons["Logs"].tap()
        XCTAssertTrue(app.navigationBars["Logs"].waitForExistence(timeout: 8))
        sleep(4)
        attach("03-logs", app.screenshot())
    }

    private func attach(_ name: String, _ screenshot: XCUIScreenshot) {
        let attachment = XCTAttachment(screenshot: screenshot)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}

/// Structural acceptance of the extended Themes editor against the live board.
/// Every theme's DisclosureGroup is expanded via its unlabeled chevron, the
/// firmware-backed per-theme controls are asserted, and a screenshot is kept.
final class ThemeEditorAcceptanceUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = true
    }

    func testEveryThemeExpandsWithCompleteOptions() throws {
        let app = XCUIApplication()
        app.launchArguments += ["-e2eHTTPURL", "http://192.168.50.102"]
        app.launch()

        let themesTab = app.tabBars.buttons["Themes"]
        XCTAssertTrue(themesTab.waitForExistence(timeout: 15))
        themesTab.tap()
        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 8))

        let preview = app.descendants(matching: .any)
            .matching(NSPredicate(format: "label BEGINSWITH 'Exact dashboard preview of'"))
            .firstMatch
        XCTAssertTrue(preview.waitForExistence(timeout: 8), "canonical mirror preview should render")

        let themes: [(id: String, name: String)] = [
            ("dyno-cell", "Dyno Cell"),
            ("vault-tec", "Vault-Tec"),
            ("night-city", "Night City"),
            ("big-digit", "Big Digit"),
            ("neon", "Neon"),
        ]

        for theme in themes {
            expand(theme: theme, app: app)
        }
    }

    private func expand(theme: (id: String, name: String), app: XCUIApplication) {
        let row = rowCell(for: theme.name, app: app)
        bringIntoView(row, app: app)
        XCTAssertTrue(row.waitForExistence(timeout: 8), "row for \(theme.name)")

        let chevron = row.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(chevron.exists, "chevron for \(theme.name)")
        chevron.tap()

        let apply = app.buttons["Apply \(theme.name) options"]
        XCTAssertTrue(apply.waitForExistence(timeout: 6), "Apply button for \(theme.name)")
        bringIntoView(apply, app: app)

        assertChecks(for: theme, app: app)

        attach("theme-\(theme.id)-expanded", app.screenshot())

        let reChevron = rowCell(for: theme.name, app: app)
            .buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        if reChevron.exists { reChevron.tap() }
    }

    private func assertChecks(for theme: (id: String, name: String), app: XCUIApplication) {
        // The firmware persists exactly vacuum/boost/overboost per theme; those
        // three palette roles must be editable in every expanded editor.
        for key in ["vacuum", "boost", "overboost"] {
            // SwiftUI ColorPicker is exposed as a ColorWell on physical iOS and
            // as a Button on some simulators — query by identifier, any element
            // type, so the assertion holds on both.
            let picker = app.descendants(matching: .any)
                .matching(NSPredicate(format: "identifier == %@", "theme.\(theme.id).color.\(key)"))
                .firstMatch
            XCTAssertTrue(picker.exists, "\(theme.name): editable \(key) picker")
        }

        let palette = app.staticTexts["Zone colors"]
        XCTAssertTrue(palette.exists, "\(theme.name): zone colors group")

        let reset = app.buttons["Reset to default colors"]
        XCTAssertFalse(reset.exists, "\(theme.name): no reset for non-customized theme")

        switch theme.id {
        case "dyno-cell":
            XCTAssertTrue(app.switches["Gradient fill"].exists, "dyno-cell: Gradient fill toggle")
        case "vault-tec":
            let face = app.textFields.matching(
                NSPredicate(format: "placeholderValue == %@", "Face color (#RRGGBB)")
            ).firstMatch
            XCTAssertTrue(face.exists, "vault-tec: face color field")
            XCTAssertTrue(
                app.steppers.matching(NSPredicate(format: "label BEGINSWITH 'Vignette'")).firstMatch.exists,
                "vault-tec: vignette stepper"
            )
            XCTAssertTrue(app.switches["Red needle"].exists, "vault-tec: Red needle toggle")
            XCTAssertTrue(app.switches["Counterweight tail"].exists, "vault-tec: Counterweight tail toggle")
        case "night-city":
            XCTAssertTrue(app.switches["Gradient fill"].exists, "night-city: Gradient fill toggle")
            XCTAssertTrue(app.switches["True black background"].exists, "night-city: True black background toggle")
        case "big-digit":
            XCTAssertTrue(app.switches["Static background"].exists, "big-digit: Static background toggle")
            XCTAssertTrue(app.switches["Color the readout"].exists, "big-digit: Color the readout toggle")
            // Live board state: bigDigitStaticBg=true, bigDigitColorText=false,
            // so both conditional pickers must be visible.
            XCTAssertTrue(
                app.descendants(matching: .any).matching(
                    NSPredicate(format: "label == %@", "Static background color")
                ).firstMatch.exists,
                "big-digit: static background color picker"
            )
            XCTAssertTrue(
                app.descendants(matching: .any).matching(
                    NSPredicate(format: "label == %@", "Readout text color")
                ).firstMatch.exists,
                "big-digit: readout text color picker"
            )
        case "neon":
            XCTAssertTrue(app.staticTexts["Layout"].exists, "neon: Layout picker")
            XCTAssertTrue(app.staticTexts["Preset"].exists, "neon: Preset picker")
            XCTAssertTrue(app.staticTexts["Readout font"].exists, "neon: Readout font picker")
            // Live board layout is Segments (1); marquee spin must be gated off.
            XCTAssertFalse(app.switches["Marquee spin"].exists, "neon: marquee spin gated to layout 2")
        default:
            XCTFail("unexpected theme \(theme.id)")
        }
    }

    private func rowCell(for name: String, app: XCUIApplication) -> XCUIElement {
        app.cells.containing(.staticText, identifier: name).firstMatch
    }

    private func collection(_ app: XCUIApplication) -> XCUIElement {
        app.collectionViews.firstMatch
    }

    private func bringIntoView(_ element: XCUIElement, app: XCUIApplication) {
        var attempts = 0
        while !element.isHittable && attempts < 12 {
            if element.exists && element.frame.midY < 0 {
                collection(app).swipeDown()
            } else {
                collection(app).swipeUp()
            }
            attempts += 1
        }
    }

    private func attach(_ name: String, _ screenshot: XCUIScreenshot) {
        let attachment = XCTAttachment(screenshot: screenshot)
        attachment.name = name
        attachment.lifetime = .keepAlways
        add(attachment)
    }
}
/// On-device diagnostic: how is a SwiftUI ColorPicker exposed in the AX tree on
/// physical iOS 26? The acceptance queries buttons by identifier
/// `theme.<id>.color.<key>`; this probe dumps the actual element types.
final class ColorPickerProbeUITests: XCTestCase {
    func testProbeColorPickerElementTypes() throws {
        let app = XCUIApplication()
        app.launchArguments += ["-e2eHTTPURL", "http://192.168.50.102"]
        app.launch()

        let themesTab = app.tabBars.buttons["Themes"]
        XCTAssertTrue(themesTab.waitForExistence(timeout: 15))
        themesTab.tap()
        XCTAssertTrue(app.navigationBars["Themes"].waitForExistence(timeout: 8))

        let row = app.cells.containing(.staticText, identifier: "Dyno Cell").firstMatch
        XCTAssertTrue(row.waitForExistence(timeout: 10))
        let chevron = row.buttons.matching(NSPredicate(format: "label == ''")).firstMatch
        XCTAssertTrue(chevron.exists)
        chevron.tap()
        XCTAssertTrue(app.buttons["Apply Dyno Cell options"].waitForExistence(timeout: 6))

        print("=== ALL descendants with identifier CONTAINS theme. ===")
        print(app.descendants(matching: .any).matching(NSPredicate(format: "identifier CONTAINS 'theme.'")).debugDescription)
        print("=== colorWells ===")
        print(app.colorWells.debugDescription)
        print("=== buttons identifier CONTAINS theme. ===")
        print(app.buttons.matching(NSPredicate(format: "identifier CONTAINS 'theme.'")).debugDescription)
        print("=== staticTexts Vacuum/Boost/Overboost ===")
        print(app.staticTexts.matching(NSPredicate(format: "label IN {'Vacuum','Boost','Overboost'}")).debugDescription)
        print("=== otherElements identifier CONTAINS theme. ===")
        print(app.otherElements.matching(NSPredicate(format: "identifier CONTAINS 'theme.'")).debugDescription)
    }
}
