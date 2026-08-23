// swift-tools-version: 5.9
//
// BoostGauge BLE peripheral simulator (macOS CoreBluetooth).
// Deliberately dependency-free: CoreBluetooth + Foundation only, macOS 13+.

import PackageDescription

let package = Package(
    name: "ble_gauge_sim",
    platforms: [
        .macOS(.v13)
    ],
    targets: [
        .executableTarget(
            name: "ble_gauge_sim",
            path: "Sources/ble_gauge_sim"
        )
    ]
)
