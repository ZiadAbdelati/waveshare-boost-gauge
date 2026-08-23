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
        .task { session.startConnectionMonitoring() }
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
