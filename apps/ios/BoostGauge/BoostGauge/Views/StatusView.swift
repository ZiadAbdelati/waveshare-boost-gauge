import SwiftUI

struct StatusView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = StatusViewModel()

    private let wheelLabels = ["FL", "FR", "RL", "RR"]

    var body: some View {
        NavigationView {
            ScrollView {
                VStack(spacing: 16) {
                    if let state = vm.state {
                        gaugeCard(state)
                        sensorCard(state)
                        tpmsCard(state)
                        obdCard(state)
                    } else {
                        placeholderCard
                    }
                    if let error = vm.errorMessage {
                        errorBanner(error)
                    }
                    transportFooter
                }
                .padding()
            }
            .navigationTitle("Boost Gauge")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.forceRefresh() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                if session.transport?.transportKind == "HTTP" {
                    vm.reset(transport: session.transport, statusStream: session.statusStream())
                } else {
                    vm.reset(transport: session.transport)
                }
            }
            .onDisappear { vm.stop() }
        }
    }

    private var placeholderCard: some View {
        VStack(spacing: 10) {
            Text("--.-")
                .font(.system(size: 72, weight: .bold, design: .rounded).monospacedDigit())
            Text("psi")
                .font(.title3)
                .foregroundColor(.secondary)
            if vm.isLoading {
                ProgressView()
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 40)
        .background(
            RoundedRectangle(cornerRadius: 16).fill(Color(.secondarySystemBackground))
        )
    }

    private func gaugeCard(_ state: GaugeState) -> some View {
        VStack(spacing: 12) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Text(Format.psi2(state.psi))
                    .font(.system(size: 72, weight: .bold, design: .rounded).monospacedDigit())
                Text("psi")
                    .font(.title3)
                    .foregroundColor(.secondary)
            }
            HStack(spacing: 10) {
                Text(state.zone.displayName)
                    .font(.headline)
                    .padding(.horizontal, 12)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(state.zone.color.opacity(0.18)))
                    .foregroundColor(state.zone.color)
                Text(state.demo ? "DEMO" : "LIVE")
                    .font(.caption.weight(.semibold))
                    .padding(.horizontal, 10)
                    .padding(.vertical, 4)
                    .background(Capsule().fill(Color(.tertiarySystemFill)))
                    .foregroundColor(.secondary)
            }
            HStack(spacing: 6) {
                Image(systemName: "arrow.up.right")
                Text("Peak \(Format.psi(state.peakPsi)) psi")
            }
            .font(.subheadline)
            .foregroundColor(.secondary)
            Divider()
            LazyVGrid(
                columns: [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)],
                spacing: 12
            ) {
                infoCell("Theme", vm.themeName ?? state.activeThemeId ?? "—")
                infoCell("Uptime", state.uptimeMs.map { Format.uptime($0) } ?? "—")
                infoCell("Firmware", state.firmwareVersion ?? "—")
                infoCell("Page", state.activePage.map { $0 == 1 ? "TPMS" : "Boost" } ?? "Boost")
            }
            Picker("Page", selection: Binding(
                get: { vm.displayedPage },
                set: { page in Task { await vm.setPage(page) } }
            )) {
                Text("Boost").tag(0)
                Text("TPMS").tag(1)
            }
            .pickerStyle(.segmented)
        }
        .padding()
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 16).fill(Color(.secondarySystemBackground))
        )
    }

    private func infoCell(_ title: String, _ value: String) -> some View {
        VStack(spacing: 4) {
            Text(value)
                .font(.subheadline.weight(.semibold))
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Text(title)
                .font(.caption)
                .foregroundColor(.secondary)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color(.tertiarySystemFill)))
    }

    private func sensorCard(_ state: GaugeState) -> some View {
        Group {
            if let sensors = state.sensors {
                VStack(alignment: .leading, spacing: 8) {
                    sectionTitle("Sensors")
                    HStack {
                        presenceBadge("ADS", present: sensors.adsPresent)
                        presenceBadge("BMP", present: sensors.bmpPresent)
                        if sensors.fault == true {
                            Text("FAULT")
                                .font(.caption.weight(.bold))
                                .foregroundColor(.red)
                        }
                    }
                    if let volts = sensors.mapVolts {
                        metricRow("MAP", "\(Format.volts(volts)) V")
                    }
                    if let kpa = sensors.mapAbsKpa {
                        metricRow("Abs kPa", Format.kpa(kpa))
                    }
                    if let ambient = sensors.ambientKpa {
                        metricRow("Ambient", Format.kpa(ambient))
                    }
                }
                .card()
            }
        }
    }

    private func presenceBadge(_ label: String, present: Bool?) -> some View {
        HStack(spacing: 4) {
            Circle()
                .fill(present == true ? Color.green : Color.gray)
                .frame(width: 8, height: 8)
            Text(label)
                .font(.caption.weight(.semibold))
                .foregroundColor(.secondary)
        }
    }

    private func tpmsCard(_ state: GaugeState) -> some View {
        Group {
            if let tpms = state.tpms {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        sectionTitle("TPMS")
                        Spacer()
                        if let lowPsi = tpms.lowPsi {
                            Text("low \(Format.psi(lowPsi))")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    LazyVGrid(
                        columns: Array(repeating: GridItem(.flexible()), count: 4),
                        spacing: 8
                    ) {
                        ForEach(Array(tpms.wheels.enumerated()), id: \.offset) { index, wheel in
                            VStack(spacing: 4) {
                                Text(wheelLabels[index < wheelLabels.count ? index : 0])
                                    .font(.caption2)
                                    .foregroundColor(.secondary)
                                Text(Format.psi(wheel.psi))
                                    .font(.headline.monospacedDigit())
                                    .foregroundColor(wheel.valid ? .primary : .red)
                                Circle()
                                    .fill(wheel.valid ? Color.green : Color.gray)
                                    .frame(width: 7, height: 7)
                            }
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 8)
                            .background(RoundedRectangle(cornerRadius: 10).fill(Color(.tertiarySystemFill)))
                        }
                    }
                }
                .card()
            }
        }
    }

    private func obdCard(_ state: GaugeState) -> some View {
        Group {
            if let obd = state.obd {
                VStack(alignment: .leading, spacing: 8) {
                    HStack {
                        sectionTitle("OBD2")
                        Spacer()
                        if obd.valid == true {
                            Text("link")
                                .font(.caption.weight(.semibold))
                                .foregroundColor(.green)
                        }
                    }
                    if obd.valid == true {
                        if let peer = obd.peer, !peer.isEmpty {
                            metricRow("Adapter", peer)
                        }
                        if let rpm = obd.rpm {
                            metricRow("RPM", "\(Format.integer(Int(rpm)))")
                        }
                        if let coolant = obd.coolantC {
                            metricRow("Coolant", "\(Format.psi(coolant)) °C")
                        }
                        if let battery = obd.batteryV {
                            metricRow("Battery", "\(Format.psi(battery)) V")
                        }
                        if let age = obd.ageMs {
                            metricRow("Age", "\(Format.uptime(UInt64(age)))")
                        }
                    } else {
                        Text("No OBD2 link. Enable tpmsBle in Settings.")
                            .font(.subheadline)
                            .foregroundColor(.secondary)
                    }
                }
                .card()
            }
        }
    }

    private func metricRow(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label)
                .foregroundColor(.secondary)
            Spacer()
            Text(value)
                .monospacedDigit()
        }
        .font(.subheadline)
    }

    private func sectionTitle(_ title: String) -> some View {
        Text(title)
            .font(.headline)
    }

    private func errorBanner(_ message: String) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "exclamationmark.triangle.fill")
                .foregroundColor(.orange)
            Text(message)
                .font(.footnote)
                .foregroundColor(.secondary)
            Spacer()
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10).fill(Color.orange.opacity(0.12)))
    }

    private var transportFooter: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(session.connectionState.color)
                .frame(width: 8, height: 8)
            Text(transportLabel)
                .font(.caption.monospaced())
                .foregroundColor(.secondary)
        }
        .padding(.top, 4)
    }

    private var transportLabel: String {
        switch session.connectionState {
        case .notConfigured:
            return "Not configured — set a host in Settings"
        case .connecting:
            return "Connecting…"
        case .notConnected:
            return "Not connected"
        case .unreachable(let error):
            return "Unreachable — \(error)"
        case .connected:
            if vm.transportKind == "BLE" {
                return "Live · BLE notify"
            }
            return "Live · HTTP 1 Hz"
        }
    }
}

private extension View {
    func card() -> some View {
        self.padding()
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                RoundedRectangle(cornerRadius: 16).fill(Color(.secondarySystemBackground))
            )
    }
}
