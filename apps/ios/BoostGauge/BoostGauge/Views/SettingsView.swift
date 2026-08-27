import SwiftUI
import UIKit

struct SettingsView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = SettingsViewModel()

    @State private var bleDevices: [BleDevice] = []
    @State private var isScanning = false
    @State private var isConnecting = false
    @State private var hasCompletedBLEScan = false

    var body: some View {
        NavigationStack {
            List {
                if let error = vm.errorMessage {
                    Section {
                        Text(error)
                            .font(.footnote)
                            .foregroundColor(.orange)
                    }
                }
                if let message = vm.savedMessage {
                    Section {
                        Text(message)
                            .font(.footnote)
                            .foregroundColor(.green)
                    }
                }
                Section {
                    NavigationLink(destination: connectionPage) {
                        Label("Connection", systemImage: "wave.3.right")
                    }
                    NavigationLink(destination: displayPage) {
                        Label("Display", systemImage: "display")
                    }
                    NavigationLink(destination: rangePage) {
                        Label("Range", systemImage: "arrow.left.and.right")
                    }
                    NavigationLink(destination: themeFlagsPage) {
                        Label("Theme & demo", systemImage: "paintpalette")
                    }
                    NavigationLink(destination: clockPage) {
                        Label("Clock & timezone", systemImage: "clock")
                    }
                    NavigationLink(destination: tpmsPage) {
                        Label("TPMS", systemImage: "tirepressure")
                    }
                    NavigationLink(destination: obdScannerPage) {
                        Label("OBD2 Scanner", systemImage: "car")
                    }
                }
            }
.gaugeScrollBottomMargin()
                        .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.loadAll() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                session.refreshBLELinkState()
                vm.reset(transport: session.transport)
            }
            .onChange(of: session.transportID) { _ in
                Task { await vm.loadAll() }
            }
            .onChange(of: session.connectionState) { state in
                if state == .connected {
                    vm.reset(transport: session.transport)
                    Task { await vm.loadAll() }
                }
            }
            .task { await vm.loadAll() }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
                session.refreshBLELinkState()
            }
        }
    }

    private var connectionPage: some View {
        Form { transportSection }
            .navigationTitle("Connection")
    }

    private var displayPage: some View {
        Form { displaySection }
            .navigationTitle("Display")
    }

    private var rangePage: some View {
        Form { rangeSection }
            .navigationTitle("Range")
    }

    private var themeFlagsPage: some View {
        Form { themeFlagsSection }
            .navigationTitle("Theme & demo")
    }

    private var clockPage: some View {
        Form { clockSection }
            .navigationTitle("Clock & timezone")
    }

    private var tpmsPage: some View {
        Form { tpmsSection }
            .navigationTitle("TPMS")
    }

    private var obdScannerPage: some View {
        Form { obdSection }
            .navigationTitle("OBD2 Scanner")
            .onAppear { vm.startOBDPolling() }
            .onDisappear { vm.stopOBDPolling() }
    }

    private var transportSection: some View {
        Section("Connection") {
            Button(action: { Task { await scan() } }) {
                if isScanning {
                    HStack {
                        ProgressView()
                        Text("Scanning…")
                    }
                } else {
                    Label("Scan for gauges", systemImage: "wave.3.right")
                }
            }
            .disabled(isScanning)
            // Peer remembered = a persisted BLE selection exists (the Saved
            // gauge row is the remembered identity, per PARITY.md row 1).
            let peerRemembered = session.kind == .ble && session.lastPeerID() != nil
            if peerRemembered {
                savedGaugeRow
            }
            // Never say "No gauge found" while a peer is remembered — the saved
            // row already answers "where is my gauge" and the two labels would
            // contradict each other.
            if hasCompletedBLEScan && bleDevices.isEmpty && !isScanning && !peerRemembered {
                Text("No gauge found. Make sure the gauge is advertising.")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            ForEach(bleDevices) { device in
                Button(action: { Task { await connect(device) } }) {
                    HStack {
                        VStack(alignment: .leading, spacing: 2) {
                            Text(device.name)
                                .foregroundColor(.primary)
                            Text("RSSI \(device.rssi)")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                        Spacer()
                        if isConnecting {
                            ProgressView()
                        }
                    }
                }
                .disabled(isConnecting)
            }
            if session.connectionState == .connected, let info = session.bleInfo {
                VStack(alignment: .leading, spacing: 2) {
                    Text(info.name)
                        .font(.headline)
                    Text("firmware \(info.firmware) · api \(info.api)")
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
            }
            if session.kind == .ble {
                // The single status string for this surface. The reconnecting
                // pill renders the Android-identical "Reconnecting… (attempt N)"
                // banner; the saved row never duplicates it.
                switch session.connectionState {
                case .connected:
                    Label("Connected to \(session.blePeerName ?? "gauge")", systemImage: "checkmark.circle.fill")
                        .foregroundColor(.green)
                case .connecting:
                    Label(session.bleConnectionMessage ?? "Connecting…", systemImage: "arrow.triangle.2.circlepath")
                        .foregroundColor(isReconnecting ? .orange : .secondary)
                case .notConnected, .notConfigured, .unreachable:
                    Label("Not connected", systemImage: "bolt.slash.circle")
                        .foregroundColor(.secondary)
                }
            }
            if session.kind == .ble,
               session.connectionState == .connected || session.connectionState == .connecting {
                Button("Disconnect", role: .destructive) {
                    session.disconnectBLE()
                }
            }
        }
    }

    private var displaySection: some View {
        Section("Display") {
            if vm.config == nil {
                unavailableRow("gauge", loading: vm.isLoading)
            } else {
Stepper("Brightness high: \(vm.brightnessHigh)%", value: $vm.brightnessHigh, in: 1...100)
            Stepper("Brightness low: \(vm.brightnessLow)%", value: $vm.brightnessLow, in: 1...100)
            Toggle("Dim schedule", isOn: $vm.dimEnabled)
            if vm.dimEnabled {
                Stepper("Start: \(minutesText(vm.dimStartMinutes))", value: $vm.dimStartMinutes, in: 0...(24 * 60 - 1))
                Stepper("End: \(minutesText(vm.dimEndMinutes))", value: $vm.dimEndMinutes, in: 0...(24 * 60 - 1))
            }
            Toggle("Companion app advertising", isOn: $vm.appBle)
                .font(.footnote)
                .foregroundColor(.secondary)
            Button("Save gauge settings") {
                Task { await vm.saveConfig() }
            }
            }
        }
    }

    private var rangeSection: some View {
        Section("Range") {
            if vm.config == nil {
                unavailableRow("gauge", loading: vm.isLoading)
            } else {
            HStack {
                Text("psiMin")
                    .foregroundColor(.secondary)
                Spacer()
                TextField("−15.0", value: $vm.psiMin, format: .number)
                    .keyboardType(.decimalPad)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 90)
            }
            HStack {
                Text("psiMax")
                    .foregroundColor(.secondary)
                Spacer()
                TextField("10.0", value: $vm.psiMax, format: .number)
                    .keyboardType(.decimalPad)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 90)
            }
            HStack {
                Text("psiOverboost")
                    .foregroundColor(.secondary)
                Spacer()
                TextField("8.0", value: $vm.psiOverboost, format: .number)
                    .keyboardType(.decimalPad)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 90)
            }
            HStack {
                Text("zeroAngle")
                    .foregroundColor(.secondary)
                Spacer()
                TextField("236.25", value: $vm.zeroAngle, format: .number)
                    .keyboardType(.decimalPad)
                    .multilineTextAlignment(.trailing)
                    .frame(width: 90)
            }
            Button("Save gauge settings") {
                Task { await vm.saveConfig() }
            }
            }
        }
    }

    private var themeFlagsSection: some View {
        Section("Theme & demo") {
            if vm.themeFlags == nil {
                unavailableRow("theme", loading: vm.isLoading)
            } else {
            Toggle("Demo mode", isOn: $vm.demoMode)
            Picker("Demo waveform", selection: $vm.demoFastSweep) {
                Text("Organic swell").tag(false)
                Text("Linear sweep (9.789 psi/s)").tag(true)
            }
            .disabled(!vm.demoMode)
            Picker("Rotation", selection: $vm.rotation) {
                ForEach([0, 90, 180, 270], id: \.self) { degrees in
                    Text("\(degrees)°").tag(degrees)
                }
            }
            Toggle("Region double-buffer", isOn: $vm.regionDBuf)
            Toggle("TE sync", isOn: $vm.teSync)
            Toggle("TE scanline", isOn: $vm.teScanline)
            Toggle("Pixel shift", isOn: $vm.pixelShift)
            if vm.pixelShift {
                Stepper("Pixel shift interval: \(vm.pixelShiftSec)s", value: $vm.pixelShiftSec, in: 30...3600, step: 30)
            }
            Button("Save theme settings") {
                Task { await vm.saveThemeFlags() }
            }
            }
        }
    }

    private var tpmsSection: some View {
        Section("TPMS") {
            if vm.tpmsConfig == nil {
                unavailableRow("TPMS", loading: vm.isLoading)
            } else {
            Stepper("Low pressure: \(Format.psi(vm.tpmsLowPsi)) psi", value: $vm.tpmsLowPsi, in: 14.5...58.0, step: 0.5)
            Picker("Stale after", selection: $vm.tpmsStaleAfterMs) {
                ForEach([5000, 10000, 15000, 30000, 60000, 120000], id: \.self) { ms in
                    Text(staleText(ms)).tag(ms)
                }
            }
            Toggle("TPMS BLE link", isOn: $vm.tpmsBle)
            Button("Save TPMS settings") {
                Task { await vm.saveTPMSConfig() }
            }
            }
        }
    }

    private var obdSection: some View {
        Section("OBD2 Scanner") {
            Text("Gauge → OBD2 dongle link")
                .font(.footnote)
                .foregroundColor(.secondary)
            HStack(spacing: 6) {
                Circle()
                    .fill(obdPillColor)
                    .frame(width: 8, height: 8)
                Text(obdPillText)
                    .font(.footnote)
                    .foregroundColor(obdPillColor)
            }
            if let lastError = vm.obdState?.lastError, lastError != 0 {
                Text("Last error \(String(format: "0x%04X", lastError))")
                    .font(.footnote)
                    .foregroundColor(.secondary)
            }
            HStack {
                Text("Peer")
                    .foregroundColor(.secondary)
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text(vm.obdPeerName ?? "—")
                    if let addr = vm.obdPeerAddr, !addr.isEmpty {
                        Text(addr)
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
            }
            Button("Forget", role: .destructive) {
                Task { await vm.forgetOBDPeer() }
            }
            .disabled(obdPhase == .idle || vm.isForgettingOBDPeer)
            if vm.isForgettingOBDPeer {
                HStack {
                    ProgressView()
                    Text("Forgetting peer…")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
        }
    }

    private var obdPhase: OBDPhase {
        vm.obdState?.phase ?? .idle
    }

    private var obdPillText: String {
        switch obdPhase {
        case .idle: return "Idle"
        case .scanning: return "Scanning"
        case .connecting(let name): return name.map { "Connecting to \($0)" } ?? "Connecting…"
        case .connected: return "Connected"
        }
    }

    private var obdPillColor: Color {
        switch obdPhase {
        case .idle: return .gray
        case .scanning: return .orange
        case .connecting: return .orange
        case .connected: return .green
        }
    }

    private var clockSection: some View {
        Section("Clock & timezone") {
            if vm.config == nil {
                unavailableRow("clock", loading: vm.isLoading)
            } else {
            Menu {
                ForEach(SettingsViewModel.timezoneOptions) { option in
                    Button {
                        Task { await vm.applyTimezoneOption(option.id) }
                    } label: {
                        if vm.timezoneSelection == option.id {
                            Label(option.label, systemImage: "checkmark")
                        } else {
                            Text(option.label)
                        }
                    }
                }
                Button {
                    Task { await vm.applyTimezoneOption(SettingsViewModel.customTimezoneID) }
                } label: {
                    if vm.timezoneSelection == SettingsViewModel.customTimezoneID {
                        Label("Custom…", systemImage: "checkmark")
                    } else {
                        Text("Custom…")
                    }
                }
            } label: {
                HStack {
                    Text("Timezone")
                        .foregroundColor(.primary)
                    Spacer()
                    Text(currentTimezoneLabel)
                        .foregroundColor(.secondary)
                    Image(systemName: "chevron.up.chevron.down")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
            if vm.timezoneSelection == SettingsViewModel.customTimezoneID {
                Stepper("Timezone offset: \(vm.timezoneOffset) min", value: $vm.timezoneOffset, in: -720...840, step: 15)
                HStack {
                    Text("TZ string")
                        .foregroundColor(.secondary)
                    TextField("UTC0", text: $vm.timezoneTZ)
                        .autocapitalization(.none)
                        .disableAutocorrection(true)
                        .multilineTextAlignment(.trailing)
                }
                Button("Save custom timezone") {
                    Task { await vm.saveTimezoneCustom() }
                }
            }
            Button("Sync timezone to gauge") {
                Task { await vm.syncTimezone() }
            }
            }
        }
    }

    private var currentTimezoneLabel: String {
        SettingsViewModel.timezoneOptions.first { $0.id == vm.timezoneSelection }?.label
            ?? (vm.timezoneSelection == SettingsViewModel.customTimezoneID ? "Custom…" : vm.timezoneTZ)
    }

    // The transport call resumes off-main; @MainActor keeps the post-await
    // @Published writes (vm.errorMessage/savedMessage) on the main thread.
    @MainActor
    private func scan() async {
        isScanning = true
        hasCompletedBLEScan = false
        bleDevices = []
        vm.errorMessage = nil
        vm.savedMessage = nil
        do {
            bleDevices = try await session.scanBLE()
            hasCompletedBLEScan = true
        } catch {
            vm.errorMessage = error.localizedDescription
        }
        isScanning = false
    }

    @MainActor
    private func connect(_ device: BleDevice) async {
        isConnecting = true
        vm.errorMessage = nil
        vm.savedMessage = nil
        do {
            _ = try await session.connectBLE(to: device)
            bleDevices = []
            hasCompletedBLEScan = false
        } catch {
            vm.errorMessage = error.localizedDescription
        }
        isConnecting = false
    }

    private func minutesText(_ minutes: Int) -> String {
        String(format: "%02d:%02d", minutes / 60, minutes % 60)
    }

    private func staleText(_ ms: Int) -> String {
        ms >= 60000 ? "\(ms / 60000) min" : "\(ms / 1000) s"
    }

    private var savedGaugeName: String {
        session.blePeerName ?? "BoostGauge"
    }

    private var isReconnecting: Bool {
        session.connectionState == .connecting && session.reconnectAttempt != nil
    }

    /// Saved-gauge row per the parity visibility matrix: visible whenever the
    /// peer is remembered AND not connected, hidden while connected. A Connect
    /// action is offered only when the link is truly down; while the reconnect
    /// loop retries, the row shows identity alone and the pill owns the
    /// "Reconnecting… (attempt N)" banner (no Connect button).
    @ViewBuilder
    private var savedGaugeRow: some View {
        let action = AppSession.savedGaugeAction(
            connectionState: session.connectionState,
            reconnectAttempt: session.reconnectAttempt
        )
        if action != .hidden, let savedID = session.lastPeerID() {
            HStack(spacing: 12) {
                Image(systemName: "bolt.circle")
                    .foregroundColor(.secondary)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Saved gauge")
                        .font(.caption)
                        .foregroundColor(.secondary)
                    Text(savedGaugeName)
                        .font(.headline)
                    Text(savedID)
                        .font(.caption)
                        .foregroundColor(.secondary)
                }
                Spacer()
                if action == .connect {
                    Button("Connect") { Task { await connectSaved() } }
                        .disabled(isConnecting)
                }
            }
        }
    }

    @MainActor
    private func connectSaved() async {
        isConnecting = true
        vm.errorMessage = nil
        vm.savedMessage = nil
        do {
            _ = try await session.connectToSavedGauge()
            bleDevices = []
            hasCompletedBLEScan = false
        } catch {
            vm.errorMessage = error.localizedDescription
        }
        isConnecting = false
    }

    @ViewBuilder
    private func unavailableRow(_ name: String, loading: Bool) -> some View {
        if loading {
            ProgressView("Loading \(name) settings…")
        } else {
            Text("\(name.capitalized) settings unavailable")
                .font(.footnote)
                .foregroundColor(.secondary)
        }
    }
}
