import XCTest
import WebKit
@testable import BoostGauge

final class GaugeMirrorWebViewTests: XCTestCase {
    @MainActor
    func testFlashGuardRevealDecision() {
        // A payload without a theme reveals on any successful render.
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: nil, payloadThemeID: nil))
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: "dyno-cell", payloadThemeID: nil))
        // A render of the WRONG theme (the web app's default boot paint) must
        // never reveal the mirror.
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: "dyno-cell", payloadThemeID: "neon"))
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: nil, payloadThemeID: "neon"))
        // Only a render that matches the requested theme reveals it.
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: "neon", payloadThemeID: "neon"))
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: "vault-tec", payloadThemeID: "vault-tec"))
        // A different requested theme after a match must not reveal.
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldReveal(renderedThemeID: "neon", payloadThemeID: "vault-tec"))
    }

    @MainActor
    func testRevealRequiresCompositedPostFontReadyFrame() {
        // Gate 1 (theme match) alone is NOT enough: revealing also needs a
        // frame a post-font-ready requestAnimationFrame actually composited
        // with its snapshot — otherwise the webview reveals while still holding
        // the previous/default face (the residual flash).
        let realSnapshot = "data:image/png;base64," + String(repeating: "A", count: 40_000)
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: false, snapshot: realSnapshot),
                       "theme matched but no composited frame → must stay hidden")
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, snapshot: ""),
                       "RAF fired but no canvas snapshot → must stay hidden")
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: false, snapshot: ""))
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, snapshot: realSnapshot),
                      "post-font-ready frame composited + snapshot captured → reveal")
    }

    @MainActor
    func testRevealRejectsTinyPreLayoutSnapshot() {
        // Symptom A: a pre-layout 0x0 canvas snapshots as a ~70-byte 1x1 PNG
        // (the stretched black pixel). The gate must reject any snapshot below
        // the floor even though the RAF fired, and accept a real face.
        let oneByOne = "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNkYPhfDwAChwGA60e6kgAAAABJRU5ErkJggg=="
        XCTAssertLessThan(oneByOne.count, GaugeMirrorWebView.Coordinator.minSnapshotBytes,
                          "the 1x1 PNG fixture must be tiny for this test to assert the floor")
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, snapshot: oneByOne),
                       "RAF fired on a 1x1 canvas → must stay hidden, not reveal a black pixel")
        let justAboveFloor = String(repeating: "B", count: GaugeMirrorWebView.Coordinator.minSnapshotBytes + 1)
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, snapshot: justAboveFloor))
    }

    @MainActor
    func testMirrorStaysHiddenUntilRequestedThemePaintsAndStartsAtTop() async throws {
        let (coordinator, webView) = makeMirrorHost()
        coordinator.payload = Self.neonPayload
        coordinator.loadCanonicalMirror()

        // Immediately after load the mirror is invisible — the web app's
        // default-theme (dyno-cell) boot paint must never flash.
        XCTAssertEqual(webView.alpha, 0, "mirror must start hidden")

        // First WKWebView process launch is several seconds on an Intel simulator.
        try await Task.sleep(nanoseconds: 6_000_000_000)
        // Never half-scrolled into the mirror.
        XCTAssertEqual(webView.scrollView.contentOffset.y, 0, accuracy: 1,
                       "mirror must load scrolled to the top")

        // Reveals only once a render matches the requested neon theme.
        let deadline = Date().addingTimeInterval(6)
        while Date() < deadline && webView.alpha < 1 {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(webView.alpha, 1, accuracy: 0.001,
                       "mirror reveals only after the requested theme paints")
    }

    @MainActor
    func testThemeSwitchRehidesUntilNewThemePaints() async throws {
        let (coordinator, webView) = makeMirrorHost()
        coordinator.payload = Self.neonPayload
        coordinator.loadCanonicalMirror()
        try await Task.sleep(nanoseconds: 6_000_000_000)
        let revealDeadline = Date().addingTimeInterval(6)
        while Date() < revealDeadline && webView.alpha < 1 {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(webView.alpha, 1, accuracy: 0.001, "neon revealed")

        // Switching to another theme must re-hide the mirror until the new
        // theme actually paints — no flash of the previous (neon) face.
        coordinator.payload = Self.vaultTecPayload
        coordinator.renderIfReady()
        XCTAssertEqual(webView.alpha, 0, "theme switch re-hides the mirror")
        let newDeadline = Date().addingTimeInterval(6)
        while Date() < newDeadline && webView.alpha < 1 {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        XCTAssertEqual(webView.alpha, 1, accuracy: 0.001, "mirror reveals the new theme once painted")
    }

    @MainActor
    private func makeMirrorHost() -> (GaugeMirrorWebView.Coordinator, WKWebView) {
        let coordinator = GaugeMirrorWebView.Coordinator()
        let webView = WKWebView(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        let host = UIViewController()
        window.rootViewController = host
        host.view.addSubview(webView)
        window.makeKeyAndVisible()
        host.view.layoutIfNeeded()
        coordinator.webView = webView
        webView.navigationDelegate = coordinator
        return (coordinator, webView)
    }

    private static let neonPayload: [String: Any] = [
        "activePage": 0,
        "theme": [
            "id": "neon",
            "name": "Neon",
            "style": "neon",
            "customized": false,
            "colors": [
                "face": "#000000", "track": "#0c1440", "text": "#ffffff", "muted": "#35509e",
                "vacuum": "#0064ff", "boost": "#c4172e", "overboost": "#ff6a00", "zero": "#ffffff",
            ],
        ],
    ]

    private static let vaultTecPayload: [String: Any] = [
        "activePage": 0,
        "theme": [
            "id": "vault-tec",
            "name": "Vault-Tec",
            "style": "vault",
            "customized": false,
            "colors": [
                "face": "#05281a", "track": "#0c3d24", "text": "#38f08a", "muted": "#1f7a4d",
                "vacuum": "#38f08a", "boost": "#38f08a", "overboost": "#eafc50", "zero": "#38f08a",
            ],
        ],
    ]

    @MainActor
    func testCanonicalMirrorRendersVisibleCanvasOffline() async throws {
        let coordinator = GaugeMirrorWebView.Coordinator()
        let webView = WKWebView(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        let window = UIWindow(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        let host = UIViewController()
        window.rootViewController = host
        host.view.addSubview(webView)
        window.makeKeyAndVisible()
        host.view.layoutIfNeeded()
        coordinator.webView = webView
        webView.navigationDelegate = coordinator
        // Canonical mirror needs a real theme payload to render — the mirror
        // hides until the requested theme's first paint (themeReady gate).
        coordinator.payload = Self.neonPayload
        coordinator.loadCanonicalMirror()

        // First WKWebView process launch + font preload is several seconds.
        try await Task.sleep(nanoseconds: 6_000_000_000)
        // Poll for reveal (theme-matched RAF + snapshot) like the real view.
        let deadline = Date().addingTimeInterval(6)
        while Date() < deadline && webView.alpha < 1 {
            try await Task.sleep(nanoseconds: 100_000_000)
        }
        let result = try await webView.evaluateJavaScript(
            "document.getElementById('gaugeCanvas')?.toDataURL('image/png') || ''"
        )
        let dataURL = try XCTUnwrap(result as? String)
        XCTAssertTrue(dataURL.hasPrefix("data:image/png;base64,"))
        XCTAssertGreaterThan(dataURL.count, 10_000, "Canonical canvas should contain rendered gauge art, not a blank surface")

        coordinator.payload = [
            "activePage": 1,
            "tpms": [
                "status": 0,
                "lowPsi": 32.0,
                "wheels": [
                    ["psi": 35.1, "valid": true], ["psi": 34.8, "valid": true],
                    ["psi": 31.2, "valid": true], ["psi": 35.0, "valid": true],
                ],
            ],
        ]
        coordinator.renderIfReady()
        try await Task.sleep(nanoseconds: 500_000_000)
        let tpmsResult = try await webView.evaluateJavaScript(
            "({ page: state.activePage, artWidth: tpmsPowertrainImg.naturalWidth })"
        )
        let tpms = try XCTUnwrap(tpmsResult as? [String: Any])
        XCTAssertEqual(tpms["page"] as? Int, 1)
        XCTAssertGreaterThan(tpms["artWidth"] as? Int ?? 0, 0, "Canonical TPMS artwork should load offline")
    }

    // MARK: - H3 mirror overlay freeze across WebContent stall/suspension

    /// Real-device probe: when the WebContent process is suspended (app
    /// backgrounded on a real device), `evaluateJavaScript` errors out. The
    /// reveal poll never even starts, and the +400/+1200 ms rescue renders
    /// armed on the key change are the LAST retries this key ever gets:
    /// `requestedKey == key` on later same-theme `updateUIView` passes means
    /// `renderIfReady` skips `armRescueRenders()`, so the frozen overlay
    /// persists until a DIFFERENT key arrives or the view is recreated.
    @MainActor
    func testH3_MIRROR_OVERLAY_FREEZE_NO_RECOVERY_AFTER_EVAL_FAILURE_SAME_KEY() async throws {
        let coordinator = GaugeMirrorWebView.Coordinator()
        let webView = StubMirrorWebView(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        webView.failEvaluations = true
        let overlay = UIImageView()
        coordinator.overlayView = overlay
        coordinator.webView = webView
        coordinator.payload = Self.neonPayload
        coordinator.themeID = "neon"
        // Drives `ready = true` and schedules the boot render, exactly as a
        // finished navigation would on the device.
        coordinator.webView(webView, didFinish: nil)

        // Boot render + the two armed rescue renders (+400/+1200 ms). Every one
        // hits an evaluateJavaScript error and returns without arming anything.
        try await Task.sleep(nanoseconds: 1_800_000_000)
        XCTAssertEqual(webView.renderCallCount, 3,
                       "boot render + 2 armed rescue renders, then nothing further is scheduled")
        XCTAssertEqual(webView.pollCallCount, 0,
                       "an eval failure never even starts the reveal poll")
        XCTAssertEqual(webView.alpha, 0, accuracy: 0.001)
        XCTAssertEqual(overlay.alpha, 1, accuracy: 0.001, "old theme frozen in the overlay")

        // Repeated same-theme updateUIView passes (the user taps the same row,
        // or a load() re-echoes the same activeThemeId): one render per pass,
        // but armRescueRenders() is skipped — the key never changed.
        coordinator.renderIfReady()
        coordinator.renderIfReady()
        try await Task.sleep(nanoseconds: 1_200_000_000)
        XCTAssertEqual(webView.renderCallCount, 5, "each manual pass fires exactly one render")
        XCTAssertEqual(webView.pollCallCount, 0)
        try await Task.sleep(nanoseconds: 1_000_000_000)
        XCTAssertEqual(webView.renderCallCount, 5,
                       "KNOWN-BUG DEMO: same-key updateUIView never re-arms the rescue renders; the frozen overlay persists until a different key or a fresh webview")
        XCTAssertEqual(overlay.alpha, 1, accuracy: 0.001)
    }

    /// Control case for the failure above: when the WebContent is alive but
    /// stalled (RAF never fires, gate 2 stays closed), the poll-exhaust rescue
    /// DOES re-arm itself forever (render → poll-exhaust → rescue → render),
    /// so a merely slow WebContent recovers on its own. Only a crashed/
    /// suspended WebContent (evaluateJavaScript error) freezes permanently.
    @MainActor
    func testH3_RESCUE_RETRY_LOOP_RECOVERS_WHEN_WEBVIEW_ALIVE() async throws {
        let coordinator = GaugeMirrorWebView.Coordinator()
        let webView = StubMirrorWebView(frame: CGRect(x: 0, y: 0, width: 466, height: 466))
        coordinator.overlayView = UIImageView()
        coordinator.webView = webView
        coordinator.payload = Self.neonPayload
        coordinator.themeID = "neon"
        coordinator.webView(webView, didFinish: nil)

        // Poll ceiling 40 × 50 ms ≈ 2 s, then a rescue render at +1.5 s → the
        // cycle repeats indefinitely while the key is unchanged.
        try await Task.sleep(nanoseconds: 5_500_000_000)
        XCTAssertGreaterThanOrEqual(webView.renderCallCount, 2,
                                    "the poll-exhaust rescue re-renders even though the requestedKey never changes")
        XCTAssertGreaterThan(webView.pollCallCount, 40,
                             "the reveal poll re-runs after every rescue render")
        XCTAssertEqual(webView.alpha, 0, accuracy: 0.001, "still hidden — no composited frame yet")
    }
}

/// Deterministic stand-in for a WKWebView whose WebContent process is stalled
/// or suspended: `evaluateJavaScript` never executes JS in a live WebContent —
/// it returns controlled results (or an error) and records how the coordinator
/// drives it. The render script is distinguishable from the poll script by its
/// `drawGauge(sample)` call.
private final class StubMirrorWebView: WKWebView {
    var failEvaluations = false
    private(set) var renderCallCount = 0
    private(set) var pollCallCount = 0

    override func evaluateJavaScript(_ javaScriptString: String, completionHandler: ((Any?, Error?) -> Void)? = nil) {
        let isRender = javaScriptString.contains("drawGauge(sample)")
        if isRender { renderCallCount += 1 } else { pollCallCount += 1 }
        if failEvaluations {
            completionHandler?(nil, NSError(domain: "WebKitErrorDomain", code: 4, userInfo: nil))
            return
        }
        // Gate 1 passes (the rendered theme matches), gate 2 never opens.
        completionHandler?(isRender ? ["renderedThemeID": "neon"] : ["done": false, "snap": ""], nil)
    }
}
