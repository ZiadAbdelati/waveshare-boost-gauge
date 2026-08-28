import SwiftUI
import UIKit

struct SettingsView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = SettingsViewModel()

    @State private var bleDevices: [BleDevice] = []
    @State private var isScanning = false
    @State private var isConnecting = false
    @State private var hasCompletedBLEScan = false
    @State private var joinSheet: WifiJoinTarget?

    var body: some View {
        NavigationStack {
            List {
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
                    NavigationLink(destination: demoModePage) {
                        Label("Demo mode", systemImage: "play.circle")
                    }
                    NavigationLink(destination: clockPage) {
                        Label("Clock & timezone", systemImage: "clock")
                    }
                    NavigationLink(destination: obdScannerPage) {
                        Label("TPMS & OBD2", systemImage: "car")
                    }
                    NavigationLink(destination: wifiPage) {
                        Label("Wi-Fi", systemImage: "wifi")
                    }
                }
                Section {
                    NavigationLink(destination: aboutPage) {
                        Label("About", systemImage: "info.circle")
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

    private var demoModePage: some View {
        Form { demoModeSection }
            .navigationTitle("Demo mode")
    }

    private var clockPage: some View {
        Form { clockSection }
            .navigationTitle("Clock & timezone")
    }

    private var obdScannerPage: some View {
        Form { obdSection }
            .navigationTitle("OBD2 Scanner")
            .onAppear { vm.startOBDPolling() }
            .onDisappear { vm.stopOBDPolling() }
    }

    private var wifiPage: some View {
        Form { wifiSection }
            .navigationTitle("Wi-Fi")
            .onAppear { Task { await vm.refreshWifiStatus() } }
            .sheet(item: $joinSheet) { target in
                NavigationStack {
                    WifiJoinView(ssid: target.ssid) { ssid, pass in
                        vm.wifiSSID = ssid
                        vm.wifiPassword = pass
                        Task { await vm.saveWifi() }
                    }
                    .navigationTitle("Join Network")
                    .navigationBarTitleDisplayMode(.inline)
                }
            }
    }

    private var aboutPage: some View {
        Form { aboutSection }
            .navigationTitle("About")
    }

    private var transportSection: some View {
        Section {
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
            ForEach(filteredBleDevices) { device in
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
        Group {
            if vm.config == nil || vm.themeFlags == nil {
                Section { unavailableRow("gauge", loading: vm.isLoading) }
            } else {
                Section("Brightness") {
                    Stepper("Brightness high: \(vm.brightnessHigh)%", value: $vm.brightnessHigh, in: 1...100)
                    Stepper("Brightness low: \(vm.brightnessLow)%", value: $vm.brightnessLow, in: 1...100)
                }
                Section("Dim schedule") {
                    Toggle("Dim schedule", isOn: $vm.dimEnabled)
                    if vm.dimEnabled {
                        DatePicker("Start", selection: Binding(
                            get: { Date(minutes: vm.dimStartMinutes) },
                            set: { vm.dimStartMinutes = $0.minutesSinceMidnight }
                        ), displayedComponents: .hourAndMinute)
                        DatePicker("End", selection: Binding(
                            get: { Date(minutes: vm.dimEndMinutes) },
                            set: { vm.dimEndMinutes = $0.minutesSinceMidnight }
                        ), displayedComponents: .hourAndMinute)
                    }
                }
                Section("Panel") {
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
                }
                Section {
                    Button("Save display settings") {
                        Task { await vm.saveDisplay() }
                    }
                }
            }
        }
    }

    private var rangeSection: some View {
        Section {
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

    private var demoModeSection: some View {
        Section {
            if vm.themeFlags == nil {
                unavailableRow("theme", loading: vm.isLoading)
            } else {
                Toggle("Demo mode", isOn: $vm.demoMode)
                Picker("Demo waveform", selection: $vm.demoFastSweep) {
                    Text("Organic swell").tag(false)
                    Text("Linear sweep").tag(true)
                }
                .disabled(!vm.demoMode)
                Button("Save demo settings") {
                    Task { await vm.saveDemoMode() }
                }
            }
        }
    }



    private var obdSection: some View {
        Group {
            Section("TPMS") {
                Toggle("BLE link", isOn: $vm.tpmsBle)
                    .onChange(of: vm.tpmsBle) { _ in Task { await vm.saveTpmsBle() } }
                Stepper("Low pressure: \(Format.psi(vm.tpmsLowPsi)) psi", value: $vm.tpmsLowPsi, in: 14.5...58.0, step: 0.5)
                Picker("Stale after", selection: $vm.tpmsStaleAfterMs) {
                    // A custom value saved from the web (e.g. 25 s) isn't in the
                    // preset list — prepend it so the picker shows the real state.
                    ForEach(staleChoices, id: \.self) { ms in
                        Text(staleText(ms)).tag(ms)
                    }
                }
                Button("Save TPMS settings") {
                    Task { await vm.saveTPMSConfig() }
                }
            }
            Section("OBD2 link") {
                HStack(spacing: 6) {
                    Circle()
                        .fill(obdPillColor)
                        .frame(width: 8, height: 8)
                    Text(obdPillText)
                        .foregroundColor(obdPillColor)
                }
                if let lastError = vm.obdState?.lastError, lastError != 0 {
                    HStack {
                        Text("Last error").foregroundColor(.secondary)
                        Spacer()
                        Text(String(format: "0x%04X", lastError))
                    }
                }
                HStack {
                    Text("Peer")
                        .foregroundColor(.secondary)
                    Spacer()
                    Text(vm.obdPeerName ?? "—")
                }
                if let addr = vm.obdPeerAddr, !addr.isEmpty {
                    HStack {
                        Text("Address")
                            .foregroundColor(.secondary)
                        Spacer()
                        Text(addr)
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
                            .foregroundColor(.secondary)
                    }
                }
            }
            Section {
            } footer: {
                Text("The BLE link connects the gauge to the ELM327 adapter in the car's OBD port for live tire pressures.")
            }
        }
    }

    private var wifiSection: some View {
        Group {
            if vm.networkStatus == nil && vm.isLoading {
                Section { ProgressView("Loading Wi-Fi…") }
            } else {
                Section("Status") {
                    if let net = vm.networkStatus {
                        HStack { Text("Mode").foregroundColor(.secondary); Spacer(); Text(net.mode ?? "—").foregroundColor(.secondary) }
                        HStack { Text("STA").foregroundColor(.secondary); Spacer(); Text(net.staConnected == true ? "Connected" : "Not connected").foregroundColor(net.staConnected == true ? .green : .secondary) }
                        if let ssid = net.staSsid, !ssid.isEmpty { HStack { Text("SSID").foregroundColor(.secondary); Spacer(); Text(ssid) } }
                        if let ip = net.staIp, !ip.isEmpty { HStack { Text("IP").foregroundColor(.secondary); Spacer(); Text(ip).foregroundColor(.secondary) } }
                        if let rssi = net.rssi, rssi != 0 { HStack { Text("RSSI").foregroundColor(.secondary); Spacer(); Text("\(rssi) dBm").foregroundColor(.secondary) } }
                        HStack { Text("AP").foregroundColor(.secondary); Spacer(); Text(net.apSsid ?? "—") }
                    } else {
                        Text("Wi-Fi status unavailable").font(.footnote).foregroundColor(.secondary)
                    }
                    Button("Refresh status") { Task { await vm.refreshWifiStatus() } }
                        .accessibilityIdentifier("wifi.refreshStatus")
                    Button("Reconnect") { Task { await vm.reconnectWifi() } }.disabled(vm.networkStatus?.staEnabled != true)
                }
                Section("Saved networks") {
                    if let saved = vm.networkStatus?.saved, !saved.isEmpty {
                        ForEach(saved, id: \.ssid) { item in
                            HStack {
                                Text(item.ssid)
                                Spacer()
                                Button("Delete", role: .destructive) { Task { await vm.deleteSavedWifi(ssid: item.ssid) } }
                            }
                        }
                    } else {
                        Text("No saved networks").foregroundColor(.secondary)
                    }
                }
                Section {
                    Button(action: { Task { await vm.scanWifi() } }) {
                        HStack {
                            if vm.isScanningWifi { ProgressView(); Text("Scanning…") }
                            else { Label("Scan networks", systemImage: "wifi") }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .contentShape(Rectangle())
                    }
                    .disabled(vm.isScanningWifi)
                    Button(action: { Task { await vm.usePhoneWifi() } }) {
                        Label("Use this iPhone's network", systemImage: "iphone.and.arrow.forward")
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentShape(Rectangle())
                    }
                    .accessibilityIdentifier("wifi.usePhoneWifi")
                    ForEach(vm.wifiNetworks) { net in
                        Button(action: { joinSheet = WifiJoinTarget(ssid: net.ssid) }) {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(net.ssid).foregroundColor(.primary)
                                    if let rssi = net.rssi { Text("\(rssi) dBm").font(.caption).foregroundColor(.secondary) }
                                }
                                Spacer()
                            }
                        }
                    }
                } header: {
                    Text("Networks")
                } footer: {
                    Text("Tap a network to enter its password. \"Use this iPhone's network\" sends the Wi-Fi this phone is on — the gauge joins it itself; no password needed.")
                }
                Section("Manual") {
                    HStack {
                        Text("SSID").foregroundColor(.secondary)
                        Spacer()
                        TextField("MyWifi", text: $vm.wifiSSID)
                            .multilineTextAlignment(.trailing)
                            .textInputAutocapitalization(.never)
                            .autocorrectionDisabled()
                    }
                    HStack {
                        Text("Password").foregroundColor(.secondary)
                        Spacer()
                        SecureField("password", text: $vm.wifiPassword)
                            .multilineTextAlignment(.trailing)
                            .frame(width: 160)
                    }
                    Button("Join") { Task { await vm.saveWifi() } }
                        .disabled(vm.isSavingWifi || vm.wifiSSID.trimmingCharacters(in: .whitespaces).isEmpty)
                }
            }
        }
    }

    private var aboutSection: some View {
        Group {
            Section("App") {
                HStack {
                    Text("Version")
                        .foregroundColor(.secondary)
                    Spacer()
                    Text(appVersion)
                        .foregroundColor(.secondary)
                        .monospacedDigit()
                }
                .accessibilityElement(children: .combine)
            }
            Section("Gauge") {
                HStack {
                    Text("Firmware")
                        .foregroundColor(.secondary)
                    Spacer()
                    Text(gaugeFirmware)
                        .foregroundColor(.secondary)
                        .monospacedDigit()
                }
                .accessibilityElement(children: .combine)
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
        Section {
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

    private func staleText(_ ms: Int) -> String {
        ms >= 60000 ? "\(ms / 60000) min" : "\(ms / 1000) s"
    }

    private var staleChoices: [Int] {
        let presets = [5000, 10000, 15000, 30000, 60000, 120000]
        if let current = vm.tpmsConfig?.staleAfterMs, !presets.contains(current) {
            return presets + [current]
        }
        return presets
    }

    /// Scan results that aren't already the saved gauge — prevents the same
    /// hardware showing twice (once as Saved gauge, once as a scan hit).
    private var filteredBleDevices: [BleDevice] {
        guard let savedID = session.lastPeerID(), let uuid = UUID(uuidString: savedID) else {
            return bleDevices
        }
        return bleDevices.filter { $0.identifier != uuid }
    }

    private var savedGaugeName: String {
        session.blePeerName ?? "BoostGauge"
    }

    private var isReconnecting: Bool {
        session.connectionState == .connecting && session.reconnectAttempt != nil
    }

    private var appVersion: String {
        let v = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "—"
        let b = Bundle.main.infoDictionary?["CFBundleVersion"] as? String ?? ""
        return b.isEmpty ? v : "\(v) (\(b))"
    }


    private var gaugeFirmware: String {
        if session.connectionState == .connected, let fw = session.bleInfo?.firmware, !fw.isEmpty {
            return fw
        }
        return "Not connected"
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
                Button("Forget", role: .destructive) {
                    Task { await session.forgetSavedGauge() }
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


struct WifiJoinTarget: Identifiable {
    let ssid: String
    var id: String { ssid }
}

struct WifiJoinView: View {
    let ssid: String
    let onJoin: (String, String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var password = ""

    var body: some View {
        Form {
            Section {
                HStack { Text("SSID").foregroundColor(.secondary); Spacer(); Text(ssid) }
            }
            Section {
                SecureField("Password", text: $password)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .submitLabel(.done)
            } footer: {
                Text("The gauge saves this network and joins it. The SoftAP stays available as fallback.")
            }
            Section {
                Button("Join") { onJoin(ssid, password); dismiss() }
                    .disabled(false)
            }
        }
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") { dismiss() }
            }
        }
    }
}


/// Non-shifting save/error indicator: floats over content without inserting
/// list rows, so taps never move between touch-down and touch-up (the cause
/// of the "dead button" reports). Auto-clears after 2 s.
