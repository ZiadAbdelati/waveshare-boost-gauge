import Foundation

final class CalibrationViewModel: ObservableObject {
    @Published var calibration: Calibration?
    @Published var isLoading = false
    @Published var isCalibrating = false
    @Published var errorMessage: String?
    @Published var successMessage: String?

    private weak var transport: GaugeTransport?

    func reset(transport: GaugeTransport?) {
        guard self.transport !== transport else { return }
        self.transport = transport
        calibration = nil
        errorMessage = nil
        successMessage = nil
    }

    func load() async {
        guard let transport else {
            errorMessage = "No active transport — choose HTTP or BLE in Settings."
            return
        }
        isLoading = true
        errorMessage = nil
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
}
