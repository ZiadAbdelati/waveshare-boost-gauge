# Bluetooth + Android app — plan

**Date:** 2026-07-26
**Status:** proposal, not started

## Why Bluetooth at all

The dashboard already works over Wi-Fi, so the case for BLE is not "another
transport" — it is the *car* case specifically:

- In the driveway the gauge joins the home LAN and everything works.
- On the road there is no LAN. The gauge falls back to SoftAP, and a phone
  joining a SoftAP with no internet gets nagged by Android, may auto-drop the
  network, and loses cellular data while connected.

BLE fixes exactly that: the phone talks to the gauge **while keeping its data
connection**. That is the whole justification. If the app were only ever used in
the driveway, Wi-Fi would already be the better answer.

## Internal RAM — MEASURED, no longer a go/no-go

**Superseded by hardware measurement.** See
[docs/research/2026-07-26-ble-wifi-coexistence.md](../research/2026-07-26-ble-wifi-coexistence.md).

An earlier draft of this plan called BLE+Wi-Fi coexistence a "high risk,
go/no-go" gate. **That framing was wrong.** BLE advertising ran alongside Wi-Fi
STA + SoftAP + HTTP + three WebSocket clients + the display on a single clean
boot, and the **display cadence was unaffected** (min 57–58 / median 60, versus
a min 57–58 / median 60 baseline).

What the spike actually found:

| Internal free at peak | bytes |
|---|---:|
| `main` as shipped | **13,611** (largest block 7,680) |
| Same, with `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` | 114,023 |
| …plus NimBLE advertising | **77,703** |

NimBLE costs **36,320 B** at runtime. The thing actually occupying internal DRAM
is **`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`, at 82,812 bytes** — roughly 2.3×
NimBLE's entire runtime cost. So the budget question was never "does the radio
fit", it was "is that LVGL IRAM option worth 82 KB", which is a tuning decision
with a measurable price.

The ledger's boot-loop row **does** generalise, and the "it was only about the
GIF decoder" objection does not hold: bringing BLE up *before* Wi-Fi reproduced
it byte-for-byte — `wifi:alloc eb len=752 type=4 fail`, `LoadProhibited`,
backtrace through `ieee80211_hostap_attach ← wifi_softap_start`, 8 boot loops in
32 s. It is the generic signature of internal-DRAM exhaustion. Ordering matters:
bring the radio up first so a shortfall surfaces as a NimBLE init failure with
the control plane already reachable, not as an unrecoverable boot loop.

**The real cost of BLE is HTTP latency**, not memory and not cadence:
`/api/v1/state` p50 goes **25 ms → 88–181 ms** while advertising. WebSocket
telemetry is unaffected. Slowing advertising to 1 Hz only partly recovers it.
Whether that is coexistence arbitration or `BT_CTRL_RUN_IN_FLASH_ONLY` (required
to fit) was not separated.

### Still unvalidated

The spike had **no BLE central available** — the host adapter was in a fault
state and no phone was reachable. So:

- Nothing off-device ever saw the advertisement. All evidence is ESP-side.
- **Throughput is entirely unmeasured.** The firmware *requests* a 15–30 ms
  interval, which would support 33–66 Hz, but no connection was ever made. The
  20 Hz design target is an assumption, not a measurement.
- **Cadence with an active connection is untested** — and given the HTTP latency
  result, this is the most important remaining gap.

Validating these needs nothing more than nRF Connect on a phone.

## Safety gate before any of this ships

The gauge is now **installed in the dash with no serial access**. A BLE bug that
faults during init happens *before* the HTTP server starts, which means OTA
cannot recover it — the same failure mode as the RAM boot-loop above. Recovery
would mean pulling the dash.

Two things must be true before risky radio code goes on that board:

1. **OTA rollback must actually be active.** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`
   is set and `app_main` calls `esp_ota_mark_app_valid_cancel_rollback()`, but
   rollback is a *bootloader* feature and this board has only ever been updated
   app-only over OTA. The new bootloader has never been written. **Until one
   full USB flash happens, the rollback net is inert.** That flash is the single
   highest-value thing to do before this project starts.
2. **BLE must be opt-in and default-off**, persisted like `teSync`, so a bad
   build still boots into a working gauge.

## Firmware design

### Transport split

Do not move everything to BLE. Keep each transport where it is strong:

| Concern | Transport | Why |
|---|---|---|
| Live telemetry | **BLE notify** | Small, frequent, wanted while driving |
| Config + commands | **BLE write** | Small, occasional |
| OTA | **Wi-Fi HTTP** | 1.6 MB; BLE would take minutes and adds a risky path |
| Log/CSV bulk download | **Wi-Fi HTTP** | Large transfer, done parked |

### Rate

The physical gauge runs at 62.5 Hz. **BLE should not.** Android typically
negotiates a 30–50 ms connection interval, and one notification per interval is
the realistic ceiling, so plan for **20 Hz** and treat anything better as a
bonus. A phone mirror does not need more, and the existing browser mirror
already smooths with a 35 ms EMA — the same trick applies.

Decimate from the existing sample notification the WebSocket push already uses,
rather than adding a second free-running timer. The ledger is explicit that a
wall-clock gate whose period is not a multiple of 16 ms aliases into a beat.

### GATT

One custom service. Telemetry as a **packed binary struct, not JSON** — at 20 Hz
in a 20-byte default ATT payload, JSON is pure waste.

| Characteristic | Props | Payload |
|---|---|---|
| Telemetry | notify | `int16 psi_cx100`, `int16 peak_cx100`, `uint16 map_kpa_x100`, `uint16 ambient_kpa_x100`, `uint8 flags` (demo/fault/ads/bmp), `uint8 zone` — 10 bytes |
| Sensor detail | read, notify | freshness ages, recoveries, calibration offset — the `/sensors/calibration` content |
| Config | read, write | gauge range, theme id, brightness, rotation |
| Command | write | reset peak, calibrate-to-atmosphere, restart |
| Device info | read | firmware version (from `esp_app_get_description()`) |

Request an MTU bump on connect so the sensor-detail characteristic does not need
fragmenting; fall back gracefully when the phone refuses.

### Pairing

The gauge can trigger `restart` and change calibration, so "anyone in range can
connect" is not acceptable. **This device has a screen** — use LE Secure
Connections with a passkey displayed on the panel. That is a genuinely good fit
and much better UX than a fixed PIN in a README. Bond after first pairing.

## Android app

- **Kotlin + Jetpack Compose**, min SDK 26.
- Android 12+ permissions: `BLUETOOTH_SCAN` / `BLUETOOTH_CONNECT` with
  `neverForLocation` so the app does not have to ask for location.
- Architecture: GATT callbacks → repository → `StateFlow` → Compose. Keep the
  BLE layer behind an interface so it can be faked for UI work without hardware.

### Rendering the faces

Two options:

1. **WebView reusing `web/app.js`** — maximum reuse, but janky at 60 FPS, awkward
   offline, and drags the whole dashboard bootstrap along.
2. **Native Compose Canvas** — more work up front, smooth, and the existing
   `drawArcGauge` / `drawVaultGauge` / `drawNightCity` / `drawBigDigit`
   functions are an exact, already-debugged geometric spec to port from.

**Recommend native Compose Canvas.** The web renderers become the specification
rather than the implementation — which is how the firmware faces were built too,
so there is precedent for keeping three renderers in agreement.

Note the cost honestly: this makes a **third** renderer to keep in sync
(firmware, web, Android). Any face change becomes three edits. That is a real
ongoing tax and worth accepting deliberately, not by accident.

## Phasing

Each phase is independently useful and independently abandonable.

| Phase | Deliverable | Gate |
|---|---|---|
| **0** | RAM/coexistence spike: NimBLE advertising + SoftAP + display DMA together | Free internal RAM at peak with a hard reserve; cadence guard still min 57 / median 60 on arc + demo. **Go/no-go.** |
| **1** | Firmware GATT service, telemetry notify only, opt-in and default-off | Verified with nRF Connect — no app needed |
| **2** | Android skeleton: scan, connect, subscribe, numeric PSI readout | Live number tracks the panel |
| **3** | Port the Dyno Cell arc face to Compose Canvas | Visually matches the web mirror side by side |
| **4** | Config + commands over BLE (reset peak, calibrate, range) | Calibration from the phone with the engine off |
| **5** | Remaining three faces, session logging, CSV export | — |
| **6** | *Optional:* BLE OTA | Only if Wi-Fi OTA proves impractical in the car |

## Risks

| Risk | Severity | Mitigation |
|---|---|---|
| BLE + Wi-Fi exceed internal RAM | **High** — precedent for a boot loop | Phase 0 gate; NimBLE only; mode-switch fallback |
| Radio coexistence degrades display cadence | Medium | Re-run the cadence guard per phase; it is a documented gate already |
| A boot-time BLE fault bricks a dash-installed board | **High** | Full USB flash to activate rollback *first*; opt-in default-off |
| Android BLE fragmentation (MTU, intervals, vendor quirks) | Medium | Test on more than one handset; never assume a negotiated MTU |
| Three renderers drift apart | Medium | Accept deliberately; screenshot comparison per face |

## Open questions

1. **Is the app for driving or for tuning?** A glanceable driving mirror and a
   data-logging tuning tool are different products. This plan assumes the mirror
   first, logging later.
2. **Should the app also speak HTTP when on the same Wi-Fi?** It would give OTA
   and fast bulk transfer in one app, at the cost of two transports to maintain.
3. **iOS?** Nothing here is Android-specific except the app layer, but it doubles
   the client work.
