import Foundation

/// `@MainActor`: every `@Published` mutation is main-actor-isolated (r9 F1).
/// The decode cache is the deliberate exception: it is guarded by `cacheLock`
/// and only ever read/written from the nonisolated `Task.detached` fetch path.
@MainActor
final class LogsViewModel: ObservableObject {
    @Published var samples: [LogSample] = []
    @Published var isLoading = false
    @Published var errorMessage: String?
    @Published private(set) var anchor: GaugeState?
    @Published private(set) var scopeLabel = "No samples"
    @Published private(set) var window: LogWindow = .fiveMinutes
    /// Monotonic bump on every published sample set. The chart keys its
    /// downsampled-column cache on (revision, column count), so a crosshair
    /// drag never rebuilds the series but a refreshed window still invalidates
    /// the cache instead of drawing stale columns.
    @Published private(set) var dataRevision = 0
    /// True when the last load served decoded samples from the per-limit cache
    /// (the device payload was byte-identical to the previous decode for that
    /// limit), so a chip switch over unchanged data skips JSON decode.
    @Published private(set) var lastLoadUsedCache = false

    /// The default graph window: 5 minutes at the gauge's 5 Hz log rate
    /// (`GET /logs?limit=1500`). Never the full-hour ring (18,000 samples).
    /// `nonisolated`: `load(limit: Int = graphLimit)` evaluates the default at
    /// the nonisolated `.task` call site.
    nonisolated static let graphLimit = 1500

private weak var transport: GaugeTransport?
    private weak var historyTransport: GaugeTransport?
    /// Decoded payloads keyed by limit. The raw body is the cache identity: an
    /// unchanged device payload (body equality, cheap byte compare) reuses the
    /// previous decode instead of re-parsing up to 4,500 JSON objects. Cleared
    /// in `reset` on transport change and replaced when a limit's body changes.
    /// Lock-protected and touched from the nonisolated `Task.detached` fetch
    /// path, so it is deliberately outside the MainActor boundary.
    nonisolated(unsafe) private var decodeCache: [Int: LogCache] = [:]
    nonisolated let cacheLock = NSLock()

    private struct LogCache {
        let body: Data
        let samples: [LogSample]
    }

    func reset(transport: GaugeTransport?, historyTransport: GaugeTransport? = nil) {
        assertMainThread()
        guard self.transport !== transport else { return }
        self.transport = transport
        self.historyTransport = historyTransport
        samples = []
        errorMessage = nil
        anchor = nil
        dataRevision = 0
        lastLoadUsedCache = false
        cacheLock.lock()
        decodeCache.removeAll()
        cacheLock.unlock()
    }

    func load(limit: Int = graphLimit) async {
        guard let transport else {
            await MainActor.run { self.errorMessage = "No active transport — connect in Settings." }
            return
        }
        let historyTransport = self.historyTransport
        let window = LogWindow(limit: limit)
        await MainActor.run {
            self.isLoading = true
            self.errorMessage = nil
        }
        // Stale-while-revalidate: a window chip switch publishes the already
        // decoded payload for the target limit immediately so the chart
        // redraws from cache while fresh data is in flight.
        publishCachedSamples(for: window)
        // Move transport read + JSON decode off the main actor so a 4500-sample
        // decode never blocks the scene-update watchdog (0x8BADF00D). Hop to
        // @MainActor exactly once to publish results; no other thread touches @Published.
        let fetched: (loaded: [LogSample], label: String, degraded: String?, anchor: GaugeState?, cached: Bool)
        do {
            fetched = try await Task.detached(priority: .userInitiated) {
                var loaded: [LogSample]
                var label: String
                var degraded: String?
                var cached = false
                let started = Date()
                do {
                    // Primary: the bounded window over /logs?limit=N. HTTP mode
                    // uses the transport; BLE mode prefers the derived HTTP host
                    // (the full ring is HTTP-only) and otherwise the BLE Control
                    // /logs route (the simulator serves the bounded window there).
                    let httpTransport = historyTransport ?? (transport.transportKind == "HTTP" ? transport : nil)
                    let fetchTransport = httpTransport ?? transport
                    // The firmware serves the bounded /logs window as an async
                    // APP_EV_LOGS event over BLE: allow one generous 20 s
                    // attempt and do not re-send after a timeout (a late
                    // fragment arriving after the old attempt's timeout used to
                    // feed a dead request and duplicate traffic). HTTP/SimBLE
                    // keep their normal single-shot fast paths.
                    let response: Resp
                    if let ble = fetchTransport as? BleTransport {
                        response = try await ble.send("GET", path: "logs?limit=\(limit)", body: [:], timeout: 20, maxRetries: 1)
                    } else {
                        response = try await fetchTransport.get("logs?limit=\(limit)")
                    }
                    guard response.status == 200 else {
                        throw NSError(domain: "Logs", code: response.status,
                                       userInfo: [NSLocalizedDescriptionKey: APIErrorText.from(response)])
                    }
                    if let cachedSamples = self.cachedSamples(for: limit, body: response.body) {
                        loaded = cachedSamples
                        cached = true
                    } else {
                        loaded = try JSONDecoder().decode(LogResponse.self, from: response.body).samples
                        self.storeSamples(loaded, for: limit, body: response.body)
                    }
                    label = "\(window.scope) · \(loaded.count) samples"
                } catch {
                    // Last resort: the capped BLE Log characteristic only carries
                    // the 8-sample BGL1 diagnostic window — report it as degraded.
                    // try? so a fallback failure can NEVER mask the primary error.
                    loaded = (try? await transport.readLogSamples(limit: 8)) ?? []
                    label = "BLE diagnostic window · \(loaded.count) samples"
                    degraded = "History unavailable: \(error.localizedDescription)"
                }
                NSLog("[Logs] limit=%d fetch+decode=%.1fms samples=%d cache=%@",
                      limit, Date().timeIntervalSince(started) * 1000, loaded.count, cached ? "hit" : "miss")
                var newAnchor: GaugeState?
                if let stateResponse = try? await transport.get("state"),
                   stateResponse.status == 200,
                   let decoded = try? JSONDecoder().decode(GaugeState.self, from: stateResponse.body) {
                    newAnchor = decoded
                }
                return (loaded, label, degraded, newAnchor, cached)
            }.value
        } catch {
            // Background refresh failed: keep whatever is on screen (cached or
            // stale-while-revalidate) and surface the note — never wipe the
            // graph the user is looking at.
            await MainActor.run {
                if samples.isEmpty {
                    cacheLock.lock()
                    samples = decodeCache[window.limit]?.samples ?? []
                    cacheLock.unlock()
                }
                errorMessage = error.localizedDescription
                isLoading = false
            }
            return
        }
        await MainActor.run {
            self.samples = fetched.loaded
            self.scopeLabel = fetched.label
            self.anchor = fetched.anchor
            self.window = window
            self.errorMessage = fetched.degraded
            self.lastLoadUsedCache = fetched.cached
            self.dataRevision += 1
            self.isLoading = false
        }
    }

    /// Publish the cached decoded samples for `window.limit` (if any) on the
    /// main actor before the revalidating fetch completes. Bumps the revision
    /// like any other published sample set so the chart's column cache keys on
    /// the newly visible window, not the one that was on screen before the chip
    /// switch. No-op until a previous load has decoded that limit.
    private func publishCachedSamples(for window: LogWindow) {
        cacheLock.lock()
        let cached = decodeCache[window.limit]
        cacheLock.unlock()
        guard let cached else { return }
        samples = cached.samples
        scopeLabel = "\(window.scope) · \(cached.samples.count) samples"
        self.window = window
        lastLoadUsedCache = true
        dataRevision += 1
    }

    nonisolated private func cachedSamples(for limit: Int, body: Data) -> [LogSample]? {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        if let entry = decodeCache[limit], entry.body == body {
            return entry.samples
        }
        return nil
    }

    nonisolated private func storeSamples(_ samples: [LogSample], for limit: Int, body: Data) {
        cacheLock.lock()
        defer { cacheLock.unlock() }
        decodeCache[limit] = LogCache(body: body, samples: samples)
    }

    func csvText() -> String {
        Self.csv(from: samples, anchor: anchor)
    }

    static func csv(from samples: [LogSample], anchor: GaugeState?) -> String {
        var lines = ["timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,psi,peakPsi,zone,demo"]
        let offset = anchor?.timezoneOffsetMinutes
        for sample in samples {
            var epochMs: Int64 = 0
            if let ts = sample.epochTs {
                epochMs = ts
            } else if let tMs = sample.tMs,
                      let anchor,
                      let anchorEpoch = anchor.epochMs,
                      let anchorUptime = anchor.uptimeMs {
                epochMs = anchorEpoch - Int64(anchorUptime) + tMs
            }
            let timestamp = epochMs > 0 ? Format.date(epochMs, offsetMinutes: offset) : ""
            let offsetText = offset.map { String($0) } ?? ""
            let peak = sample.peakPsi.map { Format.psi2($0) } ?? ""
            lines.append(
                "\(timestamp),\(offsetText),\(epochMs),\(sample.uptimeMs),\(Format.psi2(sample.psi)),\(peak),\(sample.zone),\(sample.demo ? 1 : 0)"
            )
        }
        return lines.joined(separator: "\n") + "\n"
    }
}

/// User-selectable graph window. Defaults to 5 minutes; 1m and 15m map to
/// /logs?limit=300 / 4500. Never the full-hour ring (18,000 samples).
struct LogWindow: Identifiable, Equatable {
    let title: String
    let limit: Int
    let scope: String

    var id: Int { limit }

    static let all: [LogWindow] = [
        LogWindow(title: "1m", limit: 300, scope: "Last 1 minute"),
        LogWindow(title: "5m", limit: 1500, scope: "Last 5 minutes"),
        LogWindow(title: "15m", limit: 4500, scope: "Last 15 minutes"),
    ]

    static let fiveMinutes = all[1]

    init(limit: Int) {
        self = Self.all.first { $0.limit == limit } ?? Self.fiveMinutes
    }

    init(title: String, limit: Int, scope: String) {
        self.title = title
        self.limit = limit
        self.scope = scope
    }
}
