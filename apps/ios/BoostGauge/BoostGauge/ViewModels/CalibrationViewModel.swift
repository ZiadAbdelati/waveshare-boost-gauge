import Foundation

/// `@MainActor`: every `@Published` mutation is main-actor-isolated (r9 F1).
@MainActor
final class CalibrationViewModel: ObservableObject {
    @Published var calibration: Calibration?
    @Published var isLoading = false
    @Published var isCalibrating = false
    @Published var isSavingSupply = false
    /// Transient error surfaced as a bottom toast (never an inline banner).
    @Published var errorMessage: String? {
        didSet {
            if let errorMessage { WindowToast.show(errorMessage, color: .systemOrange) }
        }
    }
    /// Transient success surfaced as a bottom toast (never an inline banner).
    @Published var successMessage: String? {
        didSet {
            if let successMessage { WindowToast.show(successMessage, color: .systemGreen) }
        }
    }
    /// Inline, field-level validation error for the supply editor.
    @Published var supplyFieldError: String?

    /// Firmware bounds for `PUT /sensors/supply` (BOOST_MAP_SUPPLY_MIN/MAX).
    static let supplyMin = 4.5
    static let supplyMax = 5.5

    private weak var transport: GaugeTransport?

    func reset(transport: GaugeTransport?) {
        assertMainThread()
        guard self.transport !== transport else { return }
        self.transport = transport
        calibration = nil
        errorMessage = nil
        successMessage = nil
        supplyFieldError = nil
    }

    func load() async {
        // `.task { await vm.load() }` runs on a background executor (SwiftUI);
        // publish on main.
        guard let transport else {
            await MainActor.run { self.errorMessage = "No active transport — connect in Settings." }
            return
        }
        await MainActor.run {
            self.isLoading = true
            self.errorMessage = nil
        }
        do {
            let response = try await transport.get("sensors/calibration")
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(Calibration.self, from: response.body)
            await MainActor.run {
                self.calibration = decoded
                self.errorMessage = nil
                self.supplyFieldError = nil
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
        await MainActor.run { self.isLoading = false }
    }

    func calibrate() async {
        guard let transport else { return }
        isCalibrating = true
        errorMessage = nil
        successMessage = nil
        do {
            let response = try await transport.send("POST", path: "sensors/calibration", body: [:])
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(Calibration.self, from: response.body)
            await MainActor.run {
                self.calibration = decoded
                self.successMessage = "Calibrated to atmosphere"
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
        await MainActor.run { self.isCalibrating = false }
    }

    func setSupplyVolts(_ v: Double) async {
        guard let transport else {
            await MainActor.run { self.errorMessage = "No active transport — connect in Settings." }
            return
        }
        guard v >= Self.supplyMin && v <= Self.supplyMax else {
            await MainActor.run {
                self.supplyFieldError = "Supply must be between \(Self.supplyMin) and \(Self.supplyMax) V"
            }
            return
        }
        isSavingSupply = true
        supplyFieldError = nil
        errorMessage = nil
        successMessage = nil
        do {
            let response = try await transport.send("PUT", path: "sensors/supply", body: ["supplyVolts": v])
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let decoded = try JSONDecoder().decode(Calibration.self, from: response.body)
            await MainActor.run {
                self.calibration = decoded
                self.successMessage = "Supply voltage saved"
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
        await MainActor.run { self.isSavingSupply = false }
    }
}
