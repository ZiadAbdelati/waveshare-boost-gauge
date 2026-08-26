// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "ble_central_test",
    platforms: [.macOS(.v12)],
    targets: [
        .executableTarget(
            name: "ble_central_test",
            path: "Sources/ble_central_test"
        )
    ]
)
