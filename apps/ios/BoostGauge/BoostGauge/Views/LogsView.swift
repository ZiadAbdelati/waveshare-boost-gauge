import SwiftUI

struct LogsView: View {
    @EnvironmentObject var session: AppSession
    @StateObject private var vm = LogsViewModel()

    @State private var showShare = false
    @State private var shareURL: URL?

    var body: some View {
        NavigationView {
            Group {
                if vm.samples.isEmpty && !vm.isLoading {
                    VStack(spacing: 8) {
                        Image(systemName: "tray")
                            .font(.largeTitle)
                            .foregroundColor(.secondary)
                        Text("No log samples yet")
                            .foregroundColor(.secondary)
                        if let error = vm.errorMessage {
                            Text(error)
                                .font(.footnote)
                                .foregroundColor(.orange)
                        }
                        Button("Load logs") {
                            Task { await vm.load() }
                        }
                    }
                } else {
                    List {
                        if let error = vm.errorMessage {
                            Text(error)
                                .font(.footnote)
                                .foregroundColor(.orange)
                        }
                        ForEach(vm.samples) { sample in
                            HStack {
                                Text(Format.uptime(UInt64(sample.uptimeMs)))
                                    .font(.caption.monospaced())
                                    .foregroundColor(.secondary)
                                    .frame(width: 100, alignment: .leading)
                                Text(Format.psi2(sample.psi))
                                    .font(.body.monospacedDigit().weight(.semibold))
                                    .frame(width: 70, alignment: .trailing)
                                Text(sample.zone)
                                    .font(.caption.weight(.semibold))
                                    .padding(.horizontal, 6)
                                    .padding(.vertical, 2)
                                    .background(Capsule().fill(Color(.tertiarySystemFill)))
                                    .foregroundColor(.secondary)
                                    .frame(width: 70)
                                Spacer()
                                if sample.demo {
                                    Text("demo")
                                        .font(.caption2)
                                        .foregroundColor(.secondary)
                                }
                            }
                        }
                    }
                }
            }
            .navigationTitle("Logs")
            .toolbar {
                ToolbarItemGroup(placement: .navigationBarTrailing) {
                    Button(action: { Task { await vm.load() } }) {
                        Image(systemName: "arrow.clockwise")
                    }
                    Button(action: exportCSV) {
                        Image(systemName: "square.and.arrow.up")
                    }
                    .disabled(vm.samples.isEmpty)
                }
            }
            .sheet(isPresented: $showShare) {
                if let shareURL {
                    ActivityView(items: [shareURL])
                }
            }
            .onAppear { vm.reset(transport: session.transport) }
            .task { await vm.load() }
        }
    }

    private func exportCSV() {
        let csv = vm.csvText()
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("boost-gauge-log.csv")
        do {
            try csv.write(to: url, atomically: true, encoding: .utf8)
            shareURL = url
            showShare = true
        } catch {
            vm.errorMessage = error.localizedDescription
        }
    }
}
