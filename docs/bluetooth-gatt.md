# Boost Gauge Bluetooth GATT — normative contract

This document is the authoritative GATT specification for the Bluetooth
companion app. Implementors are the firmware (`main/boost_app_ble.c`), the iOS
app (CoreBluetooth), the Android app, and the `tools/ble_gauge_sim` peripheral
simulator; all four MUST agree with this file byte-for-byte. Background and
scope decisions are in
[plans/2026-08-23-bluetooth-companion-app.md](plans/2026-08-23-bluetooth-companion-app.md).
Breaking changes to anything here bump `"api"` in the Device info
characteristic; additive changes do not.

## Service

| Field | Value |
|---|---|
| Service UUID | `b6a00000-0000-4000-8000-00000000b6a0` |
| Name | `BoostGauge` |
| Availability | Only while the `appBle` toggle is on (persisted like `tpmsBle`, default **OFF**; a fresh boot never advertises) |

All characteristic UUIDs share the base `-0000-4000-8000-00000000b6a0`.

## Characteristics

| Characteristic | UUID | Properties | Security |
|---|---|---|---|
| Control | `b6a00001-0000-4000-8000-00000000b6a0` | Write (with response), Notify | **Encrypted** (LE Secure Connections) |
| Status | `b6a00002-0000-4000-8000-00000000b6a0` | Read | Plaintext |
| Log | `b6a00003-0000-4000-8000-00000000b6a0` | Read (offset long-reads) | **Encrypted** (LE Secure Connections) |
| Device info | `b6a00004-0000-4000-8000-00000000b6a0` | Read | Plaintext |

### Control

Requests are written with-write-response as one JSON envelope, ≤ 480 bytes
logical payload:

```json
{"id": <u32>, "path": "<string>", "method": "GET|PUT|POST", "body": {...}}
```

- `id` — client-chosen unsigned integer, echoed verbatim in the response. The
  client uses it to correlate responses. **One request in flight per
  connection**: do not write a new request until the response for the previous
  `id` arrives (or a client-side timeout elapses).
- `path` — the `/api/v1/...` URI of the existing HTTP control plane
  (`boost_web.c`), e.g. `/api/v1/config`.
- `method` — `GET`, `PUT`, or `POST`. `DELETE` is not part of the envelope.
- `body` — JSON object; empty/absent for simple GETs.
- Serialization: the firmware processes control requests serially per
  connection and dispatches to the **same setters** the HTTP handlers use
  (`boost_theme_*`, `boost_model_set_time`, `boost_tpms_set_config`,
  `boost_sensors_*`, page/restart). There is no second API implementation.

Responses arrive as notifications, one JSON envelope ≤ 480 bytes logical
payload:

```json
{"id": <u32>, "status": <int>, "body": {...}}
```

- `status` — the HTTP status code the shared handler path produces (200, 400,
  409, …). It is an envelope field, not an ATT protocol error.
- Oversize rule: a request larger than 480 bytes, or any request whose response
  body would exceed 480 bytes, is answered with:

  ```json
  {"id": <u32>, "status": 413, "body": {"error": "too_large"}}
  ```

  Clients should use the Log characteristic or the LAN HTTP path instead of
  requesting oversized data over Control.
- `GET /state` is the compact Control snapshot (`psi`, `peakPsi`, `zone`,
  `demo`, `brightness`, `uptimeMs`, `activeThemeId`, `activePage`, and
  `transport`) so it remains within the envelope. Read Status for the complete
  state shape.

### Status

Readable on demand; the payload is the full `/state` shape (identical to HTTP,
built by `boost_json.c`). Companion apps poll Control `GET /state` for live
state instead: the Status characteristic's 1 Hz notify broadcast was removed
2026-08-28 because no client subscribed to it (the full-state push is ~5 ATT
fragments/second and hardware-verified to starve raw reads on the shared
link). This is deliberately **not** the 62.5 Hz WebSocket telemetry
(WebSocket remains browser-only), and nothing here is a demand or throttle
change to the 16 ms gauge path.

### Log

Read-only, **encrypted**. The characteristic is a logical value served in
windows: the client reads at any byte offset (Read Blob); each read returns up
to `ATT_MTU − 1` bytes and the client advances the offset by the bytes
received. A read at an offset beyond the end returns zero bytes (short read),
which ends the transfer. The value is a snapshot of the current ring at read
time.

Format:

```text
BGL1
t_ms,psi,peak_psi,zone,demo
```

- `t_ms` — uptime milliseconds, same `tMs` as `GET /api/v1/logs`.
- `psi`, `peak_psi` — two-decimal values.
- `zone` — the state zone token, one of `VAC`, `ATMO`, `BOOST`, `OVER`
  (no commas, so the CSV lines are unambiguous).
- `demo` — `0` or `1`.

The device ring is 18,000 samples (5 Hz, 1 hour, PSRAM,
`BOOST_LOG_CAPACITY` 18000). The BLE characteristic returns the most recent
eight samples as a bounded diagnostic window; `/logs.csv` remains the full
one-hour export.

### Device info

Read, plaintext:

```json
{"name": "BoostGauge", "fw": "<firmwareVersion>", "api": 1}
```

- `name` — fixed string `BoostGauge`.
- `fw` — the same value as `/api/v1/state.firmwareVersion`.
- `api` — contract version of this document. Bumped only on breaking changes;
  an app that sees an unsupported `api` MUST fail loudly rather than guess.

## Pairing and security

- **LE Secure Connections, "Just Works"** (no MITM, no passkey) — user
  decision 2026-08-23. The link is still **encrypted** and **bonded**; the
  only thing dropped is man-in-the-middle protection, traded for a
  zero-interaction pairing UX (the panel-passkey overlay never won the
  attention race against the phone's own dialog).
- Pairing is triggered by the first access to an **encrypted**
  characteristic (Control write or Log read). Reading Status or Device info
  does not trigger pairing.
- The bond is persisted on both sides (`BT_NIMBLE_NVS_PERSIST`); after the
  first pairing, reconnects are silent. If the phone loses its bond (app
  reinstall) while the gauge keeps one, the gauge deletes its stale bond on
  `REPEAT_PAIRING`/failed encryption and re-pairs cleanly.
- The service exists and advertises only while `appBle` is on.

## Advertising

| Field | Value |
|---|---|
| Name | `BoostGauge` |
| Service UUID | `b6a00000-0000-4000-8000-00000000b6a0` |
| Connectable | Yes |
| Interval | 100–250 ms |
| Default | **Off** (`appBle` persisted toggle, default OFF; fresh boot never advertises) |

## Transport notes

- **MTU.** Clients request the largest ATT MTU the platform allows: Android via
  `requestMtu` (target 517); iOS negotiates automatically (hard cap 185).
- **Writes.** A Control request may be larger than `ATT_MTU − 3`. The firmware
  write handler accepts the fully reassembled value (Write Long where needed),
  so clients do not fragment request writes themselves.
- **Notifications.** A message longer than `ATT_MTU − 3` is fragmented by the
  peripheral across consecutive notification packets. The client appends
  packets in arrival order to a per-characteristic buffer and considers the
  message complete when the buffer parses as **one complete JSON value** (no
  trailing bytes), then resets the buffer. Messages are serialized per
  characteristic, so buffers never interleave.
- **Conformance.** `tools/ble_gauge_sim` implements this spec exactly; both
  apps must pass conformance against it before hardware testing is a valid
  result.

## Host troubleshooting (Intel NUC8 Hackintosh)

Central harness is `tools/ble_central_test` (`swift build -c release`,
`codesign -s - --force .build/release/ble_central_test`). It waits up to
6 s for `CBManagerState.poweredOn` (30 × 200 ms). Verified invariants after
2026-08-24 patch: dedicated `DispatchQueue(label:"ble.central")`, broad
`scanForPeripherals(withServices:nil)` with service/name filtering in
`didDiscover`, single in-flight encrypted write with 8 s timeout + one
re-subscribe retry.

- **If `central` stays `0 (unknown)` after 6 s, reboot the Mac.** `blueutil`
  hung and `getState` reporting `0` means `bluetoothd` is wedged — this host
  showed `central 0 unknown` continuously before reboot. The harness now exits
  `2` with `bluetooth 0 (unknown) after 6000ms — bluetoothd wedged, reboot the
  Mac (do not rapid-retry or kextunload Intel BT)`. **Do not rapid-retry the
  harness and do not `kextunload` IntelBluetoothFirmware/BlueToolFixup/IntelBTPatcher
  — on this NUC8 (IntelBluetoothFirmware 2.5.0, BlueToolFixup 2.7.2, itlwm +
  HeliPort on en0, active LAN on en1) that wedges the controller and requires a
  hard reboot anyway. A clean reboot is the only safe recovery.**
- **Do not toggle `appBle` rapidly over HTTP to re-trigger advertising.** A
  single `PUT /api/v1/config {"appBle":true}` if truly off is safe; rapid
  toggles wedged Intel BT in the same way as rapid central restarts. Verify
  with `curl http://<gauge-ip>/api/v1/config` (expect `"appBle":true`) and
  with a broad scan — the harness logs `discover ? <UUID> rssi -90 svc
  B6A00000-0000-4000-8000-00000000B6A0` when the gauge is advertising.
- **Intel vs Apple silicon:** Apple-silicon Macs use the native controller;
  Intel Hackintosh uses `THIRD_PARTY_DONGLE` (USB transport, Vendor 0x004C) via
  IntelBluetoothFirmware. The harness's broad scan is required because
  `withServices:[svc]` filtering is flaky while the coex discovery cycles on
  this stack.

## Automated physical-device acceptance (2026-08-24)

The repeatable hardware gate uses a tethered iPhone as the BLE central and the
Mac only as the Xcode/XCTest orchestrator:

```sh
bash tools/check_ble_ios_e2e.sh
```

The script verifies that `GET http://192.168.50.102/api/v1/config` answers and
contains `"appBle":true`, selects an available paired physical iPhone from
`xcodebuild -showdestinations`, and runs
`BoostGaugeUITests/HardwareBleE2ETests` three consecutive times. Each run must
complete the full matrix: `GET /state`, `GET /config`, `GET /themes`, page
`0 -> 1 -> 0`, active-theme change and restore, and direct reads of Device
info and the `BGL1` Log value (plus the full `GET /state` shape). Logs, screenshots, accessibility
hierarchies, and one `.xcresult` per run are kept in the printed artifact
directory. `BLE_IOS_E2E_RUNS` changes the repeat count; `IOS_DEVICE_ID` selects
a specific available physical device.

The first accepted run passed 3/3 at
`/tmp/boost-gauge-ble-ios-e2e-20260824-191341-51420`. Its saved config records
`appBle:true`; the gauge boot/runtime log showed advertising and emitted
`control write: <N> B` for the matrix requests, proving the XCTest result was
backed by traffic reaching the firmware rather than a UI-only assertion.

The iPhone architecture is intentional, not a transport substitution. On the
Intel NUC8 Hackintosh, the gauge advertised continuously and an iPhone found
it immediately, while Mac CoreBluetooth usually delivered no gauge
advertisements after its first scan session; a `blueutil` power cycle could
re-arm only the next scan. The original Mac harness also stopped discovery on
the first arbitrary peripheral callback, so a nearby JetKVM could terminate a
pass before the service/name filter accepted `BoostGauge`. Filtering that
callback fixed the harness bug but did not make the Intel controller a stable
acceptance client. The physical iPhone exercises the real radio, pairing,
encryption, ATT fragmentation, GATT reads/writes, and app transport code while
remaining hands-free under XCTest.

Hardware bring-up exposed several coupled limits that are now part of the
contract:

- NimBLE host task stack is 8 KiB (the prior 4 KiB overflowed during the
  companion request path), and the companion BLE driver task stack is 6 KiB.
  Do not reduce either without a physical matrix soak.
- Control responses are sent only after notification subscription is ready;
  reconnect code must not trust a stale cached readiness state.
- Control `GET /state` stays in the compact <=480-byte envelope documented
  above. The Status read is independently bounded to the characteristic size.
- BLE Log is the recent eight-sample `BGL1` diagnostic window. The full
  one-hour ring remains an HTTP export and must not be forced through ATT.
- Native Logs views graph whichever history the active transport actually
  returns and label its scope. Each row is one 5 Hz ring sample: gauge uptime,
  instantaneous PSI, peak PSI, zone, and whether demo mode produced it. HTTP
  can request the full ring; BLE must never present its eight-sample window as
  the complete hour.
- Preserve the iPhone/gauge bond between gate runs. Do not uninstall the app or
  erase Bluetooth state as routine test setup: doing so replaces the silent
  reconnect path with a pairing-recovery test and invalidates repeatability.

After any firmware flash, re-check both witnesses before running the matrix:
the HTTP config must answer with `appBle:true`, and serial must show the
`advertising` line. A passing XCTest without the firmware `control write`
lines is not sufficient evidence for the physical BLE gate.

Android follows the same hardware boundary. An Android emulator can cover the
build, UI, state machine, and simulated-GATT flows, but it has no trustworthy
path to the gauge's physical BLE radio. Real Android BLE discovery, pairing,
encryption, MTU/fragment behavior, and this request matrix require a tethered
physical Android device; do not report emulator-only results as hardware BLE
acceptance.

The peripheral currently has one connection slot. Pairing/bonding another
phone does not hide it, but an *active* central connection does: firmware stops
advertising at connect and resumes after disconnect. Therefore physical iOS
and Android gates run sequentially, with the first runner fully terminated and
about two seconds allowed for advertising to resume. The Android test uses
test-only UI automation to accept the initial system pairing confirmation;
later bonded runs reconnect silently. Android scans broadly and applies the
BoostGauge name/service filter in the callback because controller-side UUID
filters missed the split legacy ADV/scan-response payload on the test tablet.
Its BLE transport also normalizes the app's HTTP-style relative route names to
the GATT contract's slash-prefixed paths.
