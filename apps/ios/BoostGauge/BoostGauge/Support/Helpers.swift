import Foundation
import SwiftUI

enum APIErrorText {
    static func from(_ response: Resp) -> String {
        if let object = try? response.jsonObject(), let error = object["error"] as? String {
            return "Device: \(error)"
        }
        return "HTTP \(response.status)"
    }
}

/// Debug-only invariant: every `@Published` mutation must happen on the main
/// thread. Transport callbacks (BLE queue / URLSession) resume continuations on
/// background executors, so a publish after an `await` without an explicit
/// `MainActor` hop trips this. No-op in Release; compile-time clean.
func assertMainThread(file: StaticString = #fileID, line: UInt = #line) {
    #if DEBUG
    assert(Thread.isMainThread, "Publishing @Published off the main thread at \(file):\(line)")
    #endif
}

enum Format {
    private static let posix = Locale(identifier: "en_US_POSIX")

    static func psi(_ value: Double) -> String {
        String(format: "%.1f", locale: posix, value)
    }

    static func psi2(_ value: Double) -> String {
        String(format: "%.2f", locale: posix, value)
    }

    static func kpa(_ value: Double) -> String {
        String(format: "%.2f", locale: posix, value)
    }

    static func volts(_ value: Double) -> String {
        String(format: "%.4f", locale: posix, value)
    }

    /// Unit-aware pressure readout from a canonical PSI value. `psiDecimals`
    /// preserves the call site's existing PSI precision, so the default unit
    /// renders byte-identically to the pre-unit UI; converted units use the
    /// contract precision (bar 2, kPa 0).
    static func pressure(_ psi: Double, unit: String, psiDecimals: Int) -> String {
        let normalized = PressureUnit.normalized(unit)
        if normalized == PressureUnit.psi {
            return String(format: "%.\(psiDecimals)f", locale: posix, psi)
        }
        return String(format: "%.\(PressureUnit.decimals(normalized))f", locale: posix,
                      PressureUnit.display(fromPsi: psi, unit: normalized))
    }

    static func integer(_ value: Int) -> String {
        String(format: "%d", locale: posix, value)
    }

    static func uptime(_ milliseconds: UInt64) -> String {
        let totalSeconds = Int64(milliseconds / 1000)
        let hours = totalSeconds / 3600
        let minutes = (totalSeconds % 3600) / 60
        let seconds = totalSeconds % 60
        return String(format: "%lldh %02lldm %02llds", locale: posix, hours, minutes, seconds)
    }

    static func date(_ epochMs: Int64, offsetMinutes: Int?) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"
        formatter.locale = posix
        if let offsetMinutes {
            formatter.timeZone = TimeZone(secondsFromGMT: offsetMinutes * 60)
        } else {
            formatter.timeZone = .current
        }
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(epochMs) / 1000))
    }

    static func time(_ epochMs: Int64, offsetMinutes: Int?) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "HH:mm:ss"
        formatter.locale = posix
        if let offsetMinutes {
            formatter.timeZone = TimeZone(secondsFromGMT: offsetMinutes * 60)
        } else {
            formatter.timeZone = .current
        }
        return formatter.string(from: Date(timeIntervalSince1970: TimeInterval(epochMs) / 1000))
    }

    static func currentTimezoneOffsetMinutes() -> Int {
        TimeZone.current.secondsFromGMT() / 60
    }

    static func posixTimezoneString(for offsetMinutes: Int) -> String {
        let posixOffset = -offsetMinutes
        let hours = abs(posixOffset) / 60
        let minutes = abs(posixOffset) % 60
        if minutes == 0 {
            return posixOffset < 0 ? "UTC-\(hours)" : "UTC\(hours)"
        }
        return "UTC-\(hours):\(String(format: "%02d", minutes))"
    }
}

/// Global pressure-display unit (PSI / bar / kPa) from the frozen units
/// contract v1. Canonical wire values are always PSI; conversion happens only
/// at display/input boundaries, never on the wire and never for gauge geometry.
enum PressureUnit {
    static let psi = "psi"
    static let bar = "bar"
    static let kPa = "kPa"

    /// Selectable units in cycle order (psi -> bar -> kPa).
    static let all = [psi, bar, kPa]

    /// Contract decimals: psi 1, bar 2, kPa 0.
    static func decimals(_ unit: String) -> Int {
        switch normalized(unit) {
        case bar: return 2
        case kPa: return 0
        default: return 1
        }
    }

    /// psi -> unit factor (contract §Conversion).
    static func factor(_ unit: String) -> Double {
        switch normalized(unit) {
        case bar: return 0.0689475729
        case kPa: return 6.89475729
        default: return 1
        }
    }

    /// Canonical contract label.
    static func label(_ unit: String) -> String {
        switch normalized(unit) {
        case bar: return "bar"
        case kPa: return "kPa"
        default: return "PSI"
        }
    }

    /// Readout suffix used by the native views. PSI keeps the app's existing
    /// lowercase `psi` so the default output is byte-identical (the contract's
    /// "PSI" is the panel unit mark); bar/kPa use the contract labels.
    static func suffix(_ unit: String) -> String {
        switch normalized(unit) {
        case bar: return "bar"
        case kPa: return "kPa"
        default: return "psi"
        }
    }

    /// Missing-reading placeholder matching the unit precision.
    static func placeholder(_ unit: String) -> String {
        switch decimals(unit) {
        case 2: return "--.--"
        case 0: return "--"
        default: return "--.-"
        }
    }

    /// Clamps an arbitrary payload value to a selectable unit (unknown -> psi).
    static func normalized(_ raw: String?) -> String {
        guard let raw, all.contains(raw) else { return psi }
        return raw
    }

    /// PSI (canonical) -> display-unit value.
    static func display(fromPsi psi: Double, unit: String) -> Double {
        psi * factor(unit)
    }

    /// PSI (canonical) -> display-unit value rounded to the unit's contract
    /// decimals. Input controls (Range fields, TPMS stepper) must show the
    /// same precision the readouts use; the raw conversion carries a long
    /// binary tail (10 psi -> 0.6894757… bar, which an unformatted SwiftUI
    /// TextField would render at full length).
    static func displayRounded(fromPsi psi: Double, unit: String) -> Double {
        let scale = pow(10.0, Double(decimals(unit)))
        return (display(fromPsi: psi, unit: unit) * scale).rounded() / scale
    }

    /// Display-unit value -> PSI (canonical).
    static func psi(fromDisplay value: Double, unit: String) -> Double {
        value / factor(unit)
    }
}

extension Color {
    init(hex: String) {
        var value: UInt64 = 0
        let cleaned = hex.replacingOccurrences(of: "#", with: "")
        Scanner(string: cleaned).scanHexInt64(&value)
        self.init(
            red: Double((value >> 16) & 0xFF) / 255.0,
            green: Double((value >> 8) & 0xFF) / 255.0,
            blue: Double(value & 0xFF) / 255.0
        )
    }
}

extension BoostZone {
    var color: Color {
        switch self {
        case .vacuum: return .cyan
        case .atmo: return .gray
        case .boost: return .green
        case .over: return .red
        case .unknown: return .gray
        }
    }
}

// The iOS 26 floating tab bar rides over scroll content; lists need explicit
// bottom clearance so the last rows stay reachable above it.
extension View {
    func gaugeScrollBottomMargin() -> some View {
        contentMargins(.bottom, 100, for: .scrollContent)
    }
}

extension Date {
    /// Minutes-since-midnight <-> Date for the dim-schedule time pickers.
    init(minutes: Int) {
        self = Calendar.current.startOfDay(for: Date()).addingTimeInterval(TimeInterval(minutes * 60))
    }

    var minutesSinceMidnight: Int {
        let comps = Calendar.current.dateComponents([.hour, .minute], from: self)
        return (comps.hour ?? 0) * 60 + (comps.minute ?? 0)
    }
}
