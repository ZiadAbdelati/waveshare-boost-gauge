import SwiftUI
import UIKit

struct StatusView: View {
    @EnvironmentObject var session: AppSession
    @Environment(\.verticalSizeClass) private var verticalSizeClass
    @StateObject private var vm = StatusViewModel()

    private let wheelLabels = ["FL", "FR", "RL", "RR"]

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    if let state = vm.state {
                        // Landscape uses the extra width as two panes: the gauge
                        // readout on the left, TPMS/sensors/OBD2 on the right.
                        // No letterboxing or stretching — same cards, same style.
                        // Both panes are width-flexible so the split spans the
                        // full landscape width on every device (a content-sized
                        // HStack cramped the layout into the left third on the
                        // Pro Max).
                        if verticalSizeClass == .compact {
                            HStack(alignment: .top, spacing: 16) {
                                gaugeCard(state)
                                    .frame(maxWidth: .infinity)
                                VStack(spacing: 16) {
                                    tpmsMirrorCard(state, title: "TPMS")
                                    sensorCard(state)
                                    obdCard(state)
                                }
                                .frame(maxWidth: .infinity)
                            }
                            .frame(maxWidth: .infinity, alignment: .topLeading)
                        } else {
                            gaugeCard(state)
                            tpmsMirrorCard(state, title: "TPMS")
                            sensorCard(state)
                            obdCard(state)
                        }
                    } else {
                        placeholderCard
                    }
                    // The transport footer already renders link failures as
                    // "Unreachable — …"; only surface errors the footer does
                    // not cover (decode failures, API errors, no-transport).
                    // Also suppress while the auto-reconnect loop is retrying:
                    // the footer's "Reconnecting… (attempt N)" and a
                    // "No active transport — connect in Settings." banner would
                    // contradict each other on one screen (r9 F4).
                    if let error = vm.errorMessage,
                       !session.connectionState.isUnreachable,
                       session.reconnectAttempt == nil {
                        errorBanner(error)
                    }
                    transportFooter
                }
                .frame(maxWidth: .infinity, alignment: .topLeading)
                .padding()
            }
            .frame(maxWidth: .infinity)
            .gaugeScrollBottomMargin()
                        .navigationTitle("Boost Gauge")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.forceRefresh() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                }
            }
            .onAppear {
                session.refreshBLELinkState()
                if session.transport?.transportKind == "HTTP" {
                    vm.reset(transport: session.transport, statusStream: session.statusStream())
                } else {
                    vm.reset(transport: session.transport)
                }
                Task { await vm.forceRefresh() }
            }
            .onDisappear { vm.stop() }
            .onReceive(NotificationCenter.default.publisher(for: UIApplication.willEnterForegroundNotification)) { _ in
                session.refreshBLELinkState()
            }
            .onChange(of: session.transportID) { _, _ in
                configureTransport()
            }
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
                infoCell("Firmware", state.firmwareVersion ?? session.bleInfo?.firmware ?? "—")
                infoCell("Page", state.activePage.map { $0 == 1 ? "TPMS" : "Boost" } ?? "Boost")
            }
        }
        .padding()
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 16).fill(Color(.secondarySystemBackground))
        )
    }

    private func configureTransport() {
        if session.transport?.transportKind == "HTTP" {
            vm.reset(transport: session.transport, statusStream: session.statusStream())
        } else {
            vm.reset(transport: session.transport)
        }
        Task { await vm.forceRefresh() }
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

    private func tpmsMirrorCard(_ state: GaugeState, title: String) -> some View {
        Group {
            if let tpms = state.tpms {
                VStack(alignment: .leading, spacing: 10) {
                    HStack {
                        sectionTitle(title)
                        Spacer()
                        if let lowPsi = tpms.lowPsi {
                            Text("low \(Format.psi(lowPsi))")
                                .font(.caption)
                                .foregroundColor(.secondary)
                        }
                    }
                    LazyVGrid(
                        columns: [GridItem(.flexible(), spacing: 10), GridItem(.flexible(), spacing: 10)],
                        spacing: 10
                    ) {
                        ForEach(Array(tpms.wheels.prefix(4).enumerated()), id: \.offset) { index, wheel in
                            tireCapsule(wheel, label: wheelLabels[index], lowPsi: tpms.lowPsi, status: tpms.status)
                        }
                    }
                    if let status = tpms.status {
                        Text("TPMS status \(status)")
                            .font(.caption)
                            .foregroundColor(.secondary)
                    }
                }
                .card()
            }
        }
    }

    private func tireCapsule(_ wheel: TpmsWheel, label: String, lowPsi: Double?, status: Int?) -> some View {
        // Stale (status==1) retains the last psi in amber; only missing data shows --.-
        let stale = status == 1
        let low = !stale && wheel.valid && (lowPsi.map { wheel.psi <= $0 } ?? false)
        let tint: Color = stale ? Color(hex: "#FFB020") : (!wheel.valid ? .gray : (low ? .orange : .green))
        let value = stale || wheel.valid ? Format.psi(wheel.psi) : "--.-"
        return VStack(spacing: 5) {
            Text(label)
                .font(.caption.weight(.semibold))
                .foregroundColor(.secondary)
            Text(value)
                .font(.title3.weight(.semibold).monospacedDigit())
                .foregroundColor(tint)
            Circle()
                .fill(stale || wheel.valid ? tint : Color.clear)
                .overlay(Circle().stroke(tint.opacity(0.8), lineWidth: 1))
                .frame(width: 8, height: 8)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 10)
        .background(
            RoundedRectangle(cornerRadius: 12).fill(tint.opacity(0.12))
        )
        .overlay(
            RoundedRectangle(cornerRadius: 12).stroke(tint.opacity(0.35), lineWidth: 1)
        )
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(label) \(stale || wheel.valid ? "\(Format.psi(wheel.psi)) psi" : "no data")\(low ? ", low pressure" : "")")
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
                        Text(vm.tpmsBleEnabled == false
                             ? "No OBD2 link. Enable tpmsBle in Settings."
                             : "No OBD2 adapter connected.")
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
            return "Not configured — pair a gauge in Settings"
        case .connecting:
            if session.kind == .ble, let attempt = session.reconnectAttempt {
                return AppSession.reconnectingMessage(attempt: attempt)
            }
            return "Connecting…"
        case .notConnected:
            if session.kind == .ble, let attempt = session.reconnectAttempt {
                return AppSession.reconnectingMessage(attempt: attempt)
            }
            return "Not connected"
        case .unreachable(let error):
            return "Unreachable — \(error)"
        case .connected:
            if vm.transportKind == "BLE" {
                return "Live · BLE notify"
            }
            return "Live · 1 Hz"
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
