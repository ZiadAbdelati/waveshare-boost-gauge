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
