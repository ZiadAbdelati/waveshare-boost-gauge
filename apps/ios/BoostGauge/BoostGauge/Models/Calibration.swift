import Foundation

struct CalibrationLive: Decodable {
    let adsPresent: Bool?
    let bmpPresent: Bool?
    let fault: Bool?
    let mapVolts: Double?
    let mapAgeMs: Int64?
    let nominalKpa: Double?
    let correctedKpa: Double?
    let bmpKpa: Double?
    let bmpAgeMs: Int64?
    let bmpUpdates: UInt32?
    let ambientIsFallback: Bool?
}

struct CalibrationInfo: Decodable {
    let valid: Bool?
    let version: UInt32?
    let offsetKpa: Double?
    let offsetPsi: Double?
    let supplyVolts: Double?
    let refMapVolts: Double?
    let refNominalKpa: Double?
    let refBmpKpa: Double?
    let samples: UInt32?
    let epochMs: Int64?
}

struct Calibration: Decodable {
    let supplyVolts: Double?
    let live: CalibrationLive?
    let calibration: CalibrationInfo?
}
