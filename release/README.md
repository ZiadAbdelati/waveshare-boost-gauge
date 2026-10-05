# v1.1.1 — panel rotation actually applied, the timezone sync kept alive, and a Force clock recovery path

Firmware 1.1.1 (ESP-IDF 5.5.1) · iOS 1.1.1 (10) · Android 1.1.1 (versionCode 12,
Android 10+). Three fixes to behaviour that shipped inert in v1.1.0, plus the recovery
path the first two make necessary.

> **Verification honesty: the firmware in this release is host-built and
> simulator-verified, NOT hardware-verified.** No board was attached when it was cut.
> The release was reviewed pre-merge and one blocking finding was fixed and
> re-verified (the panel-touch guard modelled `esp_lcd_touch`'s flag order as
> swap-first; the driver mirrors both axes first). The host suite is 15/15, the
> HTTP/BLE contract suites are 239/239 and 63/63, and the panel-orientation tap guard
> passes 34/34 including two doctored variants that prove it can fail. Both app
> artifacts were verified from their built metadata (`versionName`/`versionCode`, the
> IPA's `Info.plist`). **Unmeasured on glass:** the rotated panel's visible-window
> insets at 90/180/270, touch alignment under rotation, cadence/tear under rotation,
> and the forced-path serial line with the OSF-clearing RTC write. `v0.9.7` is still
> the last hardware-verified release — quote it (or a fresh board run) for physical
> measurements.

## What this changes

- **Panel rotation was a dead setting.** The persisted quarter turn was handed to the
  LVGL adapter, which refuses to rotate a `PANEL_IF_OTHER` display (QSPI/CO5300) — it
  logs "SPI rotation is not handled by adapter" and drops the value — so the setting
  saved, reported `restartRequired`, and did nothing. The turn is now applied to the
  CO5300's own scan order at init (`esp_lcd_panel_swap_xy` / `esp_lcd_panel_mirror`
  plus the visible-window gap), after `esp_lcd_panel_init()` and before the display
  registers, with the matching CST9217 touch flags. **Rotation 0 is byte-identical to
  the previous build** (gap `(6, 0)`, the vendor touch flags). The touch mapping was
  corrected in pre-merge review and **has not been confirmed on glass in either
  direction**; the boot log now prints the applied orientation and gap.
- **The companion apps' timezone sync could be rejected outright.** Both apps send the
  phone's epoch as a clock calibration; against a readable DS3231 more than 5 minutes
  away the gauge answers `409 clock_rejected` and writes nothing, so the timezone
  never landed either. Both apps now retry the identical request without `epochMs`, so
  the zone still saves. Over BLE the `/time` route additionally required `epochMs`,
  which turned that retry into a 400 on the apps' primary transport; the route now
  mirrors HTTP field for field, and the GATT guard checks the BLE function body rather
  than only the route table.
- **A force-clock recovery path.** The DS3231 stays the clock authority for ordinary
  syncs, but a genuinely wrong RTC was previously unrecoverable by any client (the
  dashboard's own message said to pull the RTC battery). A confirmed **Force clock**
  action on all three clients (`force: true`) now skips **only** the disagreement
  comparison, still applies the plausibility floor, and still writes the RTC —
  clearing OSF, so the corrected time becomes the new authority. A forced request is
  never auto-retried.
- **A present-but-wrong-typed `/time` field is now a 400** (`invalid_time`,
  `invalid_force`) instead of being silently reinterpreted — the same rule
  `pressureUnit` already followed. A wrong-typed `epochMs` used to downgrade a clock
  sync to a zone-only write.

## Version hygiene

The version has exactly ONE source, the repo-root `version.txt` (bare
`MAJOR.MINOR.PATCH`); ESP-IDF reads it at build time so the firmware image cannot
disagree with its tag, and the app versions, the docs that state the current release,
the app-image descriptor, the merged image and `SHA256SUMS` are all checked against it
by `tools/tests/test_version_consistency.py` — source mode on every commit, and
`--release` (artifact mode) before publishing. The artifacts in this directory were
built from the tagged commit; `SHA256SUMS` matches the bytes on disk and covers every
shipped file.

## Firmware binaries

`boost_gauge.bin` (2,682,656 bytes), `bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin` and `boost_gauge_merged.bin` (2,813,728 bytes) are fresh 1.1.1
builds. The app image is `0x28ef20` bytes — 36 % of `ota_0` free (`0x1710e0`), 1.6 KB
above the v1.1.0 image, consistent with the rotation/touch mapping and the force path.

This is an OTA-capable app-image release: web OTA uses `boost_gauge.bin` (offset
`0x20000`); `boost_gauge_merged.bin` is for a full-flash reset (offset `0x0`). Both
images carry `esp_app_desc.version = 1.1.1` (verified at offset `0x30` in the app
image and `0x20030` in the merged image, i.e. the `ota_0` offset plus the same
descriptor offset), and the merged image's bootloader and app slices were verified
byte-identical to the individual files.

The display panel geometry (rotation 0), cadence, media-store and WebSocket paths are
unchanged from the v0.9.7 hardware-verified baseline; this change-set touches the
panel init sequence, the `/time` contract on both transports and the three clients'
clock UI.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions (unchanged from v1.1.0) |
| `flash.sh` + `flash_args` | one-command full flash helper (`./flash.sh /dev/ttyACM0`) |
| `BoostGauge-android-debug.apk` | Android 1.1.1 (versionCode 12, minSdk 29 — installs on Android 10+ head units), debug build |
| `BoostGauge-1.1.1-ios.ipa` | iOS 1.1.1 (build 10), unsigned sideload IPA (arm64 device build) |
| `BoostGauge-ios-app.zip` | the same `.app` at the zip root, for `devicectl` install |
| `SHA256SUMS` | checksums for every file above |

The IPA is **unsigned** — no provisioning profile for this bundle id is installed on
the build machine, so the Release archive is built with `CODE_SIGNING_ALLOWED=NO`.
Sideloading tools re-sign it on install. The Android APK is a debug build, as in
previous releases.

## Verify

- Gate: `python3 tools/tests/test_version_consistency.py --release` → every shipped
  artifact reports 1.1.1 and `SHA256SUMS` matches the bytes.
- On a board: `GET /api/v1/state` must report `"firmwareVersion":"1.1.1"`.
- Apps: iOS About → `1.1.1 (10)`; Android About → `1.1.1 (12)`.
- On the glass (still to do): set a non-zero rotation, reboot, and confirm the serial
  boot line (`panel up ... orientation N deg (swap=… mx=… my=… gap=…)`) against what
  appears — including that taps land where they are drawn at 90/180/270, since the
  touch mapping is host-only so far. For the clock: sync from a phone whose clock is
  correct and confirm the zone saves through the 409, then use **Force clock** and
  confirm the serial line `clock FORCED from client: overriding DS3231 by N ms` and
  that the corrected time survives a power cycle (the RTC write clears OSF). Re-run
  `tools/check_hardware_gates.py` on the dyno-cell face in demo mode.
