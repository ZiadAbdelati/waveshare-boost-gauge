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

struct SavedNetwork: Decodable, Hashable {
    let ssid: String
}

struct NetworkStatus: Decodable {
    let mode: String?
    let staEnabled: Bool?
    let staConnected: Bool?
    let staSsid: String?
    let staIp: String?
    let apSsid: String?
    let apIp: String?
    let rssi: Int?
    let hasPassword: Bool?
    let saved: [SavedNetwork]?
}

struct WifiNetwork: Decodable, Identifiable, Hashable {
    let ssid: String
    let rssi: Int?
    let auth: Int?
    var id: String { ssid }
}

struct WifiScanResult: Decodable {
    let networks: [WifiNetwork]?
}
