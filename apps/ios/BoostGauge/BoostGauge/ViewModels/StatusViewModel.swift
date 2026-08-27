import Foundation

/// `@MainActor`: every `@Published` mutation is main-actor-isolated (r9 F1).
@MainActor
final class StatusViewModel: ObservableObject {
    @Published var state: GaugeState?
    @Published var themeName: String?
    @Published var errorMessage: String?
    @Published var isLoading = true
    @Published private(set) var transportKind: String?
    @Published private(set) var tpmsBleEnabled: Bool?

    private weak var transport: GaugeTransport?
    private var statusStream: AsyncStream<Result<Data, Error>>?
    private var updateTask: Task<Void, Never>?
    private var pollTask: Task<Void, Never>?
    private var themeNames: [String: String] = [:]

    func reset(
        transport: GaugeTransport?,
        statusStream: AsyncStream<Result<Data, Error>>? = nil
    ) {
        assertMainThread()
        let needsRestart = updateTask == nil
        guard self.transport !== transport || needsRestart else { return }
        updateTask?.cancel()
        pollTask?.cancel()
        updateTask = nil
        self.transport = transport
        self.statusStream = statusStream
        state = nil
        themeName = nil
        errorMessage = nil
        tpmsBleEnabled = nil
        isLoading = true
        transportKind = transport?.transportKind
        start()
    }

    func start() {
        guard let transport else {
            isLoading = false
            errorMessage = "No active transport — connect in Settings."
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
        pollTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.forceRefresh()
                try? await Task.sleep(nanoseconds: 1_000_000_000)
            }
        }
    }

    func stop() {
        updateTask?.cancel()
        pollTask?.cancel()
        updateTask = nil
        pollTask = nil
    }

    func forceRefresh() async {
        guard let transport else { return }
        do {
            let response = try await transport.get("state")
            guard response.status == 200 else {
                await MainActor.run {
                    self.isLoading = false
                    self.errorMessage = APIErrorText.from(response)
                }
                return
            }
            await apply(statusData: response.body)
        } catch {
            await MainActor.run {
                self.isLoading = false
                self.errorMessage = error.localizedDescription
            }
        }
    }

    private func apply(statusData: Data) async {
        do {
            let decoded = try JSONDecoder().decode(GaugeState.self, from: statusData)
            await MainActor.run {
                assertMainThread()
                self.state = decoded
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

    func loadThemeNames(_ transport: GaugeTransport) async {
        guard let response = try? await transport.get("themes"),
              let object = try? response.jsonObject(),
              let rows = object["themes"] as? [[String: Any]] else {
            return
        }
        let tpmsBle = object["tpmsBle"] as? Bool
        var names: [String: String] = [:]
        for row in rows {
            if let id = row["id"] as? String {
                names[id] = row["name"] as? String ?? id
            }
        }
        await MainActor.run {
            self.tpmsBleEnabled = tpmsBle
            if let id = self.state?.activeThemeId {
                self.themeName = names[id] ?? id
            }
        }
    }
}
