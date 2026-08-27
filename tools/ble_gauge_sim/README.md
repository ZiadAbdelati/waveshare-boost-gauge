# ble_gauge_sim

macOS **CoreBluetooth peripheral simulator** for the ESP32-S3 boost gauge.
It advertises as `BoostGauge` so the iOS and Android companion apps (built in
parallel) can be tested end-to-end over real BLE without the Waveshare board.

Dependency policy: **CoreBluetooth + Foundation only**. No other frameworks,
no third-party packages. Requires macOS 13+.

## Build

```bash
cd tools/ble_gauge_sim
swift build -c release
```

The executable lands at `tools/ble_gauge_sim/.build/release/ble_gauge_sim`.
Xcode/Command-Line Tools with the macOS 13+ SDK is required (@platforms
`.macOS(.v13)`).

## Run

```bash
tools/ble_gauge_sim/.build/release/ble_gauge_sim --verbose
```

Or without the per-request chatter:

```bash
tools/ble_gauge_sim/.build/release/ble_gauge_sim
```

CLI:

| Flag | Meaning |
|---|---|
| `--help` | Usage, GATT map, caveat. |
| `--verbose` | Log every control request/response (default logs connect/disconnect/state/restart events only). |
| `--firmware <string>` | Firmware string in DeviceInfo and Status (default `0.9.0-sim`, e.g. `--firmware v0.9.0-sim`). |

The process runs until interrupted (Ctrl-C). One clean line per event is
printed to stdout: CoreBluetooth state transitions, connect/subscribe,
disconnect/unsubscribe, and (with `--verbose`) each control
request/response.

## What it advertises

- Local name: **BoostGauge**
- Primary service: `b6a00000-0000-4000-8000-00000000b6a0` (in the adv payload)
- Connectable, startAdvertising on launch once CoreBluetooth is powered on.

## GATT map

| Characteristic | UUID suffix | Properties | Payload |
|---|---|---|---|
| Control | `b6a00001-...-b6a0` | write + notify | JSON RPC, requests ≤ 480 B |
| Status | `b6a00002-...-b6a0` | read + notify | `/state`-shaped JSON, notify ~1 Hz while subscribed |
| Log | `b6a00003-...-b6a0` | read | `BGL1\n` + 600 samples (2 min @ 5 Hz); offset reads return the suffix |
| DeviceInfo | `b6a00004-...-b6a0` | read | `{"name":"BoostGauge","fw":"0.9.0-sim","api":1}` |

Status payload (compact `/state`-shaped snapshot, waveform: sine 0-15 psi,
period ~8 s, monotonic `uptimeMs`; `zone` uses the firmware/`docs/bluetooth-gatt.md`
tokens VAC / ATMO / BOOST / OVER):

```json
{
  "psi": 7.32, "peak": 12.4, "zone": "BOOST", "demo": true,
  "uptimeMs": 123456, "brightness": 80, "theme": "dyno-cell",
  "tpms": {"status": "normal", "kpa": [224.3, 225.1, 223.8, 226.0], "valid": [true, true, true, true]},
  "obd": {"state": "ready", "rpm": 850.0},
  "fw": "0.9.0-sim", "transport": "ble"
}
```

## Control protocol

Requests are written to Control as JSON (≤ 480 B):

```json
{"id": 1, "path": "/config", "method": "PUT", "body": {"brightness": 60}}
```

Responses are notified back on Control:

```json
{"id": 1, "status": 200, "body": {"appBle": true, "brightness": 60, "theme": "dyno-cell"}}
```

`status` is an HTTP-style code (200 / 400 / 404 / 405 / 413). Errors use
`{"error": "..."}`. Both `/state` and `/api/v1/state` spellings are accepted
(the firmware HTTP roots). If a request has no parseable `id` the write is
still ACKed at the ATT layer but no JSON response is emitted. A request larger
than 480 bytes is answered `413 {"error": "too_large"}` when its `id` can be
scanned from the payload.

Routes:

| Path | Method | Behavior |
|---|---|---|
| `/state` | GET | Status snapshot (same object as Status characteristic). |
| `/config` | GET/PUT | In-memory config `{"appBle":bool,"brightness":int,"theme":str}`; PUT returns the updated object. |
| `/themes` | GET | `[{"id":"dyno-cell","name":"Dyno Cell"},{"id":"vault-tec","name":"Vault-Tec"},{"id":"night-city","name":"Night City"},{"id":"big-digit","name":"Big Digit"},{"id":"neon","name":"Neon"}]` |
| `/themes/active` | PUT | `{"id":"<theme>"}` → validates, activates, echoes `{"id":..,"activeThemeId":..}`; unknown id → 404. |
| `/tpms/config` | GET/PUT | Firmware-shaped `{"lowKpa":..,"lowPsi":..,"staleAfterMs":..}` (defaults 220 kPa / 15 000 ms, range-checked like firmware). |
| `/time` | POST | Accepts `epochMs`/`timezoneOffsetMinutes`/`timezoneTz` if present → 200 `{"ok":true}`. |
| `/restart` | POST | 200 `{"ok":true,"restartingInMs":400}` and prints the restart event (sim does not reboot). |
| `/logs` | GET | Body `{"count":600}` (default 600, capped at 600) → firmware-shaped `{"samples":[{"tMs":..,"psi":..,"peakPsi":..,"zone":..,"demo":..}]}`. |
| `/page` | PUT | `{"page":0|1}` (or `activePage`) → 200 `{"ok":true,"activePage":..}`. |
| anything else | any | 404 `{"error":"not_found"}`. |

## Log characteristic

Body:

```text
BGL1
t_ms,psi,peak_psi,zone,demo
0,7.50,7.50,BOOST,1
200,7.49,7.50,BOOST,1
...
```

Exactly 600 newline-terminated data lines at 200 ms spacing ending at the
current `uptimeMs`, synthesized from the same sine waveform (running peak
included). Reads honor `CBATTRequest.offset`: `offset > 0` returns the suffix
bytes from that offset; an offset at-or-beyond the end returns a zero-byte
short read (which ends the transfer, per `docs/bluetooth-gatt.md`).

## Caveats

- **Pairing/encryption is NOT simulated.** macOS `CBPeripheralManager` cannot
  require real LE Secure Connections bonding in a useful way for this tool.
  Companion apps must treat `encrypted` as advisory in sim mode (e.g. skip
  start-encryption expectations; no characteristics are protected).
- Config is **in-memory only** — restarting the simulator resets
  `appBle`/`brightness`/`theme` and TPMS thresholds.
- State is single-threaded on the main run loop (CoreBluetooth callbacks and
  the 1 Hz status timer); no locks, no background queues.
- Notifications are serialized per characteristic and fragmented into
  ≤180-byte packets (fits the iOS ATT MTU 185 → 182-byte payload). Clients
  must append packets in arrival order until the buffer parses as one complete
  JSON value (the behavior `docs/bluetooth-gatt.md` requires of apps).
- The Log characteristic carries a **600-sample (2-minute) snapshot** per this
  tool's brief; the firmware ring described in `docs/bluetooth-gatt.md` holds
  18,000 samples (1 hour). The sim deliberately serves the brief's 600-line
  snapshot.
- Advertising is not active until CoreBluetooth reports `state: poweredOn` and
  the service is published (`service added:` + `advertising:` lines).

## Host-side check

```bash
python3 tools/test_ble_sim.py
```

Validates the embedded route/theme table and `BGL1` log format from source
(no radio required).

## Troubleshooting

- **Newly built local binaries won't launch (hang at exec) on a Mac that is
  mid system/Xcode/simulator-runtime install:** the machine-wide code-sign
  validation path can stall for unsigned local binaries. Ad-hoc sign the
  binary and run again:

  ```bash
  codesign -s - --force .build/release/ble_gauge_sim
  ```

  This was required on one development host during an Xcode first-run/simulator
  bootstrap; it is a host quirk, not part of the normal build.
- On the same class of wedged host, `swift build` can stall while SwiftPM
  launches its (unsigned) manifest executable even though `swiftc -typecheck`
  and a direct `xcrun --sdk macosx swiftc -O` compile work. That is an
  environment issue; the direct compile produces the same product.
- CoreBluetooth prints nothing until the daemon delivers a state callback; if
  the whole host's CoreBluetooth is stalled (neither central nor peripheral
  callbacks arrive), wait for the host install to finish before judging the
  simulator.
