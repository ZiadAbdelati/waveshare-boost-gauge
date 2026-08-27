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
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: false, hasSnapshot: true),
                       "theme matched but no composited frame → must stay hidden")
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, hasSnapshot: false),
                       "RAF fired but no canvas snapshot → must stay hidden")
        XCTAssertFalse(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: false, hasSnapshot: false))
        XCTAssertTrue(GaugeMirrorWebView.Coordinator.shouldRevealFrame(rafDone: true, hasSnapshot: true),
                      "post-font-ready frame composited + snapshot captured → reveal")
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
}
