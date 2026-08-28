# Prebuilt firmware v0.9.2 — companion BLE hardening

Firmware **`v0.9.2`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines: the **BLE Control surface is now a real HTTP mirror** —
`/logs` returns a bounded compact window (128 points, decimated evenly across
the requested window so the 9.789 psi/s demo sweep draws triangles, not
staircases), and `/network` GET/PUT/DELETE + `/network/scan` +
`/network/reconnect` are served over Bluetooth, so the companion apps' Settings
and Wi-Fi pages work without joining the gauge's Wi-Fi. Blocking routes (logs
build, Wi-Fi scan, calibration) run on the driver task with heap buffers — the
NimBLE host loop is never stalled. The **OBD2 central defers its scan bursts
while a phone holds the companion link**, so enabling TPMS/OBD2 no longer
starves Logs. Companion apps gained **link self-healing** (iOS re-subscribes
notifications after a board reboot; Android detects and re-bonds a stale bond
left by reflashes), **instant log-window switching** (stale-while-revalidate),
and a **theme preview that actually updates** on selection. Carries v0.9.1's
Display/Demo reorg and v0.9.0's DMA-safe AMOLED path, DS3231/DST clock, and
calibrated GM 12223861 MAP path. The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`), taken
from `git describe` at build time. This release was tagged first, then
reconfigured/built/flash-verified from the tagged tree, so the board reports a
clean `v0.9.2` (serial-verified: UI ready, brightness, HTTP API ready, BLE
advertising, zero panics).

## Files

| File | Purpose |
|---|---|
| `bootloader.bin` @ `0x0` | 2nd-stage bootloader |
| `partition-table.bin` @ `0x11000` | Partition table |
| `ota_data_initial.bin` @ `0x12000` | OTA data (boots ota_0) |
| `boost_gauge.bin` @ `0x20000` | App image — use for **web OTA** |
| `boost_gauge_merged.bin` @ `0x0` | Full-flash image for a complete reset |
| `flash.sh` + `flash_args` | Helper to flash the merged image |
| `BoostGauge-android-debug.apk` | Android companion (debug-signed, versionCode 3) |
| `BoostGauge-ios-app.zip` | iOS companion `.app` bundle (install via Xcode/devicectl) |
| `SHA256SUMS` | Checksums for everything above |

## What changed since v0.9.1

### Firmware
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

### iOS companion
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

### Android companion
- **Stale-bond recovery:** verifies the encrypted link before writing the CCCD;
  on failure removes the stale bond, re-bonds fresh, and retries once. Fixes
  the endless `encryption changed status 13` reconnect loop after reflashing
  the gauge.
- Timezone: selection is local-only; sync sends the selected zone (not a
  phone-derived string) and shows one toast.

## Hardware verification (this release)

- iOS (physical iPhone, pure BLE): Logs window cycle 5m → 1m → 15m → 5m, three
  consecutive passes — `Last X · 128 samples` each, link connected throughout;
  repeated with the OBD2 BLE link enabled after the coexistence fix. Firmware
  serial: every `/logs` built in 3.5–5 ms.
- All five themes cycled over BLE from the app: preview updated each time, no
  reboot, no disconnect.
- Wi-Fi scan over BLE returned live AP rows; Settings shows real STA state.
- Android (physical Pixel): stale-bond recovery verified — `removeBond` →
  fresh pairing → `encryption changed status 0` + continuous 1 Hz state writes
  ≥60 s; 92/92 unit tests.
- Host test suite 11/11.

## Install

- **Full flash (fresh board / reset):** `./flash.sh /dev/cu.usbmodem*`
  (writes the merged image at `0x0`).
- **OTA (web UI → Settings):** upload `boost_gauge.bin`.
- **Android:** install the APK (allows "unknown developer" — it's the debug key).
- **iOS:** unzip and install the `.app` via Xcode's Devices window or
  `xcrun devicectl device install app`.
