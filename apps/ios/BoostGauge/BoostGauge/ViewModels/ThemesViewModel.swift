import Foundation

final class ThemesViewModel: ObservableObject {
    @Published var themes: [Theme] = []
    @Published var activeThemeID: String?
    @Published var isLoading = false
    @Published var errorMessage: String?

    private weak var transport: GaugeTransport?

    func reset(transport: GaugeTransport?) {
        guard self.transport !== transport else { return }
        self.transport = transport
        themes = []
        activeThemeID = nil
        errorMessage = nil
    }

    func load() async {
        guard let transport else {
            errorMessage = "No active transport — choose HTTP or BLE in Settings."
            return
        }
        isLoading = true
        errorMessage = nil
        do {
            let response = try await transport.get("themes")
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            await MainActor.run {
                self.themes = list.themes ?? []
                self.activeThemeID = list.activeThemeId
                self.errorMessage = nil
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
        await MainActor.run { self.isLoading = false }
    }

    func select(_ id: String) async {
        guard let transport else { return }
        do {
            let response = try await transport.send("PUT", path: "themes/active", body: ["id": id])
            guard response.status == 200 else {
                await MainActor.run { self.errorMessage = APIErrorText.from(response) }
                return
            }
            let list = try JSONDecoder().decode(ThemeList.self, from: response.body)
            await MainActor.run {
                self.activeThemeID = list.activeThemeId
                if let themes = list.themes {
                    self.themes = themes
                }
                self.errorMessage = nil
            }
        } catch {
            await MainActor.run { self.errorMessage = error.localizedDescription }
        }
    }
}
