import Foundation

struct CLIOptions {
    var verbose = false
    var firmware = "0.9.0-sim"
}

struct CLIError: Error, CustomStringConvertible {
    let message: String
    var description: String { message }
}

func helpText() -> String {
    """
    Usage: ble_gauge_sim [options]

    macOS CoreBluetooth PERIPHERAL SIMULATOR for the ESP32-S3 boost gauge.
    Advertises as "BoostGauge" (service b6a00000-0000-4000-8000-00000000b6a0,
    connectable, service UUID in the advertisement) so the iOS and Android
    companion apps can be tested end-to-end over real BLE without hardware.

    Options:
      --help                 Show this help and exit.
      --verbose              Log every control request/response (default: only
                             connect/disconnect/state/restart events).
      --firmware <string>    Firmware string reported by DeviceInfo and Status
                             (default: 0.9.0-sim; e.g. --firmware v0.9.0-sim).

    GATT:
      b6a00000-0000-4000-8000-00000000b6a0  primary service
      b6a00001-...-b6a0  Control  read/write+notify JSON RPC (<=480 B requests)
      b6a00002-...-b6a0  Status   read+notify, ~1 Hz /state-shaped JSON
      b6a00003-...-b6a0  Log      read, BGL1 header + 600 samples, offset reads
      b6a00004-...-b6a0  DeviceInfo read

    CAVEAT: pairing/LE-SC encryption is NOT simulated — macOS CBPeripheralManager
    cannot require real bonding in a useful way here. Companion apps must treat
    'encrypted' as advisory in sim mode.
    """
}

func parseArguments(_ arguments: [String]) -> Result<CLIOptions, CLIError> {
    var options = CLIOptions()
    var index = 1
    while index < arguments.count {
        let argument = arguments[index]
        switch argument {
        case "--help", "-h":
            print(helpText())
            exit(0)
        case "--verbose":
            options.verbose = true
        case "--firmware":
            index += 1
            guard index < arguments.count else {
                return .failure(CLIError(message: "--firmware requires a value, e.g. --firmware 0.9.0-sim"))
            }
            options.firmware = arguments[index]
        default:
            if argument.hasPrefix("--firmware=") {
                options.firmware = String(argument.dropFirst("--firmware=".count))
            } else {
                return .failure(CLIError(message: "unknown option: \(argument)"))
            }
        }
        index += 1
    }
    if options.firmware.isEmpty {
        return .failure(CLIError(message: "--firmware must not be empty"))
    }
    return .success(options)
}

let options: CLIOptions
switch parseArguments(Array(CommandLine.arguments)) {
case .success(let parsed):
    options = parsed
case .failure(let message):
    FileHandle.standardError.write(Data("\(message)\n\n\(helpText())\n".utf8))
    exit(1)
}

print("BoostGauge BLE peripheral simulator (macOS CoreBluetooth)")
print("  firmware: \(options.firmware)")
print("  verbose: \(options.verbose ? "on" : "off")")
print("  service: \(GATTPeripheral.serviceUUIDString)")
print("  pairing/encryption: NOT simulated — 'encrypted' is advisory in sim mode")

let sim = SimModel(firmware: options.firmware)
let router = ControlRouter(sim: sim, verbose: options.verbose)
let peripheral = GATTPeripheral(sim: sim, router: router, verbose: options.verbose)
peripheral.start()

// CoreBluetooth and the 1 Hz status timer are driven by the main run loop.
RunLoop.main.run()
