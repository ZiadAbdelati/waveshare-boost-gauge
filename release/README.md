# v1.1.3 — the OBD2 switch reconnects when you turn it back on

Firmware 1.1.3 (ESP-IDF 5.5.1) · iOS 1.1.3 (12) · Android 1.1.3 (versionCode 14,
Android 10+). **Firmware-only fix** for `main/boost_obd_ble.c`; the app sources are unchanged, so
the IPA/APK are rebuilt only because the version surfaces move together.

> **Verification honesty: the firmware in this release is host-built and host-tested, NOT
> hardware-verified.** No board was attached. The defect was reported on glass and the fix has not
> been there — the acceptance step is the reported scenario itself (drive with the OBD2 link up,
> toggle OBD2 BLE OFF then ON from the settings overlay, confirm the link comes back; the OBD card
> should return to a live link within ~10 s, which is the driver's own post-disconnect delay).
> The fix is proven off-glass by a new fake-NimBLE harness that drives the **real**
> `main/boost_obd_ble.c` (`tools/tests/test_obd_ble_lifecycle.py` → `tools/test_obd_ble_lifecycle.c`,
> 5 assertions): against the unfixed source invariants 2, 3 and 4 fail with the report's serial
> signature (`connect failed: status=0x000e` repeating), and each half of the fix was doctored in
> isolation to show its own guard fails without it. Host suite 16/16. Also unmeasured: everything
> v1.1.1 and v1.1.2 left open — the rotated panel's visible-window insets and touch alignment at
> 90/180/270, cadence/tear under rotation, the forced-clock RTC write, and the v1.1.2 overlay's
> flick classification against a real CST9217 tap. `v0.9.7` is still the last hardware-verified
> release — quote it (or a fresh board run) for physical measurements.

## What this changes

- **Toggling the OBD2 link off and on again left it unable to reconnect until a reboot.** NimBLE
  answers `ble_gap_connect()` with `BLE_HS_EDONE` for a peer it already holds a connection to, and
  that never clears by itself. The driver had lost track of that connection because it discarded
  the `CONNECT` event that completed while the link was disabled — NimBLE does no cleanup for a
  CONNECT event the application ignores — and `boost_obd_ble_stop()` can only tear down the handle
  it remembers (`ble_gap_conn_cancel()` returns `EALREADY` and never touches an established link).
  So a link that landed in the disabled window survived as an unfindable phantom, every later
  connect attempt was refused with EDONE, and the retry loop could never succeed. Each toggle also
  leaked one connection out of the 3-connection pool.
- **`OBD_EV_CONNECTED` now records the handle before it consults the enabled flag**, and tears the
  link down if it landed in the disabled window — a disable can no longer leave a live link behind.
- **`try_connect()` now reads `EDONE` as "already connected"**: it adopts the existing link via
  `ble_gap_conn_find_by_addr()` (public API, and exactly the comparator that produced the EDONE)
  instead of queueing a failure that retries forever.
- Everything from v1.1.2 is included: the settings overlay's gesture state machine (a drag is never
  a tap; one gesture = one touch-down), no vertical theme change on the overlay, and a BLE toggle
  that repaints its own square.

## Version hygiene

The version has exactly ONE source, the repo-root `version.txt` (bare `MAJOR.MINOR.PATCH`); ESP-IDF
reads it at build time so the firmware image cannot disagree with its tag, and the app versions, the
docs that state the current release, the app-image descriptor, the merged image and `SHA256SUMS` are
all checked against it by `tools/tests/test_version_consistency.py` — source mode on every commit,
`--release` (artifact mode) before publishing. The artifacts here were built from this release
commit; `SHA256SUMS` matches the bytes on disk and covers every shipped file.

## Firmware binaries

`boost_gauge.bin` (**2,682,976** bytes, `0x28f060`; 36 % of `ota_0` free), `bootloader.bin`,
`partition-table.bin`, `ota_data_initial.bin` and `boost_gauge_merged.bin` (**2,814,048** bytes,
`0x2af060`) are fresh 1.1.3 builds. The descriptor version was read back from the shipped bytes:
`1.1.3` at offset `0x30` in the app image and `0x20030` in the merged image. The merged image is
exactly `0x20000` bytes larger than the app image and its four slices (`0x0`, `0x8000`, `0xf000`,
`0x20000`) were verified byte-identical to the individual partition files. The bootloader, partition
table and `ota_data` are **byte-identical to v1.1.1 and v1.1.2** (same checksums), as nothing in
their inputs changed.

This is an OTA-capable app-image release: web OTA uses `boost_gauge.bin` (offset `0x20000`);
`boost_gauge_merged.bin` is for a full-flash reset (offset `0x0`). The display geometry (rotation 0),
cadence, media store, WebSocket and `/time` paths are unchanged; this change-set touches only the
OBD2 BLE central's enable/disable lifecycle.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions (unchanged) |
| `flash.sh` + `flash_args` | one-command full flash helper (`./flash.sh /dev/ttyACM0`) |
| `BoostGauge-android-debug.apk` | Android 1.1.3 (versionCode 14, minSdk 29 — installs on Android 10+ head units), debug build |
| `BoostGauge-1.1.3-ios.ipa` | iOS 1.1.3 (build 12), unsigned sideload IPA (arm64 device build) |
| `BoostGauge-ios-app.zip` | the same `.app` at the zip root, for `devicectl` install |
| `SHA256SUMS` | checksums for every file above |

The IPA is **unsigned** — no provisioning profile for this bundle id is installed on the build
machine, so the Release archive is built with `CODE_SIGNING_ALLOWED=NO`; sideloading tools re-sign it
on install. The Android APK is a debug build, as in previous releases. Both app artifacts were
rebuilt from this commit even though no app source changed, so every shipped file comes from the
same tree.

## Verify

- Gate: `python3 tools/tests/test_version_consistency.py --release` → every shipped artifact reports
  1.1.3 and `SHA256SUMS` matches the bytes.
- On a board: `GET /api/v1/state` must report `"firmwareVersion":"1.1.3"`.
- Apps: iOS About → `1.1.3 (12)`; Android About → `1.1.3 (14)`.
- Host guard: `python3 tools/tests/test_obd_ble_lifecycle.py` → 5 assertions OK.
- **On the glass (the acceptance step for this release):** drive with the OBD2 link up and streaming,
  then toggle the OBD2 BLE switch **OFF then ON** from the two-finger settings overlay's Connections
  page. The link must come back (the app/web OBD card returns to a live link, RPM/coolant/battery
  resume); it must not need a reboot. This is the exact scenario that failed in v1.1.2.
- Still open from v1.1.1/v1.1.2 (unchanged by this release): the rotated panel's insets/touch at
  90/180/270, the forced-clock RTC write, cadence under rotation, the overlay's flick classification
  against a real CST9217 tap, and `tools/check_hardware_gates.py` on the dyno-cell face in demo mode.
