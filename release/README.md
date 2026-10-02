# v1.0.0 — Selectable pressure units (psi / bar / kPa), everywhere

Firmware 1.0.0 (ESP-IDF 5.5.1) · iOS 1.0.0 (8) · Android 1.0.0 (versionCode 10,
Android 10+). The gauge, the dashboard and both companion apps can now show
pressure in **psi**, **bar** or **kPa**, switched from the physical panel, the web
Settings page, or either app.

> **Verification honesty: the firmware in this release is host-built and
> simulator-verified, NOT hardware-verified.** No board was attached when it was
> cut. The unit logic is the same shared source the device runs and it is covered
> by the host suite (13 tests), per-theme stale-pixel audits and simulator renders
> in all three units, and both app artifacts were verified from their built
> binaries - but the on-glass cadence A/B for the converted Big Digit readout and
> the physical feel of the overlay's UNITS button were **not** measured.
> **v0.9.7 remains the last hardware-verified baseline**; quote it (or a fresh
> board run) for physical measurements, never this release. First flash to
> hardware should exercise the UNITS button and soak a bar/kPa Big Digit face.

## What this changes

**Firmware**
- A single global display unit, persisted in the theme store (NVS key `unit`),
  served by `GET /api/v1/themes` as `pressureUnit`, written by
  `PUT /api/v1/themes/config`, and carried in `GET /api/v1/state` so an
  already-open dashboard follows a change made on the panel.
- **Presentation only.** Canonical values stay PSI (`/config`, `/state.psi`,
  `peakPsi`, `/logs.csv`) or kPa (sensor and calibration diagnostics). Gauge
  geometry, arc wedges and the zero marker are always PSI and are never rescaled -
  every psi readout path is byte-for-byte the pre-units render.
- Decimals: psi 1, bar 2, kPa 0. The ±0.1 psi readout dead zone is folded **before**
  conversion; Vault-Tec deliberately stays raw in every unit, and the Neon
  zone-colour decision folds through the same band so engine-off noise no longer
  flips the ring colour between samples.
- Converted readouts use separate advances-centred layouts, and Big Digit's
  bar/kPa value is drawn as one label cell **per character** - a single
  full-width label invalidated its whole 466×120 box on every 0.01-bar step
  (3.56× the psi flush per cycle; now 1.81× bar, 1.11× kPa).
- Converted Big Digit values keep the custom minus widget: `alvida_big` carries no
  `-` glyph, so a literal sign in the string was painted as nothing and `-1.0` psi
  rendered as `0.07` bar.
- TPMS values and the low-pressure threshold follow the unit.

**Physical panel** — the two-finger overlay's OBD2/App rows are replaced by a
**2-up + 1-down cluster of three rounded buttons**: APP BLE, OBD BLE, UNITS. UNITS
cycles psi → bar → kPa and persists. Button presses take part in the existing swipe
classifier, so a drag that starts on a button is still a theme swipe, not a tap.

**Dashboard** — a **Pressure unit** selector on the Settings page (Range section).
The cockpit reads the unit from every `/state` sample.

**Companion apps** — the units dropdown sits on the Range page on both platforms;
the About/Cockpit readouts show the unit.

## Version hygiene (why this release exists as 1.0.0)

The release version now has exactly **one** source: `version.txt`. Previously the
firmware derived its version from `git describe` at build time — which is how the
v0.9.7 release shipped a binary reporting `v0.9.6-3-gee90519` — while the apps
carried hand-edited literals that had already drifted apart (the iOS XcodeGen spec
said 0.9.2 while its own generated project said 0.9.7, so regenerating would have
silently *downgraded* the app).

- ESP-IDF now reads `version.txt` into `PROJECT_VER` →
  `esp_app_desc.version` → `/state.firmwareVersion`.
- iOS takes it from `project.yml`/the generated pbxproj; Android from
  `versionName`; the host simulator and the mock server read the same file.
- `tools/tests/test_version_consistency.py` runs in the host suite and, with
  `--release`, verifies the **shipped bytes**: the `esp_app_desc` inside
  `boost_gauge.bin` and the merged image, the IPA's built `Info.plist`, the APK's
  `versionName`/`versionCode` via `aapt2`, and every `SHA256SUMS` entry against
  the file it names.

**Note on the string format:** `/state.firmwareVersion` now reads `1.0.0`, without
the leading `v` that `git describe` used to produce (`v0.9.9`). That is deliberate —
the canonical file is bare `MAJOR.MINOR.PATCH`, matching the app version strings.

## Firmware binaries — new in this release

`boost_gauge.bin` (SHA-256 `b9758c37…89c2fb2d`), `bootloader.bin`,
`partition-table.bin`, `ota_data_initial.bin` and `boost_gauge_merged.bin` are fresh
1.0.0 builds (`0x28d710` bytes, 36% of the app partition free). This is an
OTA-capable app-image release: web OTA uses `boost_gauge.bin` (offset `0x20000`);
`boost_gauge_merged.bin` is for a full-flash reset.

The display, cadence, media-store and WebSocket paths are unchanged from the
v0.9.7 hardware-verified baseline; this change-set touches the readout/formatting
paths and the two-finger overlay.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions |
| `flash.sh` + `flash_args` | one-command full flash helper (`./flash.sh /dev/ttyACM0`) |
| `BoostGauge-android-debug.apk` | Android 1.0.0 (versionCode 10, minSdk 29 — installs on Android 10+ head units), debug build |
| `BoostGauge-1.0.0-ios.ipa` | iOS 1.0.0 (build 8), unsigned sideload IPA (arm64 device build) |
| `BoostGauge-ios-app.zip` | the same `.app` at the zip root, for `devicectl` install |
| `SHA256SUMS` | checksums for every file above |

The IPA is **unsigned** (no provisioning profile for this bundle id was available:
the only profile on the build machine is an unrelated tvOS one). Sideloading tools
re-sign it on install.

## Verify

- Gate: `python3 tools/tests/test_version_consistency.py --release` → every shipped
  artifact reports 1.0.0 and `SHA256SUMS` matches the bytes.
- On a board: `GET /api/v1/state` must report `"firmwareVersion":"1.0.0"`.
- Apps: iOS About → `1.0.0 (8)`; Android About → `1.0.0 (10)`.
