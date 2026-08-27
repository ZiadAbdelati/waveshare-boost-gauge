import Foundation

/// `@MainActor`: every `@Published` mutation is main-actor-isolated. SwiftUI
/// `.task` and Button-`Task` closures resume on background executors in this
/// SDK, so without this, pre-await publishes trip the "Publishing changes from
/// background threads" runtime warning (r9 F1: observed 91x on-device).
@MainActor
final class SettingsViewModel: ObservableObject {
    @Published var config: GaugeConfig?
    @Published var themeFlags: ThemeList?
    @Published var tpmsConfig: TPMSConfig?
    @Published var isLoading = false
    @Published var errorMessage: String?
    @Published var savedMessage: String?

    @Published var brightnessHigh = 100
    @Published var brightnessLow = 12
    @Published var dimEnabled = false
    @Published var dimStartMinutes = 21 * 60
    @Published var dimEndMinutes = 7 * 60
    @Published var timezoneOffset = 0
    @Published var timezoneTZ = "UTC0"
    @Published var timezoneSelection = SettingsViewModel.customTimezoneID
    @Published var psiMin = -15.0
    @Published var psiMax = 10.0
    @Published var psiOverboost = 8.0
    @Published var zeroAngle = 236.25
    @Published var appBle = false

    @Published var demoMode = false
    @Published var demoFastSweep = false
    @Published var tpmsBle = false
    @Published var rotation = 0
    @Published var regionDBuf = true
    @Published var teSync = false
    @Published var teScanline = false
    @Published var pixelShift = false
    @Published var pixelShiftSec = 90

    @Published var tpmsLowPsi = 32.0
    @Published var tpmsStaleAfterMs = 15_000

    @Published var obdState: OBDSummary?
    @Published var obdPeerName: String?
    @Published var obdPeerAddr: String?
    @Published var isForgettingOBDPeer = false

    private weak var transport: GaugeTransport?
    private var obdPollTask: Task<Void, Never>?

    func reset(transport: GaugeTransport?) {
        assertMainThread()
        guard self.transport !== transport else { return }
        stopOBDPolling()
        self.transport = transport
        config = nil
        themeFlags = nil
        tpmsConfig = nil
        obdState = nil
        obdPeerName = nil
        obdPeerAddr = nil
        isForgettingOBDPeer = false
        errorMessage = nil
        savedMessage = nil
    }

    func loadAll() async {
        guard let transport else { return }
        if let ble = transport as? BleTransport, !ble.isConnected {
            await MainActor.run {
                self.isLoading = false
                self.errorMessage = "Not connected — reconnect in Connection"
            }
            return
        }
        // `.task`/`Task` callers resume on background executors; publish on main.
        await MainActor.run {
            self.isLoading = true
            self.errorMessage = nil
        }
        await loadConfig(transport)
        await loadThemeFlags(transport)
        await loadTPMSConfig(transport)
        await MainActor.run { self.isLoading = false }
    }

    private func loadConfig(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("config")
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = "Config: \(APIErrorText.from(response))" }
                return
            }
            let decoded = try JSONDecoder().decode(GaugeConfig.self, from: response.body)
            await MainActor.run { self.applyConfig(decoded) }
        } catch {
            await MainActor.run { self.errorMessage = "Config: \(error.localizedDescription)" }
        }
    }

    private func loadThemeFlags(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("themes")
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = "Themes: \(APIErrorText.from(response))" }
                return
            }
            let decoded = try JSONDecoder().decode(ThemeList.self, from: response.body)
            await MainActor.run { self.applyThemeFlags(decoded) }
        } catch {
            await MainActor.run { self.errorMessage = "Themes: \(error.localizedDescription)" }
        }
    }

    private func loadTPMSConfig(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("tpms/config")
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = "TPMS: \(APIErrorText.from(response))" }
                return
            }
            let decoded = try JSONDecoder().decode(TPMSConfig.self, from: response.body)
            await MainActor.run { self.applyTPMS(decoded) }
        } catch {
            await MainActor.run { self.errorMessage = "TPMS: \(error.localizedDescription)" }
        }
    }

    func saveConfig() async {
        guard let transport else { return }
        savedMessage = nil
        let body: [String: Any] = [
            "brightnessHigh": brightnessHigh,
            "brightnessLow": brightnessLow,
            "dimSchedule": [
                "enabled": dimEnabled,
                "startMinutes": dimStartMinutes,
                "endMinutes": dimEndMinutes,
            ],
            "timezoneOffsetMinutes": timezoneOffset,
            "timezoneTz": timezoneTZ,
            "psiMin": psiMin,
            "psiMax": psiMax,
            "psiOverboost": psiOverboost,
            "zeroAngle": zeroAngle,
            "appBle": appBle,
        ]
        await save(transport, method: "PUT", path: "config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(GaugeConfig.self, from: data)
            self?.applyConfig(decoded)
        }
    }

    func saveThemeFlags() async {
        guard let transport else { return }
        savedMessage = nil
        // Global/demo/debug flags only. THEME-SPECIFIC settings
        // (vaultNeedleRed, vaultNeedleTail, bigDigitStaticBg) live exclusively
        // in the Themes tab editor (PARITY.md) — this PUT must never clobber them.
        let body: [String: Any] = [
            "demoMode": demoMode,
            "demoFastSweep": demoFastSweep,
            "tpmsBle": tpmsBle,
            "rotation": rotation,
            "regionDBuf": regionDBuf,
            "teSync": teSync,
            "teScanline": teScanline,
            "pixelShift": pixelShift,
            "pixelShiftSec": pixelShiftSec,
        ]
        await save(transport, method: "PUT", path: "themes/config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(ThemeList.self, from: data)
            self?.applyThemeFlags(decoded)
        }
    }

    func saveTPMSConfig() async {
        guard let transport else { return }
        savedMessage = nil
        let body: [String: Any] = [
            "lowPsi": tpmsLowPsi,
            "staleAfterMs": tpmsStaleAfterMs,
        ]
        await save(transport, method: "PUT", path: "tpms/config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(TPMSConfig.self, from: data)
            self?.applyTPMS(decoded)
        }
    }

    /// Live OBD2 Scanner page: poll `/state` at 1 Hz while the page is visible.
    /// Read-only; the gauge owns scanning/reconnecting, the app only reports it.
    func startOBDPolling() {
        guard transport != nil, obdPollTask == nil else { return }
        obdPollTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                await self.refreshOBDState()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    func stopOBDPolling() {
        obdPollTask?.cancel()
        obdPollTask = nil
    }

    func refreshOBDState() async {
        guard let transport else { return }
        do {
            let response = try await transport.get("state")
            guard response.status == 200 else { return }
            let state = try JSONDecoder().decode(GaugeState.self, from: response.body)
            await MainActor.run {
                self.obdState = state.obd
                self.obdPeerName = state.obd?.peer
                self.obdPeerAddr = state.obd?.peerAddr
            }
        } catch {
            // Keep the last known state; the next poll retries.
        }
    }

    /// Clears the stored OBD peer (`obd_peer` NVS on the gauge). The firmware
    /// route is `POST /api/v1/obd/forget` (firmware erases NVS `obd_peer` and
    /// drops any live link).
    func forgetOBDPeer() async {
        guard let transport else { return }
        isForgettingOBDPeer = true
        savedMessage = nil
        do {
            let response = try await transport.send("POST", path: "obd/forget", body: [:])
            await MainActor.run {
                self.isForgettingOBDPeer = false
                if response.status == 200 {
                    self.obdPeerName = nil
                    self.obdPeerAddr = nil
                    self.savedMessage = "OBD peer forgotten"
                } else {
                    self.errorMessage = APIErrorText.from(response)
                }
            }
        } catch {
            await MainActor.run {
                self.isForgettingOBDPeer = false
                self.errorMessage = error.localizedDescription
            }
        }
    }

    func syncTimezone() async {
        guard let transport else { return }
        savedMessage = nil
        let offset = Format.currentTimezoneOffsetMinutes()
        let body: [String: Any] = [
            "timezoneOffsetMinutes": offset,
            "timezoneTz": Format.posixTimezoneString(for: offset),
        ]
        await postTimezone(transport, body: body, success: "Device timezone synced")
    }

    /// Apply a curated (or custom) timezone: set the fields, push the
    /// timezone-only POST, then persist through the normal config save.
    func applyTimezoneOption(_ id: String) async {
        if id != SettingsViewModel.customTimezoneID,
           let option = SettingsViewModel.timezoneOptions.first(where: { $0.id == id }) {
            timezoneOffset = option.offsetMinutes
            timezoneTZ = option.tz
        }
        timezoneSelection = id
        guard let transport else { return }
        let body: [String: Any] = [
            "timezoneOffsetMinutes": timezoneOffset,
            "timezoneTz": timezoneTZ,
        ]
        await postTimezone(transport, body: body, success: "Timezone applied")
        await saveConfig()
    }

    func saveTimezoneCustom() async {
        guard let transport else { return }
        let body: [String: Any] = [
            "timezoneOffsetMinutes": timezoneOffset,
            "timezoneTz": timezoneTZ,
        ]
        await postTimezone(transport, body: body, success: "Timezone saved")
        await saveConfig()
    }

    private func postTimezone(_ transport: GaugeTransport, body: [String: Any], success: String) async {
        do {
            let response = try await transport.send("POST", path: "time", body: body)
            await MainActor.run {
                if response.status == 200 {
                    self.savedMessage = success
                } else {
                    self.errorMessage = APIErrorText.from(response)
                }
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }

    private func save(
        _ transport: GaugeTransport,
        method: String,
        path: String,
        body: [String: Any],
        onSuccess: @MainActor @escaping (Data) throws -> Void
    ) async {
        do {
            let response = try await transport.send(method, path: path, body: body)
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            // The transport continuation resumes off-main (BLE queue / URLSession),
            // so the success closure's `apply*` @Published mutations must run on
            // the main thread or SwiftUI logs "Publishing changes from background
            // threads is not allowed" (observed: 4 named + ~30 generic during
            // theme churn + reconnect torture).
            try await MainActor.run { try onSuccess(response.body) }
            await MainActor.run { self.savedMessage = "Saved" }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }

    private func applyConfig(_ config: GaugeConfig) {
        assertMainThread()
        self.config = config
        if let value = config.brightnessHigh { brightnessHigh = value }
        if let value = config.brightnessLow { brightnessLow = value }
        if let schedule = config.dimSchedule {
            dimEnabled = schedule.enabled ?? false
            dimStartMinutes = schedule.startMinutes ?? dimStartMinutes
            dimEndMinutes = schedule.endMinutes ?? dimEndMinutes
        }
        if let value = config.timezoneOffsetMinutes { timezoneOffset = value }
        if let value = config.timezoneTz { timezoneTZ = value }
        if let value = config.psiMin { psiMin = value }
        if let value = config.psiMax { psiMax = value }
        if let value = config.psiOverboost { psiOverboost = value }
        if let value = config.zeroAngle { zeroAngle = value }
        if let value = config.appBle { appBle = value }
        timezoneSelection = SettingsViewModel.timezoneOptions
            .first { $0.tz == timezoneTZ && $0.offsetMinutes == timezoneOffset }
            .map { $0.id } ?? SettingsViewModel.customTimezoneID
    }

    private func applyThemeFlags(_ flags: ThemeList) {
        assertMainThread()
        themeFlags = flags
        if let value = flags.demoMode { demoMode = value }
        if let value = flags.demoFastSweep { demoFastSweep = value }
        if let value = flags.tpmsBle { tpmsBle = value }
        if let value = flags.rotation { rotation = value }
        if let value = flags.regionDBuf { regionDBuf = value }
        if let value = flags.teSync { teSync = value }
        if let value = flags.teScanline { teScanline = value }
        if let value = flags.pixelShift { pixelShift = value }
        if let value = flags.pixelShiftSec { pixelShiftSec = value }
    }

    private func applyTPMS(_ cfg: TPMSConfig) {
        assertMainThread()
        tpmsConfig = cfg
        if let value = cfg.lowPsi {
            tpmsLowPsi = value
        } else if let kpa = cfg.lowKpa {
            tpmsLowPsi = kpa * 0.145037738
        }
        if let value = cfg.staleAfterMs { tpmsStaleAfterMs = value }
    }
}

struct TimezoneOption: Identifiable, Hashable {
    let id: String
    let label: String
    /// POSIX TZ string applied via `setenv("TZ")` on the device
    /// (e.g. `EST5EDT,M3.2.0/2,M11.1.0/2`). DST transitions are encoded in the
    /// string so the gauge tracks DST automatically.
    let tz: String
    let offsetMinutes: Int
}

extension SettingsViewModel {
    static let customTimezoneID = "custom"

    static let timezoneOptions: [TimezoneOption] = [
        TimezoneOption(id: "utc", label: "UTC", tz: "UTC0", offsetMinutes: 0),
        TimezoneOption(id: "eastern", label: "US Eastern", tz: "EST5EDT,M3.2.0/2,M11.1.0/2", offsetMinutes: -300),
        TimezoneOption(id: "central", label: "US Central", tz: "CST6CDT,M3.2.0/2,M11.1.0/2", offsetMinutes: -360),
        TimezoneOption(id: "mountain", label: "US Mountain", tz: "MST7MDT,M3.2.0/2,M11.1.0/2", offsetMinutes: -420),
        TimezoneOption(id: "pacific", label: "US Pacific", tz: "PST8PDT,M3.2.0/2,M11.1.0/2", offsetMinutes: -480),
        TimezoneOption(id: "alaska", label: "US Alaska", tz: "AKST9AKDT,M3.2.0/2,M11.1.0/2", offsetMinutes: -540),
        TimezoneOption(id: "hawaii", label: "US Hawaii", tz: "HST10", offsetMinutes: -600),
        TimezoneOption(id: "uk", label: "UK", tz: "GMT0BST,M3.5.0/1,M10.5.0/2", offsetMinutes: 0),
        TimezoneOption(id: "cet", label: "Central Europe", tz: "CET-1CEST,M3.5.0/2,M10.5.0/3", offsetMinutes: 60),
        TimezoneOption(id: "india", label: "India", tz: "IST-5:30", offsetMinutes: 330),
        TimezoneOption(id: "japan", label: "Japan", tz: "JST-9", offsetMinutes: 540),
        TimezoneOption(id: "australia-east", label: "Australia East", tz: "AEST-10AEDT,M10.1.0/2,M4.1.0/3", offsetMinutes: 600),
    ]
}
