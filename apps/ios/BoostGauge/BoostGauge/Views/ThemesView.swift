import SwiftUI

struct ThemesView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = ThemesViewModel()

    var body: some View {
        NavigationView {
            List {
                if let error = vm.errorMessage {
                    Text(error)
                        .font(.footnote)
                        .foregroundColor(.orange)
                }
                ForEach(vm.themes) { theme in
                    Button(action: { Task { await vm.select(theme.id) } }) {
                        themeRow(theme)
                    }
                    .disabled(vm.isLoading)
                }
            }
            .navigationTitle("Themes")
            .overlay {
                if vm.isLoading && vm.themes.isEmpty {
                    ProgressView()
                }
            }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.load() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear { vm.reset(transport: session.transport) }
            .task { await vm.load() }
        }
    }

    private func themeRow(_ theme: Theme) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(theme.name)
                        .font(.headline)
                    if theme.customized == true {
                        Text("custom")
                            .font(.caption2.weight(.semibold))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(Capsule().fill(Color.blue.opacity(0.15)))
                            .foregroundColor(.blue)
                    }
                }
                if let style = theme.style {
                    Text(style)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            Spacer()
            if let colors = theme.colors {
                HStack(spacing: 3) {
                    ForEach(
                        [colors.face, colors.track, colors.boost, colors.overboost, colors.zero],
                        id: \.self
                    ) { hex in
                        if let hex {
                            Circle()
                                .fill(Color(hex: hex))
                                .frame(width: 14, height: 14)
                        }
                    }
                }
            }
            if vm.activeThemeID == theme.id {
                Image(systemName: "checkmark.circle.fill")
                    .foregroundColor(.green)
            }
        }
        .contentShape(Rectangle())
    }
}
