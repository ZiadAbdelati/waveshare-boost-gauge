import Foundation

final class LogsViewModel: ObservableObject {
    @Published var samples: [LogSample] = []
    @Published var isLoading = false
    @Published var errorMessage: String?
    @Published private(set) var anchor: GaugeState?

    private weak var transport: GaugeTransport?

    func reset(transport: GaugeTransport?) {
        guard self.transport !== transport else { return }
        self.transport = transport
        samples = []
        errorMessage = nil
        anchor = nil
    }

    func load(limit: Int = 300) async {
        guard let transport else {
            errorMessage = "No active transport — choose HTTP or BLE in Settings."
            return
        }
        isLoading = true
        errorMessage = nil
        do {
            var loaded: [LogSample]
            do {
                loaded = try await transport.readLogSamples(limit: limit)
            } catch {
                let response = try await transport.get("logs")
                guard response.status == 200 else {
                    await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                    return
                }
                loaded = try JSONDecoder().decode(LogResponse.self, from: response.body).samples
            }
            var newAnchor: GaugeState?
            if let stateResponse = try? await transport.get("state"),
               stateResponse.status == 200,
               let decoded = try? JSONDecoder().decode(GaugeState.self, from: stateResponse.body) {
                newAnchor = decoded
            }
            await MainActor.run {
                self.samples = loaded
                self.anchor = newAnchor
                self.errorMessage = nil
                self.isLoading = false
            }
        } catch {
            await MainActor.run {
                self.errorMessage = error.localizedDescription
                self.isLoading = false
            }
        }
    }

    func csvText() -> String {
        Self.csv(from: samples, anchor: anchor)
    }

    static func csv(from samples: [LogSample], anchor: GaugeState?) -> String {
        var lines = ["timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo"]
        let offset = anchor?.timezoneOffsetMinutes
        for sample in samples {
            var epochMs: Int64 = 0
            if let ts = sample.epochTs {
                epochMs = ts
            } else if let tMs = sample.tMs,
                      let anchor,
                      let anchorEpoch = anchor.epochMs,
                      let anchorUptime = anchor.uptimeMs {
                epochMs = anchorEpoch - Int64(anchorUptime) + tMs
            }
            let timestamp = epochMs > 0 ? Format.date(epochMs, offsetMinutes: offset) : ""
            let offsetText = offset.map { String($0) } ?? ""
            let peak = sample.peakPsi.map { Format.psi2($0) } ?? ""
            lines.append(
                "\(timestamp),\(offsetText),\(epochMs),\(sample.uptimeMs),\(Format.psi2(sample.psi)),\(peak),\(sample.zone),\(sample.demo ? 1 : 0)"
            )
        }
        return lines.joined(separator: "\n") + "\n"
    }
}
