import SwiftUI

extension GaugeConnectionState {
    var color: Color {
        switch self {
        case .connected: return .green
        case .connecting: return .orange
        case .unreachable: return .red
        case .notConfigured, .notConnected: return .gray
        }
    }
}

struct GaugeConnectionBadge: View {
    let state: GaugeConnectionState
    let target: String?

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: symbol)
                .foregroundColor(state.color)
            Text(text)
                .font(.footnote)
                .foregroundColor(state.color)
        }
    }

    private var symbol: String {
        switch state {
        case .connected: return "checkmark.circle.fill"
        case .connecting: return "circle.dotted"
        case .unreachable: return "exclamationmark.triangle.fill"
        case .notConfigured, .notConnected: return "circle.slash"
        }
    }

    private var text: String {
        switch state {
        case .connected:
            return target.map { "Connected to \($0)" } ?? "Connected"
        case .connecting:
            return target.map { "Connecting to \($0)…" } ?? "Connecting…"
        case .unreachable(let error):
            return "Unreachable: \(error)"
        case .notConfigured:
            return "No gauge configured — enter a host above or use BLE"
        case .notConnected:
            return "Not connected"
        }
    }
}
