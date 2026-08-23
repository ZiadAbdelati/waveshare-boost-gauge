import Foundation

final class StatusViewModel: ObservableObject {
    @Published var state: GaugeState?
    @Published var themeName: String?
    @Published var errorMessage: String?
    @Published var isLoading = true
    @Published private(set) var transportKind: String?
    @Published private(set) var pendingPage: Int?

    private weak var transport: GaugeTransport?
    private var statusStream: AsyncStream<Result<Data, Error>>?
    private var updateTask: Task<Void, Never>?
    private var themeNames: [String: String] = [:]

    var displayedPage: Int {
        pendingPage ?? state?.activePage ?? 0
    }

    func reset(
        transport: GaugeTransport?,
        statusStream: AsyncStream<Result<Data, Error>>? = nil
    ) {
        let needsRestart = updateTask == nil
        guard self.transport !== transport || needsRestart else { return }
        updateTask?.cancel()
        updateTask = nil
        self.transport = transport
        self.statusStream = statusStream
        state = nil
        themeName = nil
        errorMessage = nil
        pendingPage = nil
        isLoading = true
        transportKind = transport?.transportKind
        start()
    }

    func start() {
        guard let transport else {
            isLoading = false
            errorMessage = "No active transport — choose HTTP or BLE in Settings."
            return
        }
        updateTask = Task { [weak self] in
            await self?.loadThemeNames(transport)
            let stream = self?.statusStream ?? transport.liveStatusStream()
            for await result in stream {
                switch result {
                case .success(let data):
                    await self?.apply(statusData: data)
                case .failure(let error):
                    await MainActor.run {
                        self?.isLoading = false
                        self?.errorMessage = error.localizedDescription
                    }
                }
            }
        }
    }

    func stop() {
        updateTask?.cancel()
        updateTask = nil
    }

    func forceRefresh() async {
        guard let transport else { return }
        do {
            let response = try await transport.get("state")
            await apply(statusData: response.body)
        } catch {
            await MainActor.run {
                self.isLoading = false
                self.errorMessage = error.localizedDescription
            }
        }
    }

    func setPage(_ page: Int) async {
        guard let transport else { return }
        await MainActor.run { self.pendingPage = page }
        do {
            let response = try await transport.send("PUT", path: "page", body: ["page": page])
            guard response.status == 200 else {
                await MainActor.run {
                    self.pendingPage = nil
                    self.errorMessage = APIErrorText.from(response)
                }
                return
            }
        } catch {
            await MainActor.run {
                self.pendingPage = nil
                self.errorMessage = error.localizedDescription
            }
        }
    }

    private func apply(statusData: Data) async {
        do {
            let decoded = try JSONDecoder().decode(GaugeState.self, from: statusData)
            await MainActor.run {
                self.state = decoded
                if let pending = self.pendingPage {
                    if decoded.activePage == pending {
                        self.pendingPage = nil
                    }
                }
                self.isLoading = false
                self.errorMessage = nil
                if let id = decoded.activeThemeId {
                    self.themeName = self.themeNames[id] ?? id
                }
            }
        } catch {
            await MainActor.run {
                self.isLoading = false
                self.errorMessage = "Could not decode device state"
            }
        }
    }

    private func loadThemeNames(_ transport: GaugeTransport) async {
        guard let response = try? await transport.get("themes"),
              let object = try? response.jsonObject(),
              let rows = object["themes"] as? [[String: Any]] else {
            return
        }
        var names: [String: String] = [:]
        for row in rows {
            if let id = row["id"] as? String {
                names[id] = row["name"] as? String ?? id
            }
        }
        await MainActor.run {
            self.themeNames = names
            if let id = self.state?.activeThemeId {
                self.themeName = names[id] ?? id
            }
        }
    }
}
