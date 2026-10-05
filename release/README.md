# v1.1.0 — Pressure display reference (Relative / Absolute) + a three-page settings overlay

Firmware 1.1.0 (ESP-IDF 5.5.1) · iOS 1.1.0 (9) · Android 1.1.0 (versionCode 11,
Android 10+). The gauge, the dashboard and both companion apps can now show pressure
either **Relative** (gauge — atmosphere reads 0, as a boost gauge normally does) or
**Absolute** (atmosphere included), independently of the selected unit (psi / bar /
kPa). The physical two-finger settings overlay also grew from two pages to three.

> **Verification honesty: the firmware in this release is host-built and
> simulator-verified, NOT hardware-verified.** No board was attached when it was cut.
> The reference logic is the same shared source the device runs and it is covered by
> the host suite (15 tests), a renderer call-site guard, per-theme stale-pixel audits
> and simulator renders in all three units and both reference modes; both app
> artifacts were verified from their built metadata (`versionName`/`versionCode`,
> the IPA's `Info.plist`). On-glass cadence, overlay paging and the reference/unit
> cycle remain **unmeasured**. `v0.9.7` is still the last hardware-verified release —
> quote it (or a fresh board run) for physical measurements.

## What this changes

- **Relative / Absolute display reference.** Presentation-only, exactly like the
  unit: `/state.psi`, the `/logs` JSON and `/logs.csv` columns, TPMS comparisons,
  calibration, needle position, arc geometry and the zone-colour decision all stay
  gauge psi on the wire. The ±0.1 psi display dead band runs FIRST, in gauge psi, then
  the reference is applied, then the unit conversion. **Relative mode is a
  byte-for-byte passthrough of the previous renderer** (measured: 35/35 screenshot
  frames across all five themes against a side-by-side pre-change build), and the psi
  path remains the 60 FPS reference. Absolute mode adds the BMP280 ambient
  (`absolute = gauge + ambient`), falling back to the standard atmosphere when the
  sensor is absent; the dial numerals bake the reference at scene build, when the
  scene is rebuilt on a mode or unit change.
- **Three-page physical overlay.** QR → Connections → Units, with a three-dot
  indicator and wrap-around horizontal paging. The Units page carries the unit cycler
  and the **`REL/ABS`** reference toggle, whose state line reads the full word
  (`RELATIVE` / `ABSOLUTE`); the square lights only in absolute mode.
- **Terminology.** The mode pair was renamed from "Atmospheric / Absolute" to
  **Relative / Absolute** — the standard pair for this axis is relative (or gauge) vs
  absolute pressure (`psig`/`psia`), and "atmospheric" names the reference *value*, not
  the mode. The rename covers the panel button, the web option and its hint/save text,
  both app pickers and the sim panel. `atmosphere` remains only where it names the
  baseline value.
- **Vault-Tec + bar.** The readout box is centred on the face, so bar now anchors its
  decimal **point** on the box centre (the web mirror always drew it there); psi keeps
  its historic six-slot field and kPa its own anchor.
- **Pre-release review round.** Four independent read-only reviews (firmware, clients,
  harness, release readiness) found and fixed two real defects — the arc PEAK label
  cached on the peak alone and so went stale as the ambient drifted, and the neon
  readout applied the dead band a second time after the reference — plus a web
  dial-tick rounding mismatch, a canonical theme-preview ambient gap, and several
  tests that could not fail. Details in `docs/regression-ledger.md`.

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

`boost_gauge.bin` (2,681,056 bytes), `bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin` and `boost_gauge_merged.bin` (2,812,128 bytes) are fresh 1.1.0
builds. The app image is `0x28e8e0` bytes — 36 % of `ota_0` free, 13 KB above the
v1.0.0 image, consistent with one settings module, the reference module and the third
overlay page.

This is an OTA-capable app-image release: web OTA uses `boost_gauge.bin` (offset
`0x20000`); `boost_gauge_merged.bin` is for a full-flash reset (offset `0x0`). Both
images carry `esp_app_desc.version = 1.1.0` (verified at offset `0x20` in the app
image and `0x20020` in the merged image, i.e. the `ota_0` offset).

The display, cadence, media-store and WebSocket paths are unchanged from the v0.9.7
hardware-verified baseline; this change-set touches the readout/reference formatting
paths, the two-finger overlay and the theme-preview payloads.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions |
| `flash.sh` + `flash_args` | one-command full flash helper (`./flash.sh /dev/ttyACM0`) |
| `BoostGauge-android-debug.apk` | Android 1.1.0 (versionCode 11, minSdk 29 — installs on Android 10+ head units), debug build |
| `BoostGauge-1.1.0-ios.ipa` | iOS 1.1.0 (build 9), unsigned sideload IPA (arm64 device build) |
| `BoostGauge-ios-app.zip` | the same `.app` at the zip root, for `devicectl` install |
| `SHA256SUMS` | checksums for every file above |

The IPA is **unsigned** — no provisioning profile for this bundle id is installed on
the build machine, so the Release archive is built with `CODE_SIGNING_ALLOWED=NO`.
Sideloading tools re-sign it on install. The Android APK is a debug build, as in
previous releases.

## Verify

- Gate: `python3 tools/tests/test_version_consistency.py --release` → every shipped
  artifact reports 1.1.0 and `SHA256SUMS` matches the bytes.
- On a board: `GET /api/v1/state` must report `"firmwareVersion":"1.1.0"` and carry
  `pressureAbsolute` beside `pressureUnit`.
- Apps: iOS About → `1.1.0 (9)`; Android About → `1.1.0 (11)`.
- On the glass (still to do): open the two-finger overlay and step all three pages
  with wrap-around, toggle `REL/ABS` and confirm the state line and the lit square,
  then cycle psi/bar/kPa in absolute mode and confirm the readouts equal
  `gauge + ambient` with the right decimals (psi 1, bar 2, kPa 0) — and re-run
  `tools/check_hardware_gates.py` on the dyno-cell face in demo mode.
