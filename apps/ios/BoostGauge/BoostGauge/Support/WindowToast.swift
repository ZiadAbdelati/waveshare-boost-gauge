import UIKit
import SwiftUI

/// Window-level transient indicator: lives in its own UIWindow above the app
/// with userInteractionEnabled = false (and a hitTest returning nil) so it can
/// never intercept button taps — the SwiftUI overlay variant regressed taps
/// app-wide. The window is created ONCE and reused; only the capsule content
/// changes per show. The hide is a single cancellable MainActor Task so a late
/// BLE completion that re-fires a toast doesn't leave a stuck window.
@MainActor
enum WindowToast {
    private static var window: PassthroughWindow?
    private static var hosting: UIHostingController<ToastCapsule>?
    private static var hideTask: Task<Void, Never>?

    static func show(_ text: String, color: UIColor) {
        cancel()
        let win = window ?? makeWindow()
        let capsule = ToastCapsule(text: text, color: Color(color))
        if let hosting {
            hosting.rootView = capsule
        } else {
            let hc = UIHostingController(rootView: capsule)
            hc.view.backgroundColor = .clear
            win.rootViewController = hc
            hosting = hc
        }
        win.isHidden = false
        hideTask = Task { [weak win] in
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard !Task.isCancelled else { return }
            win?.isHidden = true
        }
    }

    static func cancel() {
        hideTask?.cancel()
        hideTask = nil
    }

    private static func makeWindow() -> PassthroughWindow {
        guard let scene = UIApplication.shared.connectedScenes
            .compactMap({ $0 as? UIWindowScene }).first else {
            fatalError("WindowToast requires a foreground UIWindowScene")
        }
        let win = PassthroughWindow(windowScene: scene)
        win.windowLevel = .alert + 1
        win.isHidden = true
        window = win
        return win
    }
}

/// UIWindow that never participates in touch routing.
final class PassthroughWindow: UIWindow {
    override func hitTest(_ point: CGPoint, with event: UIEvent?) -> UIView? { nil }
}

struct ToastCapsule: View {
    let text: String
    let color: Color

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: color == Color(.systemGreen)
                  ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
            Text(text)
        }
        .font(.footnote.weight(.medium))
        .foregroundStyle(.white)
        .padding(.horizontal, 14)
        .padding(.vertical, 9)
        .background(.ultraThinMaterial, in: Capsule())
        .overlay(Capsule().strokeBorder(color.opacity(0.55), lineWidth: 1))
        .shadow(color: .black.opacity(0.25), radius: 12, y: 4)
        .padding(.bottom, 110)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottom)
        .allowsHitTesting(false)
    }
}
