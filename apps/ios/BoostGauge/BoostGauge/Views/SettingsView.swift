import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = SettingsViewModel()

    @State private var kindDraft: AppSession.Kind = .http
    @State private var hostDraft = ""
    @State private var bleDevices: [BleDevice] = []
    @State private var isScanning = false
    @State private var isConnecting = false

    var body: some View {
        NavigationView {
            Form {
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
                transportSection
                gaugeSection
                themeFlagsSection
                tpmsSection
                clockSection
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.loadAll() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                kindDraft = session.kind
                hostDraft = session.httpHost ?? ""
                vm.reset(transport: session.transport)
            }
            .onChange(of: session.transportID) { _ in
                Task { await vm.loadAll() }
            }
            .task { await vm.loadAll() }
        }
    }

    private var transportSection: some View {
        Section("Transport") {
            Picker("Transport", selection: Binding(
                get: { kindDraft },
                set: { newValue in
                    kindDraft = newValue
                    if newValue == .http && session.kind == .ble {
                        session.backToHTTP()
                    }
                }
            )) {
                Text("HTTP").tag(AppSession.Kind.http)
                Text("BLE").tag(AppSession.Kind.ble)
            }
            .pickerStyle(.segmented)

            if kindDraft == .http {
                TextField("http://192.168.1.100", text: $hostDraft)
                    .keyboardType(.URL)
                    .autocapitalization(.none)
                    .disableAutocorrection(true)
                Button("Connect") {
                    if !session.setHTTPHost(hostDraft) {
                        vm.savedMessage = nil
                        vm.errorMessage = "Enter a valid URL such as http://192.168.1.100"
                    } else {
                        vm.errorMessage = nil
                        vm.savedMessage = nil
                    }
                }
                if session.kind == .http {
                    GaugeConnectionBadge(state: session.connectionState, target: session.httpHost)
                }
            } else {
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
                if bleDevices.isEmpty && !isScanning && session.transport == nil {
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
                if let info = session.bleInfo {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(info.name)
                            .font(.headline)
                        Text("firmware \(info.firmware) · api \(info.api)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                if session.kind == .ble, session.transport != nil {
                    Label("Connected to \(session.blePeerName ?? "gauge")", systemImage: "checkmark.circle.fill")
                        .foregroundColor(.green)
                }
                if session.kind == .ble, session.transport != nil {
                    Button("Disconnect", role: .destructive) {
                        session.disconnectBLE()
                    }
                }
            }
        }
    }

    private var gaugeSection: some View {
        Section("Gauge") {
            Stepper("Brightness high: \(vm.brightnessHigh)%", value: $vm.brightnessHigh, in: 1...100)
            Stepper("Brightness low: \(vm.brightnessLow)%", value: $vm.brightnessLow, in: 1...100)
            Toggle("Dim schedule", isOn: $vm.dimEnabled)
            if vm.dimEnabled {
                Stepper("Start: \(minutesText(vm.dimStartMinutes))", value: $vm.dimStartMinutes, in: 0...(24 * 60 - 1))
                Stepper("End: \(minutesText(vm.dimEndMinutes))", value: $vm.dimEndMinutes, in: 0...(24 * 60 - 1))
            }
            Stepper("Timezone offset: \(vm.timezoneOffset) min", value: $vm.timezoneOffset, in: -720...840, step: 15)
            HStack {
                Text("TZ string")
                    .foregroundColor(.secondary)
                TextField("UTC0", text: $vm.timezoneTZ)
                    .autocapitalization(.none)
                    .disableAutocorrection(true)
                    .multilineTextAlignment(.trailing)
            }
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
            Toggle("Companion BLE advertising", isOn: $vm.appBle)
            Button("Save gauge settings") {
                Task { await vm.saveConfig() }
            }
        }
    }

    private var themeFlagsSection: some View {
        Section("Theme & demo") {
            Toggle("Demo mode", isOn: $vm.demoMode)
            Toggle("Demo fast sweep", isOn: $vm.demoFastSweep)
            Toggle("TPMS BLE link", isOn: $vm.tpmsBle)
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
            Toggle("Vault needle red", isOn: $vm.vaultNeedleRed)
            Toggle("Vault needle tail", isOn: $vm.vaultNeedleTail)
            Toggle("Big digit static bg", isOn: $vm.bigDigitStaticBg)
            Button("Save theme settings") {
                Task { await vm.saveThemeFlags() }
            }
        }
    }

    private var tpmsSection: some View {
        Section("TPMS") {
            Stepper("Low pressure: \(Format.psi(vm.tpmsLowPsi)) psi", value: $vm.tpmsLowPsi, in: 14.5...58.0, step: 0.5)
            Picker("Stale after", selection: $vm.tpmsStaleAfterMs) {
                ForEach([5000, 10000, 15000, 30000, 60000, 120000], id: \.self) { ms in
                    Text(staleText(ms)).tag(ms)
                }
            }
            Button("Save TPMS settings") {
                Task { await vm.saveTPMSConfig() }
            }
        }
    }

    private var clockSection: some View {
        Section("Clock") {
            Button("Sync device clock to phone") {
                Task { await vm.syncTime() }
            }
            Text("Sends the phone epoch and timezone to the device.")
                .font(.footnote)
                .foregroundColor(.secondary)
        }
    }

    private func scan() async {
        isScanning = true
        bleDevices = []
        vm.errorMessage = nil
        do {
            bleDevices = try await session.scanBLE()
        } catch {
            vm.errorMessage = error.localizedDescription
        }
        isScanning = false
    }

    private func connect(_ device: BleDevice) async {
        isConnecting = true
        vm.errorMessage = nil
        do {
            _ = try await session.connectBLE(to: device)
            vm.savedMessage = "Connected over BLE"
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
}
