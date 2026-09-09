# v0.9.8 — companion apps: Neon preset fix (firmware unchanged)

Companion-app release. **The firmware binaries in this release are
byte-for-byte identical to v0.9.7** — if your gauge already runs v0.9.7,
you can skip the OTA entirely and only update the phone app. Checksums
below prove the reuse.

## Fixed (companion apps, iOS 0.9.6 (6) / Android 0.9.6 (7))

- **Neon color presets now change the zone colors on the gauge.** The
  apps used to send the previously-loaded palette's vacuum/boost/overboost
  alongside a preset change, and the board applied the preset first and
  then overwrote the zones with those stale colors — so the track color
  updated but everything else didn't. The apps now send a preset change
  as a single-field request (exactly what the web dashboard has always
  sent), and only include color values the user actually edited.
- **Neon preset/layout/font apply immediately.** Each control now takes
  effect when tapped, like the web dashboard — no "Apply" round trip
  needed. The Apply button still saves zone-color edits.
- **iOS: "Reset to default colors" no longer opens the Layout dropdown.**
  The reset button (a default-styled SwiftUI Button inside the theme
  editor list) claimed the whole editor panel as its tap target, so
  tapping it resolved into the list cell and the Layout picker's menu
  opened instead of the reset firing. The button now uses an explicit
  borderless style, and a simulator UI regression test pins the fix.
  (Android's Compose implementation never had this hit-testing behavior;
  verified by code audit on both platforms.)
- **iOS: the spurious "custom" badge / reset button after a bare preset
  change is gone.** The apps no longer send untouched colors, so the
  board no longer reports Neon as customized when only the preset moved.

## Firmware — unchanged from v0.9.7

`boost_gauge.bin`, `boost_gauge_merged.bin`, `bootloader.bin`,
`partition-table.bin`, and `ota_data_initial.bin` are the same verified
v0.9.7 builds (SHA-256 `584ea7a9…4aa110` for the app image, matching the
published v0.9.7 asset byte-for-byte). v0.9.7's notes and its hardware
verification caveat still apply.

## Files

| File | Purpose |
|---|---|
| `bootloader.bin` @ `0x0` | 2nd-stage bootloader (unchanged, v0.9.7) |
| `partition-table.bin` @ `0x8000` | Partition table (unchanged, v0.9.7) |
| `ota_data_initial.bin` @ `0xf000` | OTA data (unchanged, v0.9.7) |
| `boost_gauge.bin` @ `0x20000` | App image — use for **web OTA** (unchanged, v0.9.7) |
| `boost_gauge_merged.bin` @ `0x0` | Full-flash image (unchanged, v0.9.7) |
| `flash.sh` + `flash_args` | Helper to flash the merged image |
| `BoostGauge-android-debug.apk` | Android companion 0.9.6 (versionCode 7) |
| `BoostGauge-ios-app.zip` | iOS companion 0.9.6 (build 6), install via Xcode/devicectl |
| `BoostGauge-0.9.6-ios.ipa` | iOS sideload IPA 0.9.6 (build 6) |
| `SHA256SUMS` | Checksums for everything above |

## Update notes

- **Phone apps:** install the APK (Android) or the IPA / app zip (iOS).
  The app can be updated independently of the firmware; it talks to the
  board over BLE/HTTP exactly as before — no firmware dependency in this
  change.
- **Firmware OTA (only if you are on v0.9.6 or earlier):** serve
  `boost_gauge.bin` to the dashboard's firmware-update control. After the
  reboot, the boot log must read `Loaded app from partition at offset
  0x420000` (ota_1); still booting `0x20000` proves the OTA never ran.
  `/api/v1/state` reports `v0.9.7`.
- **Verification status:** firmware artifacts are the previously
  hardware-verified v0.9.7 binaries. The app fixes were verified with
  full unit suites on both platforms (Android 104/104; iOS 104 executed
  with only the two known pre-existing timezone test failures) plus iOS
  simulator UI regression tests, including a mutation check on the reset
  button fix. No hardware re-verification was needed for app-side code.
