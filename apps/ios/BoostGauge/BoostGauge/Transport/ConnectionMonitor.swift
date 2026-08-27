import Foundation

enum GaugeConnectionState: Equatable {
    case notConfigured
    case connecting
    case connected
    case unreachable(String)
    case notConnected

    /// The transport footer already renders this state as "Unreachable — …",
    /// so StatusView suppresses the redundant amber banner for it.
    var isUnreachable: Bool {
        if case .unreachable = self { return true }
        return false
    }
}

/// Exponential reconnect backoff for the BLE link, matching Android's
/// GaugeRepository.backoffDelayMs: 1→1 s, 2→2 s, 3→5 s, 4→10 s, 5→30 s,
/// 6+→60 s cap. Resets on any successful connect.
enum BLEBackoff {
    static func delayMs(forAttempt attempt: Int) -> UInt64 {
        switch attempt {
        case ...1: return 1_000
        case 2: return 2_000
        case 3: return 5_000
        case 4: return 10_000
        case 5: return 30_000
        default: return 60_000
        }
    }

    static func delayNs(forAttempt attempt: Int) -> UInt64 {
        delayMs(forAttempt: attempt) * 1_000_000
    }
}

final class HTTPConnectionMonitor {
    struct Intervals {
        var success: TimeInterval
        var failure: TimeInterval

        init(success: TimeInterval = 1, failure: TimeInterval = 3) {
            self.success = success
            self.failure = failure
        }
    }

    private let transport: GaugeTransport
    private let intervals: Intervals
    private let onResult: (Result<Resp, Error>) async -> Void
    private var task: Task<Void, Never>?
    private var requestInFlight = false

    init(
        transport: GaugeTransport,
        intervals: Intervals = Intervals(),
        onResult: @escaping (Result<Resp, Error>) async -> Void
    ) {
        self.transport = transport
        self.intervals = intervals
        self.onResult = onResult
    }

    func start() {
        guard task == nil else { return }
        task = Task { [weak self] in
            await self?.run()
        }
    }

    func stop() {
        task?.cancel()
        task = nil
        requestInFlight = false
    }

    deinit {
        task?.cancel()
    }

    static func isSuccess(_ outcome: Result<Resp, Error>) -> Bool {
        if case .success(let response) = outcome {
            return (200..<300).contains(response.status)
        }
        return false
    }

    static func nextDelay(outcomeIsSuccess: Bool, intervals: Intervals) -> TimeInterval {
        outcomeIsSuccess ? intervals.success : intervals.failure
    }

    private func run() async {
        while !Task.isCancelled {
            guard !requestInFlight else {
                try? await Task.sleep(nanoseconds: UInt64(intervals.failure * 1_000_000_000))
                continue
            }
            requestInFlight = true
            let outcome: Result<Resp, Error>
            do {
                outcome = .success(try await transport.get("state"))
            } catch {
                outcome = .failure(error)
            }
            requestInFlight = false
            guard !Task.isCancelled else { return }
            await onResult(outcome)
            let delay = Self.nextDelay(outcomeIsSuccess: Self.isSuccess(outcome), intervals: intervals)
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
        }
    }
}
