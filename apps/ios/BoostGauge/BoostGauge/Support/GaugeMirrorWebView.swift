import SwiftUI
import UIKit
import WebKit

/// Hosts the canonical dashboard canvas renderer from `web/app.js` entirely offline.
///
/// Flash guard: the mirror is kept invisible until a render that matches the
/// requested payload theme has actually been COMPOSITED (a post-font-ready
/// requestAnimationFrame, so the theme face is usable — not merely
/// `state.activeThemeId` set synchronously). Between payload changes the last
/// confirmed frame is frozen in a `UIImageView` overlay above the web view, so
/// the circle viewport never flashes a blank, a white backdrop, or the previous
/// theme while the new theme paints.
struct GaugeMirrorWebView: UIViewRepresentable {
    let payload: [String: Any]

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeUIView(context: Context) -> UIView {
        let configuration = WKWebViewConfiguration()
        configuration.websiteDataStore = .nonPersistent()
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.navigationDelegate = context.coordinator
        view.scrollView.isScrollEnabled = false
        view.scrollView.bounces = false
        // Transparent before load: the web view's own default backdrop is the
        // residual flash — a clear, non-opaque surface cannot paint a white or
        // black frame into the circle viewport before JS runs.
        view.isOpaque = false
        view.backgroundColor = .clear
        view.alpha = 0
        context.coordinator.webView = view

        // Snapshot-freeze overlay: holds the last confirmed render of the
        // requested theme, so a theme switch never flashes a blank or the
        // previous face while the new payload paints. Hidden once the live
        // web view is revealed and correct.
        let overlay = UIImageView(frame: .zero)
        overlay.contentMode = .scaleAspectFit
        overlay.isUserInteractionEnabled = false
        overlay.backgroundColor = .clear
        context.coordinator.overlayView = overlay

        let container = UIView(frame: .zero)
        container.backgroundColor = .clear
        container.isOpaque = false
        view.translatesAutoresizingMaskIntoConstraints = false
        overlay.translatesAutoresizingMaskIntoConstraints = false
        container.addSubview(view)
        container.addSubview(overlay)
        NSLayoutConstraint.activate([
            view.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            view.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            view.topAnchor.constraint(equalTo: container.topAnchor),
            view.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            overlay.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            overlay.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            overlay.topAnchor.constraint(equalTo: container.topAnchor),
            overlay.bottomAnchor.constraint(equalTo: container.bottomAnchor),
        ])

        context.coordinator.loadCanonicalMirror()
        return container
    }

    func updateUIView(_ container: UIView, context: Context) {
        context.coordinator.payload = payload
        context.coordinator.renderIfReady()
    }

    final class Coordinator: NSObject, WKNavigationDelegate {
        weak var webView: WKWebView?
        weak var overlayView: UIImageView?
        var payload: [String: Any] = [:]
        private var ready = false
        private var revealedKey: String?
        /// Bumped on every payload-key change; stale reveal polls abort when it
        /// moves, so a poll from the previous theme can never reveal the new one.
        private var renderToken = 0

        /// Flash guard gate 1: the mirror may only become visible once a render
        /// that matches the requested payload's theme id has actually painted. A
        /// payload without a theme (TPMS page) reveals on its first successful
        /// render.
        static func shouldReveal(renderedThemeID: String?, payloadThemeID: String?) -> Bool {
            guard let payloadThemeID else { return true }
            return renderedThemeID == payloadThemeID
        }

        /// Flash guard gate 2: revealing also requires a frame that a
        /// post-font-ready requestAnimationFrame actually composited (and whose
        /// canvas snapshot was captured) — `state.activeThemeId` matching alone
        /// is not enough, or the web view reveals a frame still holding the
        /// previous/default face.
        static func shouldRevealFrame(rafDone: Bool, hasSnapshot: Bool) -> Bool {
            rafDone && hasSnapshot
        }

        private var payloadThemeID: String? {
            (payload["theme"] as? [String: Any])?["id"] as? String
        }

        private var payloadKey: String { payloadThemeID ?? "none" }

        func loadCanonicalMirror() {
            guard let webView else { return }
            webView.alpha = 0
            revealedKey = nil
            renderToken += 1
            let resourceBundle = Bundle(for: Coordinator.self)
            guard
                let indexURL = resourceBundle.url(forResource: "index", withExtension: "html"),
                let cssURL = resourceBundle.url(forResource: "styles", withExtension: "css"),
                let appURL = resourceBundle.url(forResource: "app", withExtension: "js"),
                let resourceURL = resourceBundle.resourceURL,
                var html = try? String(contentsOf: indexURL, encoding: .utf8),
                var css = try? String(contentsOf: cssURL, encoding: .utf8),
                var javascript = try? String(contentsOf: appURL, encoding: .utf8)
            else { return }

            css = css.replacingOccurrences(of: "/doto.ttf", with: "doto.ttf")
            javascript = javascript.replacingOccurrences(
                of: "const TPMS_POWERTRAIN_SRC = \"/tpms_powertrain.png\";",
                with: "const TPMS_POWERTRAIN_SRC = \"tpms_powertrain.png\";"
            )
            javascript = javascript.replacingOccurrences(
                of: "refreshAll(ERR_LIVE).finally(connectEvents);",
                with: "/* Native offline mirror: no API or WebSocket bootstrap. */"
            )
            let cropCSS = """
            <style>
            \(css)
            html, body { margin: 0 !important; width: 100%; height: 100%; overflow: hidden; background: #000 !important; }
            body > * { display: none !important; }
            body > main.shell { display: block !important; width: 100%; height: 100%; margin: 0 !important; padding: 0 !important; }
            main.shell > * { display: none !important; }
            main.shell > .gauge-bay { display: block !important; width: 100%; height: 100%; margin: 0 !important; padding: 0 !important; }
            .gauge-bay > * { display: none !important; }
            .gauge-bay > .gauge-wrap, .gauge-device, #gaugeCanvas { display: block !important; }
            .gauge-wrap > * { display: none !important; }
            .gauge-wrap > .gauge-device { display: block !important; }
            .gauge-wrap, .gauge-device { width: 100% !important; height: 100% !important; max-width: none !important; margin: 0 !important; padding: 0 !important; border: 0 !important; box-shadow: none !important; }
            #gaugeCanvas { width: 100% !important; height: 100% !important; }
            </style>
            """
            html = html.replacingOccurrences(
                of: #"<link rel="stylesheet" href="/styles.css?v=20260726a">"#,
                with: cropCSS
            )
            html = html.replacingOccurrences(
                of: #"<script src="/app.js?v=20260726a" defer></script>"#,
                with: "<script>\(javascript)</script>"
            )
            webView.loadHTMLString(html, baseURL: resourceURL)
        }

        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            ready = true
            // Never let the webview boot half-scrolled into the mirror.
            webView.scrollView.setContentOffset(.zero, animated: false)
            // Test-only seam: `-e2eMirrorFirstRenderDelay <ms>` holds off every
            // payload render after page finish so the web app's default-theme
            // boot paint is visible long enough to capture (before/after flash
            // screenshots). Off by default.
            let base = DispatchTimeInterval.milliseconds(Self.firstRenderDelayMs ?? 0)
            let render = { [weak self] in _ = self?.renderIfReady() }
            DispatchQueue.main.asyncAfter(deadline: .now() + base, execute: render)
            // Canvas does not participate in webfont swapping: whatever face
            // ctx.font resolved to when a frame was drawn is what that frame
            // keeps. With font-display:swap and the large base64 faces, the
            // first paint can land before the theme font is usable and stay
            // wrong (Big Digit's readout). `document.fonts.ready` can resolve
            // before the face is actually usable, so re-render deterministically
            // at +400/+1200 ms after page finish — the same passes Android's
            // preview uses — and the post-font-ready RAF reveal gate below
            // guarantees the revealed frame uses the real face.
            DispatchQueue.main.asyncAfter(deadline: .now() + base + .milliseconds(400), execute: render)
            DispatchQueue.main.asyncAfter(deadline: .now() + base + .milliseconds(1200), execute: render)
        }

        /// `-e2eMirrorFirstRenderDelay <ms>` (test/screenshot only).
        static let firstRenderDelayMs: Int? = {
            let arguments = ProcessInfo.processInfo.arguments
            guard let index = arguments.firstIndex(of: "-e2eMirrorFirstRenderDelay"),
                  index + 1 < arguments.count else { return nil }
            return Int(arguments[index + 1])
        }()

        func renderIfReady() {
            // No payload yet (SwiftUI hasn't pushed the theme) — wait rather
            // than paint a default-theme face that could overwrite a later,
            // correct render.
            guard !payload.isEmpty else { return }
            guard ready, let webView, JSONSerialization.isValidJSONObject(payload),
                  let data = try? JSONSerialization.data(withJSONObject: payload),
                  let json = String(data: data, encoding: .utf8) else { return }
            let key = payloadKey
            // A different payload arrived (theme switch): hide until the new
            // theme actually paints, and freeze the last confirmed frame in the
            // overlay so the circle never flashes blank or the previous theme.
            if revealedKey != key {
                webView.alpha = 0
                revealedKey = nil
                renderToken += 1
                overlayView?.alpha = 1
            }
            let token = renderToken
            let script = """
            (() => {
              const payload = \(json);
              if (payload.theme) {
                state.themes = [payload.theme];
                state.activeThemeId = payload.theme.id;
                setTheme(payload.theme);
              }
              if (payload.config) state.config = Object.assign({}, state.config, payload.config);
              if (payload.settings) Object.assign(state, payload.settings);
              if (payload.tpms) state.tpms = payload.tpms;
              state.activePage = Number(payload.activePage || 0);
              const sample = Object.assign({ psi: 0, peakPsi: 0, zone: "ATMO", demo: false, uptimeMs: 1 }, payload.sample || {});
              state.gaugeTarget = sample;
              drawGauge(sample);
              window.__gaugeMirrorRafDone = false;
              window.__gaugeMirrorSnapshot = "";
              // document.fonts.ready resolves before lazily-USED faces (Doto /
              // SF Alien) actually fetch; force-load the readout variants so
              // the gated frame is drawn with the real face, then two RAFs to
              // guarantee compositing.
              const preload = Promise.all([
                document.fonts.load('700 126px "Doto"'),
                document.fonts.load('400 126px "Doto"'),
                document.fonts.load('italic 108px "SF Alien Encounters"'),
                document.fonts.load('italic 154px "SF Alien Encounters"'),
                document.fonts.load('400 65px "Archivo Black"'),
              ]).catch(() => {});
              Promise.all([document.fonts.ready, preload]).then(() => {
                requestAnimationFrame(() => requestAnimationFrame(() => {
                  window.__gaugeMirrorRafDone = true;
                  try {
                    window.__gaugeMirrorSnapshot = document.getElementById('gaugeCanvas').toDataURL('image/png');
                  } catch (e) {
                    window.__gaugeMirrorSnapshot = "";
                  }
                }));
              });
              return { renderedThemeID: state.activeThemeId || null };
            })();
            """
            webView.evaluateJavaScript(script) { [weak self] result, error in
                guard let self, error == nil else { return }
                var renderedThemeID: String?
                if let object = result as? [String: Any] {
                    renderedThemeID = object["renderedThemeID"] as? String
                }
                // Gate 1: the render must match the ACTUAL payload theme — the
                // web app's default-theme boot paint must never flash.
                guard Self.shouldReveal(renderedThemeID: renderedThemeID, payloadThemeID: self.payloadThemeID) else {
                    return
                }
                // Gate 2: wait for a post-font-ready RAF that actually
                // composited a frame (and its snapshot) before revealing.
                self.pollForRevealSnapshot(webView: webView, token: token)
            }
        }

        /// Polls for the post-font-ready RAF snapshot. Reveals only when the
        /// frame has actually been composited with the theme face usable.
        private func pollForRevealSnapshot(webView: WKWebView, token: Int, attempts: Int = 0) {
            guard revealedKey == nil, token == renderToken else { return }
            webView.evaluateJavaScript(
                "({ done: window.__gaugeMirrorRafDone === true, snap: window.__gaugeMirrorSnapshot || '' })"
            ) { [weak self] result, error in
                guard let self, error == nil else { return }
                if let object = result as? [String: Any] {
                    let rafDone = object["done"] as? Bool == true
                    let hasSnapshot = (object["snap"] as? String).map { !$0.isEmpty } ?? false
                    if Self.shouldRevealFrame(rafDone: rafDone, hasSnapshot: hasSnapshot) {
                        self.revealedKey = self.payloadKey
                        self.applySnapshot(object["snap"] as? String ?? "")
                        webView.alpha = 1
                        self.overlayView?.alpha = 0
                        return
                    }
                }
                // Ceiling ~2 s per render pass; the +400/+1200 re-renders reset
                // the flags and start a fresh poll, so a slow first paint is
                // still caught.
                guard attempts < 40 else { return }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
                    self.pollForRevealSnapshot(webView: webView, token: token, attempts: attempts + 1)
                }
            }
        }

        private func applySnapshot(_ dataURL: String) {
            guard let overlayView, dataURL.hasPrefix("data:image/png;base64,"),
                  let data = Data(base64Encoded: String(dataURL.dropFirst("data:image/png;base64,".count))),
                  let image = UIImage(data: data) else { return }
            overlayView.image = image
        }
    }
}