import Foundation

enum BoostZone: String {
    case vacuum = "VAC"
    case atmo = "ATMO"
    case boost = "BOOST"
    case over = "OVER"
    case unknown = ""

    static func make(_ raw: String?) -> BoostZone {
        BoostZone(rawValue: raw ?? "") ?? .unknown
    }

    var displayName: String {
        switch self {
        case .vacuum: return "Vacuum"
        case .atmo: return "Atmosphere"
        case .boost: return "Boost"
        case .over: return "Overboost"
        case .unknown: return "Unknown"
        }
    }
}

struct SensorSummary: Decodable {
    let adsPresent: Bool?
    let bmpPresent: Bool?
    let fault: Bool?
    let mapVolts: Double?
    let mapAbsKpa: Double?
    let ambientKpa: Double?
}

struct TpmsWheel: Decodable {
    let psi: Double
    let valid: Bool
}

struct TpmsSummary: Decodable {
    let status: Int?
    let lowPsi: Double?
    let wheels: [TpmsWheel]
}

struct OBDSummary: Decodable {
    let state: Int?
    let lastError: UInt32?
    let peer: String?
    let peerAddr: String?
    let uptimeMs: UInt32?
    let ageMs: UInt32?
    let valid: Bool?
    let rpm: Double?
    let coolantC: Double?
    let batteryV: Double?

    enum CodingKeys: String, CodingKey {
        case state, lastError, peer, peerAddr, uptimeMs, ageMs, valid
        case rpm, coolantC, batteryV
    }
}

/// The four pill states of the OBD2 Scanner settings page, derived from the
/// firmware's `boost_obd_ble_state_t` int (0 DOWN, 1 SCANNING, 2 CONNECTING,
/// 3 DISCOVERING, 4 READY, 5 DISCONNECTED).
enum OBDPhase: Equatable {
    case idle
    case scanning
    case connecting(name: String?)
    case connected
}

extension OBDSummary {
    var phase: OBDPhase {
        switch state ?? 0 {
        case 1: return .scanning
        case 2, 3: return .connecting(name: peer)
        case 4: return .connected
        // DISCONNECTED means the firmware reconnect loop is already back at
        // the stored peer (Connecting) or has dropped to a fresh scan.
        case 5: return (peer ?? "").isEmpty ? .scanning : .connecting(name: peer)
        default: return .idle
        }
    }
}

struct GaugeState: Decodable {
    let psi: Double
    let peakPsi: Double
    let zone: BoostZone
    let demo: Bool
    let firmwareVersion: String?
    let uptimeMs: UInt64?
    let epochMs: Int64?
    let timezoneOffsetMinutes: Int?
    let activeThemeId: String?
    let activePage: Int?
    let sensors: SensorSummary?
    let tpms: TpmsSummary?
    let obd: OBDSummary?

    enum CodingKeys: String, CodingKey {
        case psi, peakPsi, zone, demo, firmwareVersion, uptimeMs, epochMs
        case timezoneOffsetMinutes, activeThemeId, activePage
        case sensors, tpms, obd
    }

    init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        psi = try container.decode(Double.self, forKey: .psi)
        peakPsi = try container.decode(Double.self, forKey: .peakPsi)
        zone = BoostZone.make(try container.decode(String.self, forKey: .zone))
        demo = try container.decode(Bool.self, forKey: .demo)
        firmwareVersion = try container.decodeIfPresent(String.self, forKey: .firmwareVersion)
        uptimeMs = try container.decodeIfPresent(UInt64.self, forKey: .uptimeMs)
        epochMs = try container.decodeIfPresent(Int64.self, forKey: .epochMs)
        timezoneOffsetMinutes = try container.decodeIfPresent(Int.self, forKey: .timezoneOffsetMinutes)
        activeThemeId = try container.decodeIfPresent(String.self, forKey: .activeThemeId)
        activePage = try container.decodeIfPresent(Int.self, forKey: .activePage)
        sensors = try container.decodeIfPresent(SensorSummary.self, forKey: .sensors)
        tpms = try container.decodeIfPresent(TpmsSummary.self, forKey: .tpms)
        obd = try container.decodeIfPresent(OBDSummary.self, forKey: .obd)
    }
}
