# Bluetooth companion app — plan

**Date:** 2026-08-23
**Status:** proposal — firmware + apps in progress in this worktree; hardware gates pending (board not available)
**Branch:** `companion-app`
**Supersedes:** [2026-07-26-bluetooth-android-app.md](./2026-07-26-bluetooth-android-app.md) — the earlier Android-only
mirror plan. Its transport split (BLE telemetry at 20 Hz + HTTP for OTA/logs) and its native-gauge-mirror target are
replaced by the locked decisions below.
**GATT contract:** [../bluetooth-gatt.md](../bluetooth-gatt.md) is the normative reference; this plan only summarises it.

## Why Bluetooth at all

The case is the *car*, exactly as the superseded plan argued:

- In the driveway the gauge joins the home LAN and everything works.
- On the road there is no LAN. The gauge falls back to SoftAP, and a phone
  joining a SoftAP with no internet gets nagged by Android, may auto-drop the
  network, and loses its cellular data connection while attached.

BLE fixes that: the phone keeps its data connection while talking to the gauge.
Where the previous plan built a driving mirror, this plan is **settings-first**:
the app is the companion that lets you change gauge settings, watch status, and
pull the log ring in the car without touching Wi-Fi.

## Locked decisions

These decisions are locked for this project. They deliberately reverse the
superseded plan's mirror-and-high-rate-telemetry assumptions.

| Decision | Detail |
|---|---|
| Settings/control-first scope | The app is a settings + status + log companion, not a gauge mirror. Priority flows: pair once, adjust config/theme/time/TPMS/calibration, watch status, pull logs. |
| **NO** native gauge mirror | A native renderer would be a **third** renderer to keep in sync (firmware, web, native) — the main ongoing tax of the superseded plan. The web mirror and its dev tooling stay the only live renderers; apps present status numerically and as settings, never the faces. |
| Single native UI per platform | Exactly one SwiftUI app and one Jetpack Compose app. No per-theme or per-face variants, no second UI convention to maintain. |
| Transport: BLE in car / HTTP REST on LAN | Chosen by reachability, **one at a time**. The app never mixes transports within an operation. WebSocket stays browser-only. |
| Keep the existing RAM log ring | The device keeps its 1-hour ring unchanged (5 Hz, `BOOST_LOG_CAPACITY` 18000, 432,000 B PSRAM). The app pulls the ring and logs locally per session. No firmware-side ring change. |
| OTA stays Wi-Fi/HTTP only | `POST /api/v1/ota` only. No BLE OTA. |
| Media upload LAN-only | GIF upload/delete stay on the raw dual-slot store over HTTP; not exposed over BLE. |
| Pairing | LE Secure Connections with a passkey shown on the gauge panel; bond persisted so car use is a one-time pair. |
| Deferred: flash-persistent logs | ~432 KB/hr of flash wear is fine, but persistence needs a partition-table change = full reflash, plus erase-stall risk on the 16 ms render path. Not now. |
| Deferred: BLE OTA | Revisit only if Wi-Fi/HTTP OTA proves impractical in the car; today it does not. |

## Shared contract

### HTTP surface (already in `main/boost_web.c`)

The app's LAN path and the BLE Control path both speak the same control-plane
semantics these handlers already implement:

- `GET /api/v1/state` — status snapshot (also the basis of the BLE Status char)
- `GET|PUT /api/v1/config` — brightness / dim schedule / gauge range
- `POST /api/v1/time` — clock sync (same setters and RTC/clock guards)
- `GET /api/v1/themes`, `PUT /api/v1/themes/active`, `PUT /api/v1/themes/config` — theme order/picker/settings
- `GET|PUT /api/v1/tpms/config` — TPMS alert threshold and staleness
- `GET|POST /api/v1/sensors/calibration`, `PUT /api/v1/sensors/supply`, `GET /api/v1/sensors/scan`
- `PUT /api/v1/page` — boost ↔ TPMS page
- `GET|DELETE /api/v1/logs`, `GET /api/v1/logs.csv` — RAM ring (HTTP bulk path)
- `POST /api/v1/media`, `DELETE /api/v1/media`, `GET /api/v1/media/status` — raw dual-slot media (LAN-only)
- `POST /api/v1/ota` — Wi-Fi/HTTP-only OTA
- `POST /api/v1/restart`
- `GET|PUT|DELETE /api/v1/network`, `POST /api/v1/network/reconnect`, `GET /api/v1/network/scan`
- WebSocket `/ws/state` — browser-only; the companion apps never speak WebSocket

### New BLE GATT service (normative: `docs/bluetooth-gatt.md`)

One custom service, UUID `b6a00000-0000-4000-8000-00000000b6a0`:

| Characteristic | UUID suffix | Properties | Security | Payload |
|---|---|---|---|---|
| Control | `b6a00001` | write-with-response + notify | **ENCRYPTED** | JSON request/response envelope ≤ 480 B |
| Status | `b6a00002` | read + notify (~1 Hz) | plaintext | same JSON as `GET /api/v1/state` + `"transport":"ble"` |
| Log | `b6a00003` | read (offset long-reads) | **ENCRYPTED** | `BGL1\n` header + CSV lines |
| Device info | `b6a00004` | read | plaintext | `{"name":"BoostGauge","fw":...,"api":1}` |

Control requests are `{"id":<u32>,"path":<str>,"method":"GET|PUT|POST","body":{...}}`;
responses arrive by notify as `{"id":<u32>,"status":<int>,"body":{...}}`. A request
larger than 480 B, or a response that would exceed 480 B, is answered with
`413 {"error":"too_large"}`. Advertising carries the service UUID and name
`BoostGauge`, connectable, 100–250 ms interval. LE Secure Connections pairing is
triggered by accessing an encrypted characteristic; the passkey is shown on the
gauge panel and the bond is persisted. The whole service is gated on the
`appBle` toggle, persisted like `tpmsBle`, default **OFF** — a fresh boot never
advertises.

## Firmware work

### New module: `main/boost_app_ble.c/.h`

Ownership: the GATT server side of the already-running NimBLE host. It is
**dual-role**: the peripheral (phone) lives beside the central in
`boost_obd_ble.c` (OBD2/TPMS) on the same controller.

- Registers the service/characteristics and the security callbacks under the
  existing NimBLE host.
- Control dispatch calls the **same setters the HTTP handlers use**
  (`boost_theme_*`, `boost_model_set_time`, `boost_tpms_set_config`,
  `boost_sensors_*` calibration/supply, page/restart paths) — never a second
  API implementation, never a compatibility shim.
- Responses reuse the existing JSON bodies where they fit the envelope; the
  413 rule covers the rest.
- Status notify is a ~1 Hz publication of the `/api/v1/state` shape with
  `"transport":"ble"` added. It is deliberately **not** the 62.5 Hz WebSocket
  telemetry path; nothing about the 16 ms gauge path changes.

### Radio configuration

`CONFIG_BT_CTRL_BLE_MAX_ACT` goes **3 → 5** in `sdkconfig.defaults`: the current
3 covers the OBD central's scan↔connect handoff; a live peripheral link plus a
live central link plus scanning needs 5 controller activity instances (~828 B
each). Verify in the generated `sdkconfig`, never just the defaults file.

### Opt-in, persisted, default off

`appBle` is persisted exactly like `tpmsBle` (NVS, exposed through
`/api/v1/themes/config`, default OFF). A fresh boot never touches the radio for
the app path. **Bootstrap:** enable once at home over Wi-Fi, pair while the
panel is reachable, then leave the toggle on for car use.

### Hardware gates — ALL pending, required before merge

The board is not available in this worktree, so **no hardware measurement is
claimed in this plan**. Every gate below is a merge blocker and each result must
land in `docs/regression-ledger.md` with the condensed guard updated in
`AGENTS.md` in the same change.

| Gate | What must pass |
|---|---|
| RAM spike with `OBD_BLE_MIN_DMA_BLOCK` guard | Boot with `appBle` on and the OBD central on/off: the existing 40,960 B DMA largest-block floor still guards init — refuse gracefully, never a boot loop; record free internal RAM at peak (phone + OBD + Wi-Fi + display). |
| Two-live-link radio soak | OBD2 FD+ (7.5–15 ms connection interval) **and** phone (~30–50 ms) connected simultaneously: TPMS freshness, OBD latency, and the 30 s `tools/check_display_cadence.py` soak on the dyno-cell demo — sustained median physical `renderFps` ≥ 60, no `ESP_ERR_NO_MEM`, no `send color data failed`. |
| HTTP latency re-check | Repeat the `/api/v1/state` p50/p90 measurement of the 2026-07-26 spike with both links live; expect degradation, record it, require zero timeouts. |
| Ledger rows | All of the above recorded, with the guard text updated in the same change. |

## iOS app

- **SwiftUI + CoreBluetooth**, minimum iOS 15.
- Scan by service UUID (`b6a00000-0000-4000-8000-00000000b6a0`).
- Automatic MTU (CoreBluetooth negotiates; max 185 — see the fragmentation rule
  in the GATT doc).
- Passkey pairing handled by CoreBluetooth (the panel shows the passkey).
- **URLSession** for the HTTP REST path on LAN.
- `bluetooth-central` background mode + state restoration (restore identifier),
  so the app can resume after being backgrounded.
- Log CSV saved to Files (document picker / `fileExporter`).
- Local session logging: samples recorded locally per session, exported to CSV.

## Android app

- **Kotlin + Jetpack Compose + coroutines**, minimum SDK 31.
- **No location permission, ever.** `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT`
  with `neverForLocation`.
- Manual `requestMtu` (target 517) on connect; never assume a negotiated MTU.
- GATT callbacks on a dedicated single-thread dispatcher, never the main thread.
- Foreground service (`connectedDevice` type) to keep background BLE alive.
- SAF `ACTION_CREATE_DOCUMENT` for CSV export.
- **OkHttp** for the HTTP REST path.
- Local session logging, same as iOS.

## Phasing

Each phase is independently useful and gated. Hardware gates are marked pending
until the board is available; host/simulator gates run now.

| Phase | Deliverable | Gate |
|---|---|---|
| **P0** | Firmware GATT server: service + 4 characteristics, `appBle` toggle default off, Control dispatch to the HTTP setters, `CONFIG_BT_CTRL_BLE_MAX_ACT=5` | nRF Connect verify on a phone: pair (passkey on panel), read Device info/Status, Control round-trip, Log long-read. Bench RAM init check against `OBD_BLE_MIN_DMA_BLOCK`. Hardware gates pending. |
| **P1** | iOS skeleton: scan, connect, pair, Status read/notify, Device info | Runs against `tools/ble_gauge_sim` and shows live status; Xcode install pending. |
| **P2** | Android skeleton: scan, connect, pair, Status read/notify, Device info | Runs against `tools/ble_gauge_sim`; manifest has no location permission. |
| **P3** | BLE control surfaces via Control char: config, theme, time, TPMS, calibration, page, restart | Conformance checks in the simulator: id echo, 413 `too_large`, encrypted-access pairing trigger. |
| **P4** | HTTP parity incl. media + OTA — the LAN path reaches full dashboard parity | Media upload/abort/delete ordering over HTTP; OTA verified per AGENTS (boot log `0x420000`). Only reachable over HTTP. |
| **P5** | Log pull + session logging: BLE Log offset long-reads, local session log, CSV export | Pull the 18,000-sample ring and round-trip to CSV; count and timestamps match the ring. |
| **P6** | Polish: reconnect/backoff, badge, background behavior, store builds | iOS/Android store-conformant builds; Android manifest audit; no location permission. |

## Risks

| Risk | Mitigation |
|---|---|
| Dual-role RAM re-opens the internal-DRAM budget — boot-loop precedent from 2026-07-26 | Keep `OBD_BLE_MIN_DMA_BLOCK`, app BLE init after the web control plane, opt-in default off, and the hardware RAM gate before merge. |
| Two-live-link radio is unmeasured (OBD central + phone peripheral on one 2.4 GHz radio) | Required two-live-link soak + HTTP latency re-check gates; the exponential inter-scan backoff already guards the disconnected central. |
| Android BLE fragmentation (MTU, vendor quirks, background) | minSdk 31, manual `requestMtu`, dedicated GATT dispatcher, foreground service, test on more than one handset. |
| iOS background limits | `bluetooth-central` + state restoration; treat background as degraded — log locally and flush on foreground. |
| Clients drift (firmware + iOS + Android + simulator) | One normative `docs/bluetooth-gatt.md`; conformance checks in the simulator and mock server; no second API convention. |
| App bitrot when the board/firmware changes | `"api":1` contract version in Device info; apps fail loudly on mismatch instead of guessing. |

## Simulator / host verification

Both apps are developed and acceptance-tested against host-only stand-ins, with
hardware as the final gate:

1. **`tools/mock_server.py` extended to the full API** — the existing host mock
   already mirrors config/network APIs; extend it to full parity
   (state/config/time/themes/tpms/sensors/page/logs/logs.csv/media/ota/restart/network)
   so both apps can exercise the LAN HTTP path without a board.
2. **`tools/ble_gauge_sim/`** — a new macOS **CoreBluetooth peripheral
   simulator** advertising the exact GATT spec (service UUID, characteristics,
   envelope + 480 B limit + 413, ~1 Hz Status, Log window, pairing stub). This
   requires Xcode; that install is pending (see below). Both apps run their BLE
   paths against it.

## Open questions

None blocking. The only pending item is the **Xcode install** on the dev machine,
which blocks the macOS CoreBluetooth simulator (`tools/ble_gauge_sim/`) and any
iOS build until it lands.
