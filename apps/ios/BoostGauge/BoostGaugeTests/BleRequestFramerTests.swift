import XCTest
@testable import BoostGauge

final class BleRequestFramerTests: XCTestCase {
    func testMakeRequestEncodesExpectedFields() throws {
        let framer = BleRequestFramer()
        let (id, data) = try framer.makeRequest(method: "PUT", path: "themes/active", body: ["id": "neon"])
        let object = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [String: Any])
        XCTAssertGreaterThan(id, 0)
        XCTAssertEqual(object["id"] as? Int, Int(id))
        XCTAssertEqual(object["path"] as? String, "themes/active")
        XCTAssertEqual(object["method"] as? String, "PUT")
        XCTAssertEqual((object["body"] as? [String: Any])?["id"] as? String, "neon")
    }

    func testRequestIDsIncrement() throws {
        let framer = BleRequestFramer()
        let first = try framer.makeRequest(method: "GET", path: "state", body: [:]).id
        let second = try framer.makeRequest(method: "GET", path: "state", body: [:]).id
        XCTAssertEqual(first + 1, second)
    }

    func testOversizedRequestThrows() {
        let framer = BleRequestFramer()
        let hugeBody: [String: Any] = ["payload": String(repeating: "x", count: 600)]
        XCTAssertThrowsError(try framer.makeRequest(method: "PUT", path: "config", body: hugeBody)) { error in
            guard case TransportError.requestTooLarge = error else {
                return XCTFail("Expected requestTooLarge, got \(error)")
            }
        }
    }

    func testSingleResponseParses() throws {
        let framer = BleRequestFramer()
        let response = responseData(id: 7, status: 200, body: ["psi": 3.0])
        let parsed = framer.append(response)
        XCTAssertEqual(parsed.count, 1)
        XCTAssertEqual(parsed[0].id, 7)
        XCTAssertEqual(parsed[0].status, 200)
        let body = try JSONSerialization.jsonObject(with: parsed[0].body) as? [String: Any]
        XCTAssertEqual(body?["psi"] as? Double, 3.0)
    }

    func testFragmentedResponseIsReassembled() throws {
        let framer = BleRequestFramer()
        let response = responseData(id: 3, status: 200, body: ["psi": 1.25])
        let splitAt = response.count / 2
        let chunks = [response.prefix(splitAt), response.suffix(from: splitAt)]
        XCTAssertTrue(framer.append(chunks[0]).isEmpty)
        let parsed = framer.append(chunks[1])
        XCTAssertEqual(parsed.count, 1)
        XCTAssertEqual(parsed[0].id, 3)
    }

    func testCoalescedResponsesParseBoth() {
        let framer = BleRequestFramer()
        var data = Data()
        data.append(responseData(id: 1, status: 200, body: ["psi": 1.0]))
        data.append(responseData(id: 2, status: 404, body: [:]))
        let parsed = framer.append(data)
        XCTAssertEqual(parsed.map(\.id), [UInt32(1), UInt32(2)])
        XCTAssertEqual(parsed.map(\.status), [Int(200), Int(404)])
    }

    func testGarbagePrefixIsSkipped() throws {
        let framer = BleRequestFramer()
        var data = Data("!!".utf8)
        data.append(responseData(id: 5, status: 200, body: [:]))
        let parsed = framer.append(data)
        XCTAssertEqual(parsed.count, 1)
        XCTAssertEqual(parsed[0].id, 5)
    }

    func testPartialObjectYieldsNothing() {
        let framer = BleRequestFramer()
        let parsed = framer.append(Data(#"{"id":1,"status":200"#.utf8))
        XCTAssertTrue(parsed.isEmpty)
    }

    private func responseData(id: UInt32, status: Int, body: [String: Any]) -> Data {
        let object: [String: Any] = ["id": id, "status": status, "body": body]
        return try! JSONSerialization.data(withJSONObject: object)
    }
}

final class BleLogParsingTests: XCTestCase {
    func testParsesHeaderAndRows() throws {
        let payload = """
        BGL1
        t_ms,psi,peak_psi,zone,demo
        1000,1.50,2.00,BOOST,0
        2000,-0.25,2.00,VAC,1
        """
        let data = Data(payload.utf8)
        let samples = try BleTransport.parseLogData(data)
        XCTAssertEqual(samples.count, 2)
        XCTAssertEqual(samples[0].tMs, 1000)
        XCTAssertEqual(samples[0].psi, 1.5)
        XCTAssertEqual(samples[0].peakPsi, 2.0)
        XCTAssertEqual(samples[0].zone, "BOOST")
        XCTAssertFalse(samples[0].demo)
        XCTAssertTrue(samples[1].demo)
    }

    func testRejectsBadMagic() {
        let data = Data("not-a-log".utf8)
        XCTAssertThrowsError(try BleTransport.parseLogData(data)) { error in
            guard case TransportError.badLogFormat = error else {
                return XCTFail("Expected badLogFormat, got \(error)")
            }
        }
    }
}
