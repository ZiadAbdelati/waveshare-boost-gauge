import Foundation

struct LogSample: Identifiable, Equatable, Decodable {
    let tMs: Int64?
    let epochTs: Int64?
    let psi: Double
    let peakPsi: Double?
    let zone: String
    let demo: Bool

    var id: String { "\(tMs ?? -1)-\(epochTs ?? -1)-\(psi)" }
    var uptimeMs: Int64 { tMs ?? 0 }
    var epochMs: Int64? { epochTs }
}

struct LogResponse: Decodable {
    let samples: [LogSample]
}

extension LogSample {
    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        tMs = try container.decodeIfPresent(Int64.self, forKey: .tMs)
        epochTs = try container.decodeIfPresent(Int64.self, forKey: .ts)
        psi = try container.decode(Double.self, forKey: .psi)
        peakPsi = try container.decodeIfPresent(Double.self, forKey: .peakPsi)
        zone = try container.decodeIfPresent(String.self, forKey: .zone) ?? "ATMO"
        demo = try container.decodeIfPresent(Bool.self, forKey: .demo) ?? false
    }

    private enum CodingKeys: String, CodingKey {
        case tMs, ts, psi, peakPsi, zone, demo
    }
}
