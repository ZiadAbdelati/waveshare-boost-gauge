import Foundation

/// In-process request router for the Control characteristic
/// (b6a00001-0000-4000-8000-00000000b6a0).
///
/// The wire envelope is `{"id":u32,"path":str,"method":"GET|PUT|POST","body":{...}}`
/// and every answered request is notified back as
/// `{"id":u32,"status":int,"body":...}` where `status` is an HTTP-style code.
/// The path space mirrors the gauge's HTTP API (see main/boost_web.c for the
/// firmware field shapes) with the routes below.
final class ControlRouter {
    /// Logical request/response payload cap (the GATT doc's ≤480 B rule).
    /// Oversized requests are answered 413 too_large when an `id` is parseable.
    static let maxPayloadBytes = 480

    private static let idPattern = try! NSRegularExpression(
        pattern: #"\"id\"\s*:\s*(\d+)"#
    )

    private let sim: SimModel
    private let verbose: Bool

    init(sim: SimModel, verbose: Bool) {
        self.sim = sim
        self.verbose = verbose
    }

    /// Handle one control write (expected ≤ 480 bytes; larger payloads get a
    /// 413 too_large response when an `id` is parseable). Returns the JSON
    /// response Data, or nil when the request had no parseable `id` (we cannot
    /// address a response then; the caller still ACKs the ATT write).
    func handle(raw: Data) -> Data? {
        if raw.count > Self.maxPayloadBytes {
            guard let id = extractID(from: raw) else {
                log("[control] warn: oversized control write (\(raw.count) B) without parseable id")
                return nil
            }
            log("[control] request oversized (\(raw.count) B) -> 413 too_large")
            return response(id: id, status: 413, body: ["error": "too_large"])
        }
        guard
            let json = try? JSONSerialization.jsonObject(with: raw),
            let object = json as? [String: Any],
            let idNumber = object["id"] as? NSNumber,
            let rawPath = object["path"] as? String,
            let rawMethod = object["method"] as? String
        else {
            log("[control] warn: dropped control write without parseable id/path/method (\(raw.count) B)")
            return nil
        }

        let id = idNumber.uint32Value
        let method = rawMethod.uppercased()

        var body: [String: Any] = [:]
        if let rawBody = object["body"] {
            guard let dictBody = rawBody as? [String: Any] else {
                log("[control] request #\(id): \(method) \(rawPath) -> 400 invalid_json (body not an object)")
                return response(id: id, status: 400, body: ["error": "invalid_json"])
            }
            body = dictBody
        }

        if verbose {
            log("[control] request #\(id): \(method) \(rawPath)")
        }

        let (status, responseBody) = route(
            path: normalizedPath(rawPath),
            method: method,
            body: body
        )

        if verbose {
            log("[control] response #\(id): \(status) for \(method) \(rawPath)")
        }
        return response(id: id, status: status, body: responseBody)
    }

    // MARK: - Routing

    /// Accepts the brief's route names ("/state", "themes/active", ...) as well
    /// as the firmware-style full HTTP path ("/api/v1/state") so companion apps
    /// that mirror the HTTP API work without translation.
    private func normalizedPath(_ raw: String) -> String {
        var path = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        if let queryStart = path.firstIndex(of: "?") {
            path = String(path[..<queryStart])
        }
        if path.hasPrefix("/api/v1/") {
            path = String(path.dropFirst("/api/v1/".count))
        } else if path.hasPrefix("/") {
            path = String(path.dropFirst())
        }
        return path
    }

    private func route(
        path: String,
        method: String,
        body: [String: Any]
    ) -> (Int, Any) {
        switch path {
        case "state":
            guard method == "GET" else { return invalidMethod() }
            return (200, sim.statusObject(uptimeMs: sim.uptimeMs))

        case "config":
            switch method {
            case "GET":
                return (200, sim.configObject())
            case "PUT":
                return applyConfig(body)
            default:
                return invalidMethod()
            }

        case "themes":
            guard method == "GET" else { return invalidMethod() }
            let themes: [[String: String]] = GaugeRoutes.themes.map { theme in
                ["id": theme.id, "name": theme.name]
            }
            return (200, themes)

        case "themes/active":
            guard method == "PUT" else { return invalidMethod() }
            guard let id = body["id"] as? String else {
                return (400, ["error": "invalid_theme"])
            }
            guard GaugeRoutes.containsTheme(id) else {
                log("[control] theme not found: \(id)")
                return (404, ["error": "theme_not_found"])
            }
            sim.config.theme = id
            log("[control] theme activated: \(id)")
            return (200, ["id": id, "activeThemeId": id])

        case "tpms/config":
            switch method {
            case "GET":
                return (200, sim.tpmsConfigObject())
            case "PUT":
                return applyTpmsConfig(body)
            default:
                return invalidMethod()
            }

        case "time":
            guard method == "POST" else { return invalidMethod() }
            if let epoch = body["epochMs"], !(epoch is NSNumber) {
                return (400, ["error": "invalid_time"])
            }
            if let offset = body["timezoneOffsetMinutes"], !(offset is NSNumber) {
                return (400, ["error": "invalid_time"])
            }
            if let tz = body["timezoneTz"], !(tz is String) {
                return (400, ["error": "invalid_time"])
            }
            log("[time] clock sync accepted (sim) with body \(describe(body))")
            return (200, ["ok": true])

        case "restart":
            guard method == "POST" else { return invalidMethod() }
            log("[restart] simulated gauge restart requested; doing nothing (sim)")
            return (200, ["ok": true, "restartingInMs": 400])

        case "logs":
            guard method == "GET" else { return invalidMethod() }
            var count = 600
            if let countNumber = body["count"] as? NSNumber {
                count = countNumber.intValue
            }
            guard count > 0 else {
                return (400, ["error": "invalid_logs"])
            }
            return (200, sim.logsObject(count: count))

        case "page":
            guard method == "PUT" else { return invalidMethod() }
            let page: Int
            if let pageNumber = body["page"] as? NSNumber {
                page = pageNumber.intValue
            } else if let pageNumber = body["activePage"] as? NSNumber {
                page = pageNumber.intValue
            } else {
                return (400, ["error": "invalid_page"])
            }
            guard page == 0 || page == 1 else {
                return (400, ["error": "invalid_page"])
            }
            log("[page] active page set to \(page)")
            return (200, ["ok": true, "activePage": page])

        default:
            log("[control] unknown route: \(path)")
            return (404, ["error": "not_found"])
        }
    }

    private func applyConfig(_ body: [String: Any]) -> (Int, Any) {
        if let appBle = body["appBle"] {
            guard let flag = appBle as? Bool else {
                return (400, ["error": "invalid_config"])
            }
            sim.config.appBle = flag
        }
        if let brightness = body["brightness"] {
            guard let number = brightness as? NSNumber else {
                return (400, ["error": "invalid_config"])
            }
            sim.config.brightness = min(max(number.intValue, 0), 100)
        }
        if let theme = body["theme"] {
            guard let id = theme as? String else {
                return (400, ["error": "invalid_config"])
            }
            guard GaugeRoutes.containsTheme(id) else {
                log("[config] unknown theme in /config: \(id)")
                return (400, ["error": "invalid_config"])
            }
            sim.config.theme = id
            log("[config] theme updated via /config: \(id)")
        }
        return (200, sim.configObject())
    }

    private func applyTpmsConfig(_ body: [String: Any]) -> (Int, Any) {
        var next = sim.tpmsConfig
        if let lowKpa = body["lowKpa"] as? NSNumber {
            let value = lowKpa.doubleValue
            guard value >= 100.0 && value <= 400.0 else {
                return (400, ["error": "invalid_tpms_config"])
            }
            next.lowKpa = value
        } else if let lowPsi = body["lowPsi"] as? NSNumber {
            let kpa = lowPsi.doubleValue / 6.894_757_293_168_361
            guard kpa >= 100.0 && kpa <= 400.0 else {
                return (400, ["error": "invalid_tpms_config"])
            }
            next.lowKpa = kpa
        }
        if let stale = body["staleAfterMs"] as? NSNumber {
            let value = stale.intValue
            guard value >= 2_000 && value <= 120_000 else {
                return (400, ["error": "invalid_tpms_config"])
            }
            next.staleAfterMs = UInt32(value)
        }
        sim.tpmsConfig = next
        return (200, sim.tpmsConfigObject())
    }

    private func invalidMethod() -> (Int, Any) {
        (405, ["error": "method_not_allowed"])
    }

    /// Best-effort `id` scan for oversized writes: finds `"id": <digits>` in the
    /// raw ASCII payload so the 413 envelope can still be addressed.
    private func extractID(from data: Data) -> UInt32? {
        guard let ascii = String(data: data, encoding: .ascii) else { return nil }
        let ns = ascii as NSString
        guard let match = Self.idPattern.firstMatch(
            in: ascii,
            range: NSRange(location: 0, length: ns.length)
        ) else { return nil }
        let digitRange = match.range(at: 1)
        guard digitRange.location != NSNotFound else { return nil }
        return UInt32(ns.substring(with: digitRange))
    }

    private func response(id: UInt32, status: Int, body: Any) -> Data {
        let envelope: [String: Any] = [
            "id": NSNumber(value: id),
            "status": NSNumber(value: status),
            "body": body,
        ]
        guard let data = try? JSONSerialization.data(
            withJSONObject: envelope,
            options: [.sortedKeys]
        ) else {
            log("[control] error: failed to serialize response")
            return Data()
        }
        return data
    }

    private func describe(_ body: [String: Any]) -> String {
        let keys = body.keys.sorted().joined(separator: ",")
        return "{\(keys)}"
    }

    private func log(_ line: String) {
        print(line)
        fflush(stdout)
    }
}
