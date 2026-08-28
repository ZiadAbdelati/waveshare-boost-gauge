import Foundation
import CoreLocation
import NetworkExtension
import SystemConfiguration.CaptiveNetwork

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
    @Published var errorMessage: String? {
        didSet {
            if let errorMessage {
                WindowToast.show(errorMessage, color: .systemOrange)
            }
        }
    }
    @Published var savedMessage: String? {
        didSet {
            if let savedMessage {
                WindowToast.show(savedMessage, color: .systemGreen)
            }
        }
    }

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

    // Wi-Fi pairing (via BLE Control, no SoftAP join needed)
    @Published var networkStatus: NetworkStatus?
    @Published var wifiNetworks: [WifiNetwork] = []
    @Published var wifiSSID = ""
    @Published var wifiPassword = ""
    @Published var isScanningWifi = false
    @Published var isSavingWifi = false

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
        await loadNetworkStatus(transport)
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

    private func loadNetworkStatus(_ transport: GaugeTransport) async {
        do {
            let response = try await transport.get("network")
            guard response.status == 200 else {
                await MainActor.run { errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(NetworkStatus.self, from: response.body)
            await MainActor.run { self.networkStatus = decoded }
        } catch {
            // Silent failure here is why "Refresh status" looked dead: the
            // request failed and the UI showed nothing. Surface it.
            await MainActor.run { errorMessage = error.localizedDescription }
        }
    }

    func refreshWifiStatus() async {
        guard let transport else {
            await MainActor.run { errorMessage = "No gauge connection — reconnect, then refresh." }
            return
        }
        await loadNetworkStatus(transport)
    }

    func scanWifi() async {
        guard let transport else {
            await MainActor.run { errorMessage = "No gauge connection — reconnect, then scan." }
            return
        }
        await MainActor.run { isScanningWifi = true; errorMessage = nil }
        do {
            // The gauge's scan can legitimately return an empty list when the
            // radio was still settling (first call after connect). One retry
            // keeps the first tap useful.
            for attempt in 0..<2 {
                let response = try await transport.get("network/scan")
                guard response.status == 200 else {
                    await MainActor.run { isScanningWifi = false; errorMessage = APIErrorText.from(response) }
                    return
                }
                let decoded = try JSONDecoder().decode(WifiScanResult.self, from: response.body)
                let networks = decoded.networks ?? []
                await MainActor.run { wifiNetworks = networks }
                if !networks.isEmpty || attempt == 1 { break }
                try? await Task.sleep(nanoseconds: 700_000_000)
            }
            await MainActor.run { isScanningWifi = false }
        } catch {
            await MainActor.run { isScanningWifi = false; errorMessage = error.localizedDescription }
        }
    }

    func saveWifi() async {
        guard let transport else { return }
        let ssid = wifiSSID.trimmingCharacters(in: .whitespaces)
        guard !ssid.isEmpty else { errorMessage = "SSID required"; return }
        await MainActor.run { isSavingWifi = true; errorMessage = nil; savedMessage = nil }
        var body: [String: Any] = ["ssid": ssid, "mode": "apsta"]
        if !wifiPassword.isEmpty { body["password"] = wifiPassword }
        do {
            let response = try await transport.send("PUT", path: "network", body: body)
            guard response.status == 200 else {
                await MainActor.run { isSavingWifi = false; errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(NetworkStatus.self, from: response.body)
            await MainActor.run { networkStatus = decoded; isSavingWifi = false; savedMessage = "Wi-Fi saved"; wifiPassword = "" }
        } catch {
            await MainActor.run { isSavingWifi = false; errorMessage = error.localizedDescription }
        }
    }

    /// Sends the Wi-Fi network the PHONE is currently on to the gauge: fetches
    /// the current SSID (NEHotspotNetwork when the Hotspot entitlement is
    /// present, else the iOS-14+ SystemConfiguration cached hint), then a
    /// password-less PUT (keepPassword=true retains the stored PSK if the
    /// gauge already knows this SSID). The gauge joins it itself.
    func usePhoneWifi() async {
        guard let transport else {
            await MainActor.run { errorMessage = "No gauge connection — connect first, then retry." }
            return
        }
        // iOS needs Location permission before it will reveal the SSID. Ask
        // right here — the system dialog pops from this tap — instead of
        // sending the user to Settings.
        let granted = await Self.requestLocationPermission()
        var full = granted
        if granted {
            // Pop the one-time "precise location" dialog right here if the
            // user (or iOS default) has reduced accuracy — no Settings trip.
            full = await Self.requestFullAccuracyIfNeeded()
        }
        let result: (ssid: String?, diag: String) = full ? await Self.currentPhoneWifiSSIDWithDiag() : (nil, "perm:denied")
        guard let ssid = result.ssid, !ssid.isEmpty else {
            await MainActor.run {
                if !granted {
                    errorMessage = "Location access was denied. Enable it in Settings → Privacy → Location Services → Boost Gauge, then retry."
                } else {
                    errorMessage = "iOS hides this iPhone\'s Wi-Fi SSID from apps without a paid developer entitlement. Pick your network from the scan list below instead — same result, one password."
                }
            }
            return
        }
        await MainActor.run { isSavingWifi = true; errorMessage = nil; savedMessage = nil }
        var body: [String: Any] = ["ssid": ssid, "mode": "apsta", "keepPassword": true]
        if !wifiPassword.isEmpty { body["password"] = wifiPassword; body["keepPassword"] = false }
        do {
            let response = try await transport.send("PUT", path: "network", body: body)
            guard response.status == 200 else {
                await MainActor.run { isSavingWifi = false; errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(NetworkStatus.self, from: response.body)
            await MainActor.run {
                networkStatus = decoded
                isSavingWifi = false
                savedMessage = "Gauge joining \(ssid)…"
            }
        } catch {
            await MainActor.run { isSavingWifi = false; errorMessage = error.localizedDescription }
        }
    }

    /// Triggers the system Location dialog from the button tap and waits for
    /// the user's answer. Returns true when permission is granted.
    @MainActor
    private static func requestLocationPermission() async -> Bool {
        let manager = CLLocationManager()
        switch manager.authorizationStatus {
        case .authorizedWhenInUse, .authorizedAlways:
            return true
        case .notDetermined:
            return await withCheckedContinuation { cont in
                let delegate = LocationAuthDelegate { status in
                    cont.resume(returning: status == .authorizedWhenInUse || status == .authorizedAlways)
                }
                manager.delegate = delegate
                delegate.retainCycleBreaker = delegate  // keep alive until callback
                manager.requestWhenInUseAuthorization()
            }
        case .denied, .restricted:
            return false
        @unknown default:
            return false
        }
    }

    /// Reads the SSID the phone is on. NEHotspotNetwork.fetchCurrent (Access
    /// WiFi Information entitlement) is the modern path; CNCopyCurrentNetworkInfo
    /// is the legacy fallback. Both need the Location grant on iOS 14+ — the
    /// button requests it in-line before calling this.
    /// If the grant is reduced-accuracy, pops the system one-time "precise
    /// location" dialog. Returns true when full accuracy is available after.
    @MainActor
    private static func requestFullAccuracyIfNeeded() async -> Bool {
        let manager = CLLocationManager()
        guard manager.accuracyAuthorization == .reducedAccuracy else { return true }
        return await withCheckedContinuation { cont in
            manager.requestTemporaryFullAccuracyAuthorization(withPurposeKey: "WifiSSID") { _ in
                cont.resume(returning: manager.accuracyAuthorization == .fullAccuracy)
            }
        }
    }

    static func currentPhoneWifiSSID() async -> String? {
        await currentPhoneWifiSSIDWithDiag().ssid
    }

    /// Same read, but reports exactly which source failed so the failure is
    /// diagnosable from the error banner instead of guessed at.
    static func currentPhoneWifiSSIDWithDiag() async -> (ssid: String?, diag: String) {
        var diag: [String] = []
        if let hot = try? await NEHotspotNetwork.fetchCurrent(), !hot.ssid.isEmpty {
            return (hot.ssid, "hs:ok")
        } else {
            diag.append("hs:nil")
        }
        guard let interfaces = CNCopySupportedInterfaces() as? [String], !interfaces.isEmpty else {
            return (nil, diag.joined(separator: ",") + ",cnc:no-if")
        }
        diag.append("cnc:\(interfaces.joined(separator: "/"))")
        for ifname in interfaces {
            if let cfDict = CNCopyCurrentNetworkInfo(ifname as CFString) as? [String: Any],
               let ssid = cfDict["SSID"] as? String, !ssid.isEmpty {
                return (ssid, "ok")
            }
            diag.append("\(ifname):nil")
        }
        return (nil, diag.joined(separator: ","))
    }

    func deleteSavedWifi(ssid: String) async {
        guard let transport else { return }
        do {
            let response = try await transport.send("DELETE", path: "network", body: ["ssid": ssid])
            guard response.status == 200 else {
                await MainActor.run { errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(NetworkStatus.self, from: response.body)
            await MainActor.run { networkStatus = decoded; savedMessage = "Removed \(ssid)" }
        } catch {
            await MainActor.run { errorMessage = error.localizedDescription }
        }
    }

    func reconnectWifi() async {
        guard let transport else { return }
        do {
            let response = try await transport.send("POST", path: "network/reconnect", body: [:])
            guard response.status == 200 else {
                await MainActor.run { errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(NetworkStatus.self, from: response.body)
            await MainActor.run { networkStatus = decoded; savedMessage = "Reconnecting…" }
        } catch {
            await MainActor.run { errorMessage = error.localizedDescription }
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
        ]
        await save(transport, method: "PUT", path: "config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(GaugeConfig.self, from: data)
            self?.applyConfig(decoded)
        }
    }

    /// Display page saves both the brightness/dim schedule (PUT /config) and the
    /// display flags (rotation/regionDBuf/teSync/teScanline/pixelShift) via
    /// PUT /themes/config, keeping the two endpoints in one user action.
    func saveDisplay() async {
        guard let transport else { return }
        savedMessage = nil
        let configBody: [String: Any] = [
            "brightnessHigh": brightnessHigh,
            "brightnessLow": brightnessLow,
            "dimSchedule": [
                "enabled": dimEnabled,
                "startMinutes": dimStartMinutes,
                "endMinutes": dimEndMinutes,
            ],
        ]
        let themeBody: [String: Any] = [
            "rotation": rotation,
            "regionDBuf": regionDBuf,
            "teSync": teSync,
            "teScanline": teScanline,
            "pixelShift": pixelShift,
            "pixelShiftSec": pixelShiftSec,
        ]
        do {
            let configResp = try await transport.send("PUT", path: "config", body: configBody)
            guard configResp.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(configResp) }
                return
            }
            let themeResp = try await transport.send("PUT", path: "themes/config", body: themeBody)
            guard themeResp.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(themeResp) }
                return
            }
            let decodedConfig = try JSONDecoder().decode(GaugeConfig.self, from: configResp.body)
            let decodedTheme = try JSONDecoder().decode(ThemeList.self, from: themeResp.body)
            await MainActor.run {
                self.applyConfig(decodedConfig)
                self.applyThemeFlags(decodedTheme)
                self.savedMessage = "Saved"
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }

    func saveDemoMode() async {
        guard let transport else { return }
        savedMessage = nil
        let body: [String: Any] = [
            "demoMode": demoMode,
            "demoFastSweep": demoFastSweep,
        ]
        await save(transport, method: "PUT", path: "themes/config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(ThemeList.self, from: data)
            self?.applyThemeFlags(decoded)
        }
    }

    func saveTpmsBle() async {
        guard let transport else {
            await MainActor.run { errorMessage = "No gauge connection — reconnect, then retry." }
            return
        }
        let body: [String: Any] = ["tpmsBle": tpmsBle]
        await save(transport, method: "PUT", path: "themes/config", body: body) { [weak self] data in
            let decoded = try JSONDecoder().decode(ThemeList.self, from: data)
            self?.applyThemeFlags(decoded)
        }
    }

    func saveThemeFlags() async {
        await saveDemoMode()
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
        guard let transport else {
            errorMessage = "No gauge connection — reconnect, then retry."
            return
        }
        savedMessage = nil
        // Send the SELECTED timezone (curated option or custom fields), not a
        // phone-offset-derived string: a synthetic "UTC4" overwrote the gauge's
        // real POSIX TZ ("EST5EDT,..."), and loadConfig then matched no curated
        // option → the picker flapped back to "Custom" on next load.
        // The firmware /time route requires epochMs (the phone is the time
        // authority, same as the web UI); tz-only bodies 400 with invalid_time.
        let body: [String: Any] = [
            "epochMs": Int64(Date().timeIntervalSince1970 * 1000),
            "timezoneOffsetMinutes": timezoneOffset,
            "timezoneTz": timezoneTZ,
        ]
        await postTimezone(transport, body: body, success: "Device timezone synced")
    }

    /// Apply a curated (or custom) timezone: set the local fields only.
    /// The gauge is updated only when the user taps "Sync timezone to gauge".
    /// Previously this pushed immediately and then saved, producing two toasts
    /// ("Timezone applied" + "Saved") on every picker tap.
    func applyTimezoneOption(_ id: String) async {
        if id != SettingsViewModel.customTimezoneID,
           let option = SettingsViewModel.timezoneOptions.first(where: { $0.id == id }) {
            timezoneOffset = option.offsetMinutes
            timezoneTZ = option.tz
        }
        timezoneSelection = id
    }

    func saveTimezoneCustom() async {
        guard let transport else { return }
        // Push the custom timezone + current time, then persist the config,
        // but show only ONE toast — the intermediate "Timezone saved" + "Saved"
        // double-fire was the second half of the double notification.
        let timeBody: [String: Any] = [
            "epochMs": Int64(Date().timeIntervalSince1970 * 1000),
            "timezoneOffsetMinutes": timezoneOffset,
            "timezoneTz": timezoneTZ,
        ]
        do {
            let timeResp = try await transport.send("POST", path: "time", body: timeBody)
            guard timeResp.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(timeResp) }
                return
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
            return
        }
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


/// One-shot CLLocationManagerDelegate that resumes the permission wait once.
/// Held alive by its own property until the callback fires.
final class LocationAuthDelegate: NSObject, CLLocationManagerDelegate {
    private var continuation: CheckedContinuation<Bool, Never>?
    var retainCycleBreaker: LocationAuthDelegate?

    private let onStatus: (CLAuthorizationStatus) -> Void

    init(onStatus: @escaping (CLAuthorizationStatus) -> Void) {
        self.onStatus = onStatus
    }

    func locationManager(_ manager: CLLocationManager, didChangeAuthorization status: CLAuthorizationStatus) {
        guard status != .notDetermined else { return }
        onStatus(status)
        retainCycleBreaker = nil
    }
}
