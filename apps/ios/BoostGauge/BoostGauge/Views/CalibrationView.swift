import SwiftUI

struct CalibrationView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = CalibrationViewModel()

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
                if let message = vm.successMessage {
                    Section {
                        Text(message)
                            .font(.footnote)
                            .foregroundColor(.green)
                    }
                }
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
                            if let supply = cal.supplyVolts {
                                row("Supply", "\(Format.volts(supply)) V")
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
            .navigationTitle("Calibration")
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
