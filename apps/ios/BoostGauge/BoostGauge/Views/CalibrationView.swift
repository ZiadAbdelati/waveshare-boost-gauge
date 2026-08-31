import SwiftUI

struct CalibrationView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = CalibrationViewModel()
    @State private var supplyVoltsField = 5.0
    @FocusState private var supplyFieldFocused: Bool

    var body: some View {
        NavigationStack {
            Form {
                if let live = vm.calibration?.live {
                    Section("Live sensors") {
                        presenceRow("ADS1115", present: live.adsPresent)
                        presenceRow("BMP280", present: live.bmpPresent)
                        if live.fault == true {
                            Label("Sensor fault", systemImage: "exclamationmark.triangle")
                                .foregroundColor(.red)
                        }
                        if let value = live.mapVolts {
                            row("MAP volts", Format.volts(value))
                        }
                        if let value = live.nominalKpa {
                            row("Nominal kPa", Format.kpa(value))
                        }
                        if let value = live.correctedKpa {
                            row("Corrected kPa", Format.kpa(value))
                        }
                        if let value = live.bmpKpa {
                            row("BMP kPa", Format.kpa(value))
                        }
                        if let value = live.mapAgeMs {
                            row("MAP age", ageText(value))
                        }
                        if let value = live.bmpAgeMs {
                            row("BMP age", ageText(value))
                        }
                        if let value = live.bmpUpdates {
                            row("BMP updates", Format.integer(Int(value)))
                        }
                        if live.ambientIsFallback == true {
                            row("Ambient", "fallback")
                                .foregroundColor(.orange)
                        }
                    }
                }
                if let cal = vm.calibration?.calibration {
                    Section("Saved calibration") {
                        if cal.valid == true {
                            if let offset = cal.offsetPsi {
                                row("Offset", "\(Format.psi2(offset)) psi")
                            }
                            if let offset = cal.offsetKpa {
                                row("Offset kPa", Format.kpa(offset))
                            }
                            if let version = cal.version {
                                row("Version", Format.integer(Int(version)))
                            }
                            if let ref = cal.refMapVolts {
                                row("Ref MAP volts", Format.volts(ref))
                            }
                            if let samples = cal.samples {
                                row("Samples", Format.integer(Int(samples)))
                            }
                            if let epoch = cal.epochMs, epoch > 0 {
                                row("Calibrated", Format.date(epoch, offsetMinutes: nil))
                            }
                        } else {
                            Text("Not calibrated")
                                .foregroundColor(.secondary)
                        }
                    }
                }
                if vm.calibration?.calibration?.supplyVolts != nil {
                    Section("MAP supply voltage") {
                        HStack {
                            Text("Supply")
                                .foregroundColor(.secondary)
                            Spacer()
                            TextField("5.0", value: $supplyVoltsField, format: .number)
                                .keyboardType(.decimalPad)
                                .focused($supplyFieldFocused)
                                .multilineTextAlignment(.trailing)
                                .frame(width: 90)
                        }
                        Button("Save supply voltage") {
                            supplyFieldFocused = false
                            Task { await vm.setSupplyVolts(supplyVoltsField) }
                        }
                        .disabled(vm.isSavingSupply)
                        if let fieldError = vm.supplyFieldError {
                            Text(fieldError)
                                .font(.footnote)
                                .foregroundColor(.red)
                        }
                    }
                }
                Section {
                    Button(action: { Task { await vm.calibrate() } }) {
                        if vm.isCalibrating {
                            HStack {
                                ProgressView()
                                Text("Calibrating…")
                            }
                        } else {
                            Label("Calibrate to Atmosphere", systemImage: "target")
                        }
                    }
                    .disabled(vm.isCalibrating)
                    Text("Takes about 2 seconds while the device observes the atmosphere.")
                        .font(.footnote)
                        .foregroundColor(.secondary)
                }
            }
            .gaugeScrollBottomMargin()
                        .navigationTitle("Calibration")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.load() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
                ToolbarItemGroup(placement: .keyboard) {
                    Spacer()
                    Button("Done") { supplyFieldFocused = false }
                }
            }
            .onAppear { vm.reset(transport: session.transport) }
            .onChange(of: session.transportID) { _, _ in
                vm.reset(transport: session.transport)
                Task { await vm.load() }
            }
            .onChange(of: vm.calibration?.calibration?.supplyVolts) { _, newValue in
                if let newValue { supplyVoltsField = newValue }
            }
            .task { await vm.load() }
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        HStack {
            Text(title)
                .foregroundColor(.secondary)
            Spacer()
            Text(value)
                .monospacedDigit()
        }
    }

    private func presenceRow(_ title: String, present: Bool?) -> some View {
        HStack {
            Text(title)
                .foregroundColor(.secondary)
            Spacer()
            Text(present == true ? "present" : "absent")
                .foregroundColor(present == true ? .green : .red)
        }
    }

    private func ageText(_ ageMs: Int64) -> String {
        ageMs < 0 ? "never read" : "\(Format.uptime(UInt64(ageMs)))"
    }
}
