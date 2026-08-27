import SwiftUI

struct RootView: View {
    @EnvironmentObject var session: AppSession
    @State private var selectedTab = RootView.initialTabIndex()

    var body: some View {
        TabView(selection: $selectedTab) {
            StatusView()
                .tabItem { Label("Status", systemImage: "gauge") }
                .tag(0)
            ThemesView()
                .tabItem { Label("Themes", systemImage: "paintpalette") }
                .tag(1)
            SettingsView()
                .tabItem { Label("Settings", systemImage: "gearshape") }
                .tag(2)
            CalibrationView()
                .tabItem { Label("Calibrate", systemImage: "target") }
                .tag(3)
            LogsView()
                .tabItem { Label("Logs", systemImage: "list.bullet.rectangle") }
                .tag(4)
        }
        .overlay(alignment: .top) {
            if session.hardwareBleE2ERequested {
                VStack(spacing: 2) {
                    Text(session.hardwareBleE2EStatus ?? "STARTING")
                        .accessibilityIdentifier("hardwareBLEE2EStatus")
                    ForEach(session.hardwareBleE2ESteps.keys.sorted(), id: \.self) { step in
                        Text(session.hardwareBleE2ESteps[step] ?? "PENDING")
                            .accessibilityLabel(session.hardwareBleE2ESteps[step] ?? "PENDING")
                            .accessibilityIdentifier("hardwareBLEE2EStep.\(step)")
                    }
                }
                .font(.caption2.monospaced())
                .padding(4)
                .background(.thinMaterial)
            }
        }
        .task {
            session.startConnectionMonitoring()
            await session.startHardwareBleE2EIfRequested()
        }
    }

    private static func initialTabIndex() -> Int {
        let arguments = ProcessInfo.processInfo.arguments
        if let index = arguments.firstIndex(of: "-e2eTab"), index + 1 < arguments.count {
            switch arguments[index + 1].lowercased() {
            case "status": return 0
            case "themes": return 1
            case "settings": return 2
            case "calibrate": return 3
            case "logs": return 4
            default: return 0
            }
        }
        return 0
    }
}
