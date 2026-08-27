import Foundation

enum TransportError: LocalizedError, Equatable {
    case invalidURL
    case badResponse
    case requestTooLarge
    case bluetoothUnavailable
    case scanTimeout
    case connectTimeout
    case operationTimeout
    case notConnected
    case deviceNotFound
    case serviceNotFound
    case characteristicNotFound
    case writeFailed
    case busy
    case badLogFormat

    var errorDescription: String? {
        switch self {
        case .invalidURL: return "Invalid server URL"
        case .badResponse: return "Unexpected server response"
        case .requestTooLarge: return "Request exceeds the 480 byte BLE limit"
        case .bluetoothUnavailable: return "Bluetooth is unavailable"
        case .scanTimeout: return "No gauge found within the scan window"
        case .connectTimeout: return "Connection timed out"
        case .operationTimeout: return "The device did not respond"
        case .notConnected: return "Not connected to a gauge"
        case .deviceNotFound: return "The selected gauge is no longer in range"
        case .serviceNotFound: return "Gauge service not found"
        case .characteristicNotFound: return "Gauge characteristic not found"
        case .writeFailed: return "Failed to write to the gauge (v0.8.1-10 retry5+busy)"
        case .busy: return "Another Bluetooth operation is in progress"
        case .badLogFormat: return "Unsupported log format from the gauge"
        }
    }
}

protocol GaugeTransport: AnyObject {
    var transportKind: String { get }
    func get(_ path: String) async throws -> Resp
    func send(_ method: String, path: String, body: [String: Any]) async throws -> Resp
    func readLogSamples(limit: Int) async throws -> [LogSample]
    func liveStatusStream() -> AsyncStream<Result<Data, Error>>
    func disconnect()
}

/// The BLE link surface the reconnect loop and link monitor depend on.
/// `linkStateStream()` is the single source of truth for GATT liveness
/// (didDiscover/`finishConnect` publishes true, didDisconnect publishes
/// false); the session mirrors it, never the loop's own bookkeeping.
protocol BLELinkTransport: GaugeTransport {
    var isConnected: Bool { get }
    func connect(toSavedIdentifier identifier: UUID, name: String) async throws
    func readDeviceInfo() async throws -> BleDeviceInfo
    func linkStateStream() -> AsyncStream<Bool>
}

extension GaugeTransport {
    var transportKind: String { "transport" }
}
