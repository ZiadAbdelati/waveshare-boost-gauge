import Foundation

struct Resp {
    let status: Int
    let body: Data

    func jsonObject() throws -> [String: Any] {
        guard let object = try JSONSerialization.jsonObject(with: body) as? [String: Any] else {
            throw TransportError.badResponse
        }
        return object
    }

    func jsonArray() throws -> [Any] {
        guard let array = try JSONSerialization.jsonObject(with: body) as? [Any] else {
            throw TransportError.badResponse
        }
        return array
    }
}
