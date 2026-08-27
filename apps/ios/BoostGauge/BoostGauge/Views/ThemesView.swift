import SwiftUI

struct ThemesView: View {
    @EnvironmentObject var session: AppSession
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @StateObject private var vm = ThemesViewModel()
    @State private var expandedThemes: Set<String> = []

    var body: some View {
        NavigationStack {
            Group {
                // Landscape uses the extra width as two panes: the exact
                // dashboard preview on the left, the theme list on the right.
                // The preview pane is a width-relative fraction of the screen
                // (never a fixed 360 pt — that cramped the Pro Max into its
                // left third), and both panes share one background so there is
                // no seam where the pane boundary falls.
                if verticalSizeClass == .compact {
                    HStack(alignment: .top, spacing: 0) {
                        previewPane
                        themesList
                    }
                    .frame(maxWidth: .infinity)
                } else {
                    List {
                        if let activeTheme {
                            Section("Preview") {
                                themePreview(activeTheme)
                                    .listRowInsets(EdgeInsets())
                                    .listRowBackground(Color.clear)
                            }
                        }
                        if let error = vm.errorMessage {
                            errorRow(error)
                        }
                        themesSection
                    }
                    .gaugeScrollBottomMargin()
                }
            }
            .navigationTitle("Themes")
            .overlay {
                if vm.isLoading && vm.themes.isEmpty { ProgressView() }
            }
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.load() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                vm.reset(transport: session.transport)
                Task { await vm.resyncActiveTheme() }
            }
            .task { await vm.load() }
        }
    }

    @ViewBuilder
    private var previewPane: some View {
        if let activeTheme {
            ScrollView {
                themePreview(activeTheme)
                    .padding(.horizontal, 28)
                    .frame(maxWidth: .infinity)
            }
            // Share the List's grouped background so the preview pane never
            // paints a different shade (the portrait row uses the List's own
            // background; the landscape sidebar must match it exactly).
            .scrollContentBackground(.hidden)
            .background(Color(.systemGroupedBackground))
            .containerRelativeFrame(.horizontal) { length, _ in
                min(max(length * 0.4, 300), 420)
            }
        }
    }

    private var themesList: some View {
        List {
            if let error = vm.errorMessage {
                errorRow(error)
            }
            themesSection
        }
        .frame(maxWidth: .infinity)
        .gaugeScrollBottomMargin()
    }

    @ViewBuilder
    private func errorRow(_ error: String) -> some View {
        Text(error)
            .font(.footnote)
            .foregroundColor(.orange)
    }

    @ViewBuilder
    private var themesSection: some View {
        Section("Gauge themes") {
            ForEach(vm.themes) { theme in
                DisclosureGroup(isExpanded: expansionBinding(theme.id)) {
                    themeOptions(theme)
                        .padding(.vertical, 6)
                        // A List indents DisclosureGroup content ~20 pt
                        // below the label; pull it back so the options
                        // panel shares the theme title's leading axis.
                        .padding(.leading, -20)
                } label: {
                    Button(action: { Task { await vm.select(theme.id) } }) {
                        themeRow(theme)
                    }
                    .buttonStyle(.plain)
                    .disabled(vm.isLoading)
                }
            }
        }
    }

    private var activeTheme: Theme? {
        vm.themes.first { $0.id == vm.activeThemeID } ?? vm.themes.first
    }

    private func expansionBinding(_ id: String) -> Binding<Bool> {
        Binding(
            get: { expandedThemes.contains(id) },
            set: { expanded in
                if expanded { expandedThemes.insert(id) } else { expandedThemes.remove(id) }
            }
        )
    }

    private func themePreview(_ theme: Theme) -> some View {
        // Circular cut-out with the web UI's `.gauge-device` bezel reproduced as
        // concentric strokes: an 8 px `#0c0e12` pod ring and a hairline black
        // rim, reading as a recessed pod on both light and dark page
        // backgrounds. Corners stay transparent; no offset shadow (it reads
        // harsh against a light page).
        GaugeMirrorWebView(payload: vm.previewPayload(for: theme))
            .aspectRatio(1, contentMode: .fit)
            .clipShape(Circle())
            .overlay {
                Circle().stroke(Color.white.opacity(0.05), lineWidth: 1)
            }
            .background {
                ZStack {
                    Circle().stroke(Color(hex: "#0c0e12"), lineWidth: 16)
                    Circle().stroke(Color.black.opacity(0.9), lineWidth: 2)
                }
            }
            // The bezel ring draws outside the square's frame, so the row must
            // grow on every side or the ring is flat-clipped at the first row's
            // top/bottom edge. No offset shadow: the web bezel is a flat recessed
            // pod ring, and a shadow reads harsh against a light page.
            .padding(.vertical, 20)
            .padding(.horizontal, 8)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Exact dashboard preview of \(theme.name)")
    }

    private func themeRow(_ theme: Theme) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    Text(theme.name).font(.headline)
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
                    Text(style).font(.caption).foregroundColor(.secondary)
                }
            }
            Spacer()
            palette(theme)
            if vm.activeThemeID == theme.id {
                Image(systemName: "checkmark.circle.fill").foregroundColor(.green)
            }
        }
        .contentShape(Rectangle())
    }

    private func palette(_ theme: Theme) -> some View {
        HStack(spacing: 3) {
            ForEach(ThemesViewModel.zoneKeys, id: \.self) { key in
                if let hex = vm.colorHex(for: theme, key: key) {
                    Circle()
                        .fill(Color(hex: hex))
                        .frame(width: 11, height: 11)
                        .accessibilityLabel("\(paletteLabel(key)) \(hex)")
                }
            }
        }
    }

    @ViewBuilder
    private func themeOptions(_ theme: Theme) -> some View {
        VStack(alignment: .leading, spacing: 14) {
            GroupBox("Zone colors") {
                VStack(spacing: 12) {
                    ForEach(ThemesViewModel.zoneKeys, id: \.self) { key in
                        if let hex = vm.colorHex(for: theme, key: key) {
                            ColorPicker(
                                paletteLabel(key),
                                selection: colorBinding(theme: theme, key: key),
                                supportsOpacity: false
                            )
                            .accessibilityIdentifier("theme.\(theme.id).color.\(key)")
                        }
                    }
                }
            }
            .padding(.bottom, 4)
            switch theme.id {
            case "dyno-cell":
                Toggle("Gradient fill", isOn: $vm.arcGradient)
            case "vault-tec":
                TextField("Face color (#RRGGBB)", text: $vm.vaultFace)
                    .textInputAutocapitalization(.never)
                    .disableAutocorrection(true)
                Stepper("Vignette: \(vm.vaultVignette)%", value: $vm.vaultVignette, in: 0...90)
                Toggle("Red needle", isOn: $vm.vaultNeedleRed)
                Toggle("Counterweight tail", isOn: $vm.vaultNeedleTail)
            case "night-city":
                Toggle("Gradient fill", isOn: $vm.hudGradient)
                Toggle("True black background", isOn: $vm.hudTrueBlack)
            case "big-digit":
                Toggle("Static background", isOn: $vm.bigDigitStaticBg)
                Toggle("Color the readout", isOn: $vm.bigDigitColorText)
                if vm.bigDigitStaticBg {
                    ColorPicker(
                        "Static background color",
                        selection: hexBinding($vm.bigDigitStaticColor),
                        supportsOpacity: false
                    )
                }
                if !vm.bigDigitColorText {
                    ColorPicker(
                        "Readout text color",
                        selection: hexBinding($vm.bigDigitTextColor),
                        supportsOpacity: false
                    )
                }
            case "neon":
                Picker("Layout", selection: $vm.neonLayout) {
                    Text("Tube").tag(0)
                    Text("Segments").tag(1)
                    Text("Marquee").tag(2)
                }
                Picker("Preset", selection: $vm.neonPreset) {
                    Text("Violet").tag(0)
                    Text("Miami").tag(1)
                    Text("Toxic").tag(2)
                    Text("Blood Moon").tag(3)
                }
                Picker("Readout font", selection: $vm.neonFont) {
                    Text("SF Alien").tag(0)
                    Text("Doto").tag(1)
                }
                if vm.neonLayout == 2 {
                    Toggle("Marquee spin", isOn: $vm.neonMarqueeSpin)
                }
            default:
                Text("This theme has no additional options.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            Button(action: { Task { await vm.saveOptions(for: theme.id) } }) {
                Text("Apply \(theme.name) options")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .contentShape(Rectangle())
            .disabled(vm.isLoading)
            .padding(.top, 4)
            if theme.customized == true {
                Button("Reset to default colors", role: .destructive) {
                    Task { await vm.resetColors(for: theme.id) }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
                .disabled(vm.isLoading)
            }
        }
    }

    private func paletteLabel(_ key: String) -> String {
        switch key {
        case "face": return "Face"
        case "track": return "Track"
        case "text": return "Text"
        case "muted": return "Muted"
        case "vacuum": return "Vacuum"
        case "boost": return "Boost"
        case "overboost": return "Overboost"
        case "zero": return "Zero marker"
        default: return key.capitalized
        }
    }

    private func colorBinding(theme: Theme, key: String) -> Binding<Color> {
        Binding(
            get: { Color(hex: vm.colorHex(for: theme, key: key) ?? "#000000") },
            set: { vm.setColor($0.hexRGB, for: theme.id, key: key) }
        )
    }

    private func hexBinding(_ value: Binding<String>) -> Binding<Color> {
        Binding(
            get: { Color(hex: value.wrappedValue) },
            set: { value.wrappedValue = $0.hexRGB }
        )
    }
}

private extension Color {
    var hexRGB: String {
        let color = UIColor(self)
        var red: CGFloat = 0
        var green: CGFloat = 0
        var blue: CGFloat = 0
        var alpha: CGFloat = 0
        guard color.getRed(&red, green: &green, blue: &blue, alpha: &alpha) else { return "#000000" }
        return String(format: "#%02X%02X%02X", Int(red * 255), Int(green * 255), Int(blue * 255))
    }
}
