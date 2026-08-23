import Foundation

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
    @Published var vaultNeedleRed = false
    @Published var vaultNeedleTail = false
    @Published var bigDigitStaticBg = false

    @Published var tpmsLowPsi = 32.0
    @Published var tpmsStaleAfterMs = 15_000

    private weak var transport: GaugeTransport?

    func reset(transport: GaugeTransport?) {
        guard self.transport !== transport else { return }
        self.transport = transport
        config = nil
        themeFlags = nil
        tpmsConfig = nil
        errorMessage = nil
        savedMessage = nil
    }

    func loadAll() async {
        guard let transport else { return }
        isLoading = true
        errorMessage = nil
        await loadConfig(transport)
        await loadThemeFlags(transport)
        await loadTPMSConfig(transport)
        await MainActor.run { self.isLoading = false }
    }

    private func loadConfig(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("config")
            guard response.status == 200 else { return }
            let decoded = try JSONDecoder().decode(GaugeConfig.self, from: response.body)
            await MainActor.run { self.applyConfig(decoded) }
        } catch {
            await MainActor.run { self.errorMessage = "Config: \(error.localizedDescription)" }
        }
    }

    private func loadThemeFlags(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("themes")
            guard response.status == 200 else { return }
            let decoded = try JSONDecoder().decode(ThemeList.self, from: response.body)
            await MainActor.run { self.applyThemeFlags(decoded) }
        } catch {
            await MainActor.run { self.errorMessage = "Themes: \(error.localizedDescription)" }
        }
    }

    private func loadTPMSConfig(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("tpms/config")
            guard response.status == 200 else { return }
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
            "vaultNeedleRed": vaultNeedleRed,
            "vaultNeedleTail": vaultNeedleTail,
            "bigDigitStaticBg": bigDigitStaticBg,
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

    func syncTime() async {
        guard let transport else { return }
        savedMessage = nil
        let offset = Format.currentTimezoneOffsetMinutes()
        let body: [String: Any] = [
            "epochMs": Date().timeIntervalSince1970 * 1000,
            "timezoneOffsetMinutes": offset,
            "timezoneTz": Format.posixTimezoneString(for: offset),
        ]
        do {
            let response = try await transport.send("POST", path: "time", body: body)
            await MainActor.run {
                if response.status == 200 {
                    self.savedMessage = "Device clock synced"
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
        onSuccess: @escaping (Data) throws -> Void
    ) async {
        do {
            let response = try await transport.send(method, path: path, body: body)
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            try onSuccess(response.body)
            await MainActor.run { self.savedMessage = "Saved" }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }

    private func applyConfig(_ config: GaugeConfig) {
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
    }

    private func applyThemeFlags(_ flags: ThemeList) {
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
        if let value = flags.vaultNeedleRed { vaultNeedleRed = value }
        if let value = flags.vaultNeedleTail { vaultNeedleTail = value }
        if let value = flags.bigDigitStaticBg { bigDigitStaticBg = value }
    }

    private func applyTPMS(_ cfg: TPMSConfig) {
        tpmsConfig = cfg
        if let value = cfg.lowPsi {
            tpmsLowPsi = value
        } else if let kpa = cfg.lowKpa {
            tpmsLowPsi = kpa * 0.145037738
        }
        if let value = cfg.staleAfterMs { tpmsStaleAfterMs = value }
    }
}
