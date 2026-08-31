# Prebuilt firmware v0.9.4 — theme preview freeze fix

Firmware **`v0.9.4`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines (apps-only release; firmware image rebuilt from the tag so
`/state` reports the matching version): the **iOS Themes preview no longer
freezes on the old theme**. On 0.9.3 a suspended WebContent process
(backgrounding the app) left the preview frozen for minutes and surviving
restart, even though every tap switched the physical gauge. Root-caused with
four deterministically reproduced paths and all fixed: foreground re-arm of
the preview mirror (H3), reconcile GET after a lost switch echo (H2), resync
fetch failures now surface instead of silently re-applying a stale disk
cache (H1), and stale list responses can no longer clobber a newer
selection (H4). The Android app was audited for the same defect classes: the
lost-echo and stale-clobber paths existed and are fixed identically; the
other two are structurally impossible there (documented in the ledger).
Companion apps: iOS **0.9.4 (build 4)**, Android **0.9.4 (versionCode 5)**.
Carries v0.9.3's supply-save/keyboard fixes and v0.9.2's BLE HTTP mirror.
The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time. This release was tagged first, then
reconfigured/built/flash-verified from the tagged tree, so the board reports a
clean `v0.9.4` (hardware-verified: `/state` returns `v0.9.4`, clean boot,
HTTP 200, cadence gate median 60 FPS, host suite 11/11; user confirmed the
preview recovers after backgrounding).

## Files

| File | Purpose |
|---|---|
| `bootloader.bin` @ `0x0` | 2nd-stage bootloader |
| `partition-table.bin` @ `0x11000` | Partition table |
| `ota_data_initial.bin` @ `0x12000` | OTA data (boots ota_0) |
| `boost_gauge.bin` @ `0x20000` | App image — use for **web OTA** |
| `boost_gauge_merged.bin` @ `0x0` | Full-flash image for a complete reset |
| `flash.sh` + `flash_args` | Helper to flash the merged image |
| `BoostGauge-android-debug.apk` | Android companion (debug-signed, versionCode 5) |
| `BoostGauge-ios-app.zip` | iOS companion `.app` bundle (install via Xcode/devicectl) |
| `SHA256SUMS` | Checksums for everything above |

## What changed since v0.9.1

### Apps (v0.9.4)
- **iOS: theme preview freeze fixed (four reproduced paths).** A suspended
  WebContent process (backgrounded app) left the preview frozen for minutes,
  surviving restart, while the gauge itself switched on every tap. Fixes:
  scenePhase-active re-arm of the preview mirror's rescue renders; reconcile
  GET after a lost switch echo (error clears only if the board took the
  requested theme); resync fetch failures surface instead of silently
  re-applying a stale disk cache; stale list responses can no longer clobber
  a newer selection. iOS suite 101 tests (2 pre-existing timezone failures);
  hardware-confirmed on device.
- **Android: parity audit.** Lost-echo reconcile and stale-clobber guard
  fixed identically (requestSeq); no theme cache and no freeze mechanism
  exist on Android (documented). 99 unit tests green.


### Firmware (v0.9.3, carried)
- **BLE `/sensors/supply` PUT is driver-task dispatched** (`APP_EV_SUPPLY`,
  heap request/response buffers). The inline route-table entry shipped
  2026-08-30 blocked the NimBLE host loop on an NVS commit — phone saves
  timed out and BLE links wedged. The first refactor put a 4 KB response body
  on the driver-task stack and boot-looped the board (stack overflow,
  serial-captured); heap buffers only, like every other blocking route.
- **Serial instrumentation** for every Control request/response
  (`control req id method path`, `supply ev fired`, `tx resp len`) — kept
  permanently; it localized the app-side save failure in one capture.

### iOS companion (v0.9.3, carried)
- **Calibration Save works again.** This round's keyboard-dismissal machinery
  (SwiftUI tap gestures + `.immediately` scroll dismissal) ate the Save
  button's tap — serial capture showed zero BLE traffic per tap. Deleted; the
  view now matches the always-working plain-Form Settings pattern.
- **Keyboard UX:** Done button on the keyboard accessory bar, drag-down
  dismiss, and tap-outside dismiss via a window-level recognizer with
  `cancelsTouchesInView = false` (fires alongside button taps, never cancels
  them — simulator-harness verified: Save still fires, other rows dismiss).
- **Toast visibility:** Save drops focus before sending, so the bottom toast
  is no longer hidden behind the keyboard. The no-transport guard now toasts
  instead of returning silently.
- **Logs graph survives a failed refresh:** a background revalidation failure
  keeps displayed samples and surfaces a note instead of wiping the graph.

### Android companion (v0.9.3, carried)
- Save/status feedback moved to M3 snackbars (Settings' existing pattern);
  supply field gains IME Done + outside-tap focus clear. Logs failure path
  verified to already keep the displayed graph.

### v0.9.2 (carried)
- **BLE `/logs` route is real.** Compact `{"tMs","psi"}` samples, ~22 B each,
  128 points (cap/32) decimated evenly across the requested `?limit=` — the
  verbose 42-point window aliased the demo sweep into staircase pulses.
- **BLE `/network*` routes.** Settings/Wi-Fi pages in the apps now reflect the
  real STA state (SSID, IP, RSSI, saved networks) and can join/remove networks
  over Bluetooth. Wi-Fi scan runs on the driver task (`APP_EV_SCAN`), never on
  the NimBLE host task.
- **Blocking routes are async.** `/logs` (`APP_EV_LOGS`), `/network/scan`
  (`APP_EV_SCAN`), and `/sensors/calibration` all run on the driver task with
  heap response buffers; the host event loop never blocks.
- **OBD scan coexistence.** The OBD central's retry loop defers new 3 s scan
  bursts while a companion client is connected (they starved the peripheral's
  notification path mid-fragment). Stored-peer connects and real scan hits are
  not gated, so the in-car adapter link is unaffected.
- `APP_BLE_CTRL_RESP_MAX` stays **4096** — an 8 KB bump panicked twice under
  Wi-Fi + LVGL DMA pressure and is not measured-safe.

### iOS companion (v0.9.2, carried)
- **CCCD self-healing:** Control notifications are re-subscribed on every
  write/timeout failure. A board reboot resets the gauge's subscription state;
  CoreBluetooth never re-issues `setNotifyValue` on auto-reconnect, so
  responses silently vanished — the first request after a gauge reboot now
  heals it (one timeout, then clean).
- **Per-call timeouts:** `/logs` uses a single 20 s attempt + one retry instead
  of the 10 s × 4-retry cascade that re-sent requests whose late responses fed
  a dead pending.
- **Instant window switching:** the Logs view publishes the cached 1m/5m/15m
  window before fetching (stale-while-revalidate).
- **Theme preview updates on selection** (reveal-gate race fixed: explicit
  theme-ID change signal + rescue re-renders re-armed per switch).
- Toasts: one reusable window, bottom capsule, no resurfacing on tab switches;
  timezone selection is local-only until you tap "Sync timezone to gauge"
  (single toast).

### Android companion (v0.9.2, carried)
- **Stale-bond recovery:** verifies the encrypted link before writing the CCCD;
  on failure removes the stale bond, re-bonds fresh, and retries once. Fixes
  the endless `encryption changed status 13` reconnect loop after reflashing
  the gauge.
- Timezone: selection is local-only; sync sends the selected zone (not a
  phone-derived string) and shows one toast.

## Hardware verification (this release)

- **Theme preview recovery (v0.9.4):** user-confirmed on iPhone — switching
  themes updates the preview, and backgrounding/reopening the app no longer
  leaves it frozen.
- Firmware v0.9.4 flashed from the tag: `/state` reports `v0.9.4`; clean
  boot (no panics), HTTP 200; cadence gate median 60 FPS (demo + fast sweep
  on dyno-cell, restored off after); host suite 11/11.

### v0.9.3 (carried)
- **Supply save round trip over BLE:** phone Save → `control req
  id=16 PUT /sensors/supply` → `MAP supply set to 5.00 V` (NVS) → `tx resp
  len=449` fragmented to the phone; board confirmed at 5.00 V via HTTP. The
  zero-traffic failure mode (button tap eaten) was captured and fixed in the
  same session.
- Web UI supply save verified in a real browser: edit → debounced PUT →
  "MAP supply 5.10 V saved" banner → board value confirmed → restored.
- iOS (physical iPhone, pure BLE, v0.9.2-era matrix re-verified on the
  release build): Logs window cycle 5m → 1m → 15m → 5m, three consecutive
  passes — `Last X · 128 samples` each, link connected throughout; repeated
  with the OBD2 BLE link enabled after the coexistence fix. Firmware serial:
  every `/logs` built in 3.5–5 ms.
- All five themes cycled over BLE from the app: preview updated each time, no
  reboot, no disconnect.
- Wi-Fi scan over BLE returned live AP rows; Settings shows real STA state.
- Android (physical Pixel): stale-bond recovery verified — `removeBond` →
  fresh pairing → `encryption changed status 0` + continuous 1 Hz state writes
  ≥60 s; 92/92 unit tests.
- Host test suite 11/11; iOS suite 96 tests (only the two pre-existing
  SettingsViewModel timezone failures); Android unit tests green.

## Install

- **Full flash (fresh board / reset):** `./flash.sh /dev/cu.usbmodem*`
  (writes the merged image at `0x0`).
- **OTA (web UI → Settings):** upload `boost_gauge.bin`.
- **Android:** install the APK (allows "unknown developer" — it's the debug key).
- **iOS:** unzip and install the `.app` via Xcode's Devices window or
  `xcrun devicectl device install app`.
