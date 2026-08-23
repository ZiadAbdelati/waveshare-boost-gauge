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
| Status | `b6a00002-0000-4000-8000-00000000b6a0` | Read, Notify | Plaintext |
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

  Clients should use the Status / Log characteristics or the LAN HTTP path
  instead of requesting oversized data over Control.

### Status

Readable on demand and notified at ~1 Hz while subscribed. Payload is the same
JSON shape as `GET /api/v1/state` with one extra top-level field,
`"transport": "ble"`.

~1 Hz is the nominal companion cadence. This is deliberately **not** the
62.5 Hz WebSocket telemetry (WebSocket remains browser-only), and nothing here
is a demand or throttle change to the 16 ms gauge path.

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

The ring is 18,000 samples (5 Hz, 1 hour, PSRAM, `BOOST_LOG_CAPACITY` 18000).

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

- **LE Secure Connections only**, authenticated (MITM) pairing.
- Passkey entry: the gauge panel displays the 6-digit passkey; the phone
  confirms/enters it. The pairing flow is triggered by the first access to an
  **encrypted** characteristic (Control write or Log read). Reading Status or
  Device info does not trigger pairing.
- The bond is persisted on both sides; after the first pairing, reconnects are
  silent.
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
