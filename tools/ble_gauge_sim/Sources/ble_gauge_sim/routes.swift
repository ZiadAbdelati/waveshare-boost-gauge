import Foundation

/// Single selectable-theme table for the simulator. It mirrors the firmware's
/// authoritative order (`boost_theme.c:s_defaults[]`): Dyno Cell, Vault-Tec,
/// Night City, Big Digit, Neon. The web picker, physical swipes, `/themes`,
/// and `/themes/active` all consume this table.
struct SimTheme {
    let id: String
    let name: String
}

enum GaugeRoutes {
    static let themes: [SimTheme] = [
        SimTheme(id: "dyno-cell", name: "Dyno Cell"),
        SimTheme(id: "vault-tec", name: "Vault-Tec"),
        SimTheme(id: "night-city", name: "Night City"),
        SimTheme(id: "big-digit", name: "Big Digit"),
        SimTheme(id: "neon", name: "Neon"),
    ]

    /// Background-log body magic/version header. The Log characteristic body
    /// is `BGL1\n` followed by one line per sample.
    static let logHeader = "BGL1\n"
    /// Column header for human readers; each data line follows this order:
    /// `t_ms,psi,peak_psi,zone,demo`.
    static let logLineFormat = "t_ms,psi,peak_psi,zone,demo\n"

    static func containsTheme(_ id: String) -> Bool {
        themes.contains { $0.id == id }
    }

    static func themeName(_ id: String) -> String? {
        themes.first { $0.id == id }?.name
    }
}
