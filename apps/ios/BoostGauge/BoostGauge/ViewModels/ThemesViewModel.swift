import Foundation

/// `@MainActor`: every `@Published` mutation is main-actor-isolated (r9 F1).
@MainActor
final class ThemesViewModel: ObservableObject {
    @Published var themes: [Theme] = []
    @Published var activeThemeID: String?
    @Published var configuration: ThemeList?
    @Published private(set) var gaugeConfiguration: [String: Any] = [:]
    @Published var isLoading = false
    @Published var errorMessage: String?

    @Published var arcGradient = false
    @Published var hudGradient = false
    @Published var hudTrueBlack = false
    @Published var bigDigitStaticBg = false
    @Published var bigDigitColorText = false
    @Published var bigDigitStaticColor = "#000000"
    @Published var bigDigitTextColor = "#ffffff"
    @Published var vaultFace = "#05281a"
    @Published var vaultVignette = 60
    @Published var vaultNeedleRed = false
    @Published var vaultNeedleTail = false
    @Published var neonLayout = 1
    @Published var neonFont = 0
    @Published var neonPreset = 0
    @Published var neonMarqueeSpin = false
    @Published private(set) var themeColorEdits: [String: [String: String]] = [:]

    private weak var transport: GaugeTransport?
    /// Monotonic activation sequence: bumped on every `select` request so a
    /// slow, older PUT echo can never clobber `activeThemeID` with a theme the
    /// user has already tapped away from (rapid taps over a serialized BLE
    /// link return out of order).
    private var activationSeq = 0
    private var newestRequestSeq = 0

    func reset(transport: GaugeTransport?) {
        assertMainThread()
        guard self.transport !== transport else { return }
        self.transport = transport
        themes = []
        activeThemeID = nil
        configuration = nil
        errorMessage = nil
        themeColorEdits = [:]
    }

    /// Tab re-entry / reconnect: the board is authoritative. If its
    /// activeThemeId moved (other phone, web UI), adopt list + id so the tab
    /// never shows a stale active theme. Lightweight: one GET.
    func resyncActiveTheme() async {
        guard let transport, !isLoading else { return }
        // Prime from the persisted snapshot first: zero-latency correct state,
        // then let the fetch correct it if the board moved again.
        if let cached = Self.cachedThemeList() {
            apply(cached)
        }
        do {
            let response = try await transport.get("themes")
            guard response.status == 200 else { return }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            Self.cacheThemeList(list)
            await MainActor.run { self.apply(list) }
        } catch {
            // A failed resync leaves the (possibly stale) cache applied —
            // surface it; H1 proved the silent catch hid a frozen preview
            // across restarts.
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }

    // Last confirmed theme list, so tab re-entry renders instantly instead of
    // waiting a BLE round-trip for the fetch.
    private static let themeCacheKey = "themes.cachedPayload"
    private static func cacheThemeList(_ list: ThemeList) {
        guard let data = try? JSONEncoder().encode(list) else { return }
        UserDefaults.standard.set(data, forKey: themeCacheKey)
    }
    private static func cachedThemeList() -> ThemeList? {
        guard let data = UserDefaults.standard.data(forKey: themeCacheKey) else { return nil }
        return try? JSONDecoder().decode(ThemeList.self, from: data)
    }

    func load() async {
        // SwiftUI runs `.task { await vm.load() }` on a background executor, so
        // the pre-await publishes must hop to the main thread themselves.
        guard let transport else {
            await MainActor.run { self.errorMessage = "No active transport — connect in Settings." }
            return
        }
        await MainActor.run {
            self.isLoading = true
            self.errorMessage = nil
        }
        do {
            // Parallelize the two independent GETs: the HTTP transport serves
            // them concurrently; the BLE transport serializes them in its own
            // queue either way, so this never regresses the link.
            async let themesResp = transport.get("themes")
            async let configResp = transport.get("config")
            let response = try await themesResp
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            let gaugeConfig = (try? await configResp).flatMap { try? $0.jsonObject() } ?? [:]
            await MainActor.run {
                self.apply(list)
                self.gaugeConfiguration = gaugeConfig
                self.errorMessage = nil
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
        await MainActor.run { self.isLoading = false }
    }

    func previewPayload(for theme: Theme) -> [String: Any] {
        var colors: [String: Any] = [:]
        for key in Self.paletteKeys {
            if let value = colorHex(for: theme, key: key) { colors[key] = value }
        }
        let themeObject: [String: Any] = [
            "id": theme.id,
            "name": theme.name,
            "style": theme.style ?? "arc",
            "colors": colors,
            "customized": theme.customized ?? false,
        ]
        return [
            "theme": themeObject,
            "config": gaugeConfiguration,
            "settings": [
                "arcGradient": arcGradient,
                "hudGradient": hudGradient,
                "hudTrueBlack": hudTrueBlack,
                "bigDigitStaticBg": bigDigitStaticBg,
                "bigDigitColorText": bigDigitColorText,
                "bigDigitStaticColor": bigDigitStaticColor,
                "bigDigitTextColor": bigDigitTextColor,
                "vaultFace": vaultFace,
                "vaultVignette": vaultVignette,
                "vaultNeedleRed": vaultNeedleRed,
                "vaultNeedleTail": vaultNeedleTail,
                "neonLayout": neonLayout,
                "neonFont": neonFont,
                "neonPreset": neonPreset,
                "neonMarqueeSpin": neonMarqueeSpin,
            ],
        ]
    }

    func select(_ id: String) async {
        activationSeq += 1
        newestRequestSeq = activationSeq
        await put("themes/active", body: ["id": id], activationSeq: activationSeq)
    }

    func saveOptions(for themeID: String) async {
        var body: [String: Any]
        switch themeID {
        case "dyno-cell":
            body = ["arcGradient": arcGradient]
        case "vault-tec":
            body = [
                "vaultFace": vaultFace,
                "vaultVignette": vaultVignette,
                "vaultNeedleRed": vaultNeedleRed,
                "vaultNeedleTail": vaultNeedleTail,
            ]
        case "night-city":
            body = ["hudGradient": hudGradient, "hudTrueBlack": hudTrueBlack]
        case "big-digit":
            body = [
                "bigDigitStaticBg": bigDigitStaticBg,
                "bigDigitColorText": bigDigitColorText,
                "bigDigitStaticColor": bigDigitStaticColor,
                "bigDigitTextColor": bigDigitTextColor,
            ]
        case "neon":
            body = [
                "neonLayout": neonLayout,
                "neonFont": neonFont,
                "neonPreset": neonPreset,
                "neonMarqueeSpin": neonMarqueeSpin,
            ]
        default:
            return
        }
        let editable = themeColorEdits[themeID] ?? [:]
        body["id"] = themeID
        body["colors"] = [
            "vacuum": editable["vacuum"] ?? "#000000",
            "boost": editable["boost"] ?? "#000000",
            "overboost": editable["overboost"] ?? "#000000",
        ]
        await put("themes/config", body: body)
    }

    func resetColors(for themeID: String) async {
        await put("themes/config", body: ["id": themeID, "reset": true])
    }

    /// Shared PUT skeleton for the theme mutations: PUT, require 200, decode
    /// the echoed ThemeList, and `apply` it on the main actor. Any failure —
    /// missing transport, non-200 status, or decode/transport error — surfaces
    /// via `errorMessage`. For activations (seq != nil) a lost echo is
    /// ambiguous: the board may have applied the switch even though the
    /// response vanished (H2), so reconcile with a GET before giving up.
    private func put(_ path: String, body: [String: Any], activationSeq seq: Int? = nil) async {
        guard let transport else { return }
        do {
            let response = try await transport.send("PUT", path: path, body: body)
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                if seq != nil { await reconcileAfterLostEcho(requested: body["id"] as? String) }
                return
            }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            await MainActor.run {
                self.apply(list, activationSeq: seq)
                self.errorMessage = nil
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
            if seq != nil { await reconcileAfterLostEcho(requested: body["id"] as? String) }
        }
    }

    /// Lost-echo recovery: the board may have applied the switch even though
    /// the response was lost. GET the board's real state and adopt it; clear
    /// the error only if the board actually took the requested theme
    /// (switch succeeded despite the lost echo) — otherwise the failure
    /// message stands while the UI still shows the board's authoritative
    /// state, matching the Android reconcile contract.
    private func reconcileAfterLostEcho(requested: String?) async {
        guard let transport else { return }
        do {
            let response = try await transport.get("themes")
            guard response.status == 200 else { return }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            Self.cacheThemeList(list)
            await MainActor.run {
                self.apply(list, activationSeq: newestRequestSeq)
                if let requested, list.activeThemeId == requested {
                    self.errorMessage = nil
                }
            }
        } catch { /* reconcile best-effort; the error message stands */ }
    }

    func colorHex(for theme: Theme, key: String) -> String? {
        if let edited = themeColorEdits[theme.id]?[key] { return edited }
        switch key {
        case "face": return theme.colors?.face
        case "track": return theme.colors?.track
        case "text": return theme.colors?.text
        case "muted": return theme.colors?.muted
        case "vacuum": return theme.colors?.vacuum
        case "boost": return theme.colors?.boost
        case "overboost": return theme.colors?.overboost
        case "zero": return theme.colors?.zero
        default: return nil
        }
    }

    func setColor(_ hex: String, for themeID: String, key: String) {
        var colors = themeColorEdits[themeID] ?? [:]
        colors[key] = hex
        themeColorEdits[themeID] = colors
    }

    static let paletteKeys = ["face", "track", "text", "muted", "vacuum", "boost", "overboost", "zero"]
    static let zoneKeys = ["vacuum", "boost", "overboost"]

    private func apply(_ list: ThemeList, activationSeq seq: Int? = nil) {
        assertMainThread()
        configuration = list
        themes = list.themes ?? themes
        if let seq {
            // Only the newest activation request may set `activeThemeID`: a
            // slow older PUT echo (stale `seq`) must not re-apply a theme the
            // user already tapped away from.
            if seq >= newestRequestSeq {
                activeThemeID = list.activeThemeId ?? activeThemeID
            }
        } else if activationSeq == newestRequestSeq {
            // No-seq applies (load/resync) follow the same rule: once the user
            // has tapped a selection, a slow in-flight list response must not
            // clobber it (H4).
            activeThemeID = list.activeThemeId ?? activeThemeID
        }
        if let value = list.arcGradient { arcGradient = value }
        if let value = list.hudGradient { hudGradient = value }
        if let value = list.hudTrueBlack { hudTrueBlack = value }
        if let value = list.bigDigitStaticBg { bigDigitStaticBg = value }
        if let value = list.bigDigitColorText { bigDigitColorText = value }
        if let value = list.bigDigitStaticColor { bigDigitStaticColor = value }
        if let value = list.bigDigitTextColor { bigDigitTextColor = value }
        if let value = list.vaultFace { vaultFace = value }
        if let value = list.vaultVignette { vaultVignette = value }
        if let value = list.vaultNeedleRed { vaultNeedleRed = value }
        if let value = list.vaultNeedleTail { vaultNeedleTail = value }
        if let value = list.neonLayout { neonLayout = value }
        if let value = list.neonFont { neonFont = value }
        if let value = list.neonPreset { neonPreset = value }
        if let value = list.neonMarqueeSpin { neonMarqueeSpin = value }
        for theme in list.themes ?? [] {
            var colors: [String: String] = [:]
            for key in Self.paletteKeys {
                if let value = colorHexFromPayload(theme, key: key) { colors[key] = value }
            }
            themeColorEdits[theme.id] = colors
        }
    }

    private func colorHexFromPayload(_ theme: Theme, key: String) -> String? {
        switch key {
        case "face": return theme.colors?.face
        case "track": return theme.colors?.track
        case "text": return theme.colors?.text
        case "muted": return theme.colors?.muted
        case "vacuum": return theme.colors?.vacuum
        case "boost": return theme.colors?.boost
        case "overboost": return theme.colors?.overboost
        case "zero": return theme.colors?.zero
        default: return nil
        }
    }
}
