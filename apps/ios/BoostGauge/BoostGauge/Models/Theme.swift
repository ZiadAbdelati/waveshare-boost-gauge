import Foundation

struct ThemeColors: Codable {
    let face: String?
    let track: String?
    let text: String?
    let muted: String?
    let vacuum: String?
    let boost: String?
    let overboost: String?
    let zero: String?
}

struct Theme: Codable, Identifiable {
    let id: String
    let name: String
    let style: String?
    let colors: ThemeColors?
    let customized: Bool?
}

struct ThemeList: Codable {
    let activeThemeId: String?
    let themes: [Theme]?
    /// Global pressure-display unit ("psi" | "bar" | "kPa"); units contract v1.
    let pressureUnit: String?
    /// Global pressure reference: `false` = relative/gauge (default),
    /// `true` = absolute (display = gauge + ambient reference); reference
    /// contract. Wire values stay gauge PSI; the reference is presentation-only.
    let pressureAbsolute: Bool?
    let bigDigitStaticBg: Bool?
    let bigDigitColorText: Bool?
    let bigDigitStaticColor: String?
    let bigDigitTextColor: String?
    let arcGradient: Bool?
    let hudGradient: Bool?
    let hudTrueBlack: Bool?
    let dynoTrueBlack: Bool?
    let neonMarqueeSpin: Bool?
    let teSync: Bool?
    let regionDBuf: Bool?
    let teScanline: Bool?
    let rotation: Int?
    let demoMode: Bool?
    let demoFastSweep: Bool?
    let tpmsBle: Bool?
    let pixelShift: Bool?
    let pixelShiftSec: Int?
    let vaultFace: String?
    let vaultVignette: Int?
    let vaultNeedleRed: Bool?
    let vaultNeedleTail: Bool?
    let neonLayout: Int?
    let neonFont: Int?
    let neonPreset: Int?
}

extension ThemeList {
    var themeDictionary: [String: Theme] {
        var result: [String: Theme] = [:]
        for theme in themes ?? [] {
            result[theme.id] = theme
        }
        return result
    }
}
