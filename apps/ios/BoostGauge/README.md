# Boost Gauge iOS companion

SwiftUI companion app for the ESP32-S3 boost gauge. One native UI, two
transports (HTTP over the `/api/v1/*` control plane, BLE over the custom GATT
service), generated with XcodeGen, no third-party dependencies.

## Layout

- `BoostGauge/Transport/` — `GaugeTransport` protocol, `HttpTransport`,
  `BleTransport` (CoreBluetooth async bridge + request framer).
- `BoostGauge/Models/` — `Codable` models mirroring `main/boost_web.c` payloads.
- `BoostGauge/ViewModels/` — `ObservableObject` view models (Status, Themes,
  Settings, Calibration, Logs) plus `AppSession` (transport lifecycle and
  `UserDefaults` persistence).
- `BoostGauge/Views/` — SwiftUI screens.
- `BoostGaugeTests/` — XCTest suite (in-process URLProtocol HTTP stub, BLE
  framing tests, fake-transport view-model tests, fixture JSON parsing).
- `project.yml` — XcodeGen spec; regenerate the project with `xcodegen generate`.

## Build and test

Requires Xcode 26.x with an iOS simulator runtime. When Xcode appears:

```sh
scripts/check_xcode.sh   # wait for install, select, first-launch, list sims
```

```sh
xcodegen generate
xcodebuild build \
  -project BoostGauge.xcodeproj -scheme BoostGauge \
  -destination 'platform=iOS Simulator,name=iPhone 16' \
  -derivedDataPath DerivedData CODE_SIGNING_ALLOWED=NO
```

Tests:

```sh
xcodebuild test \
  -project BoostGauge.xcodeproj -scheme BoostGauge \
  -destination 'platform=iOS Simulator,name=iPhone 16' \
  -derivedDataPath DerivedData CODE_SIGNING_ALLOWED=NO
```

## E2E against the repo mock server

```sh
scripts/e2e.sh 8080
```

The script starts `tools/mock_server.py`, boots the simulator, installs and
launches the app with `-e2eHTTPURL http://127.0.0.1:8080` (the simulator shares
the host network, so localhost reaches the mock), screenshots to
`/tmp/ios_screen.png`, and greps the mock access log for `/api/v1/state` polls
as the live-data check. The launch-argument hook only overrides the persisted
HTTP host when the argument is present.

`-e2eTab <status|themes|settings|calibrate|logs>` opens a specific tab at
launch so scripted screenshots can land on the screen under test (there is no
`simctl` tap command).

`scripts/sim_settings_verify.sh` runs the Settings free/connected verification:
launch with a dead host (`http://127.0.0.1:9`) and Settings tab, soak 30 s
with screenshots and a process-liveness check, then repeat against the live
mock server on port 18099 and screenshot the green connected state. Screenshot
outputs are `/tmp/ios_settings_unreachable.png` and
`/tmp/ios_settings_connected.png`; the mock access log is
`/tmp/boost-gauge-mock-settings.log`.

## Connection state

No host is assumed on first run: the default URL is only the text-field
placeholder until the user saves a host. The HTTP "connected" indicator is
derived from a live `/api/v1/state` probe (`HTTPConnectionMonitor`, serial, 1 s
success / 3 s failure backoff, 5 s request timeout), not from the existence of
a transport object. That probe is the single HTTP `/state` poller: the same
response feeds both `AppSession.connectionState` and the Status screen stream
(`AppSession.statusStream()`), so only one request per second hits `/state`
while connected. BLE keeps its notify-driven stream. Settings and the Status
footer share the same `GaugeConnectionState` rendering; an unreachable host
shows "Unreachable" with the error instead of a green checkmark.

The Status screen also exposes a segmented Boost | TPMS page control that PUTs
`/page` and reflects `/state.activePage` (with an optimistic selection until
the next state sample confirms it).

## GATT surface (from the companion-app spec)

- Service `b6a00000-0000-4000-8000-00000000b6a0`
- Control `b6a00001-…`: write-with-response + notify; requests are
  `{"id":u32,"path":str,"method":"GET|PUT|POST","body":{…}}` capped at 480 B;
  responses are `{"id":u32,"status":int,"body":{…}}` notifications, reassembled
  across fragments and matched by request id. Wire paths are leading-slash
  control-plane routes (e.g. `/themes/active`) per the firmware route table in
  `main/boost_app_ble.c`; the HTTP transport keeps the prefixless form.
- Status `b6a00002-…`: read + ~1 Hz notify of the `/state`-shaped JSON plus
  `"transport":"ble"`.
- Log `b6a00003-…`: read with automatic CoreBluetooth offset long-read;
  payload is `BGL1\n` + `t_ms,psi,peak_psi,zone,demo\n` rows.
- DeviceInfo `b6a00004-…`: `{"name":"BoostGauge","fw":…,"api":1}`.

The Logs screen uses the Log characteristic over BLE and `GET /logs` over
HTTP, with the other transport's path as fallback. CSV export matches
`/logs.csv` columns: `timestamp_local,utc_offset_minutes,epoch_ms,uptime_ms,
psi,peakPsi,zone,demo`.

## Deviations and risks

- **NavigationView instead of NavigationStack.** NavigationStack is iOS 16+
  and the target is iOS 15.0; NavigationView provides the same single-level
  push navigation on iOS 15.
- **BLE unverified against hardware.** The async CoreBluetooth bridge, request
  matching, fragmentation, and log parsing are unit-tested, but no real gauge
  was available; MTU/fragment behaviour and notify cadence need a device pass.
- `AppSession` persists transport kind/host/peer; BLE auto-reconnect is out of
  scope for v1 (manual Connect in Settings).
- The mock shipped with this repo currently serves `/state` with `tMs`-keyed
  logs; `LogSample` accepts both `tMs` (firmware) and `ts` (older mock) keys.
- Settings writes are explicit "Save" actions per section rather than the web
  dashboard's debounced auto-save.
