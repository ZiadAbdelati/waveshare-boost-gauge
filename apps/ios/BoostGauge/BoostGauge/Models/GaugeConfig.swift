import Foundation

struct DimSchedule: Decodable {
    let enabled: Bool?
    let startMinutes: Int?
    let endMinutes: Int?
}

struct GaugeConfig: Decodable {
    let brightnessHigh: Int?
    let brightnessLow: Int?
    let dimSchedule: DimSchedule?
    let timezoneOffsetMinutes: Int?
    let timezoneTz: String?
    let activeThemeId: String?
    let psiMin: Double?
    let psiMax: Double?
    let psiOverboost: Double?
    let zeroAngle: Double?
    let appBle: Bool?
}

struct TPMSConfig: Decodable {
    let lowKpa: Double?
    let lowPsi: Double?
    let staleAfterMs: Int?
}
