# Prebuilt firmware v0.9.5 — dyno-cell zero dead zone + iOS About self-heal

Firmware **`v0.9.5`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Headlines: the **dyno-cell arc now draws nothing around zero** — the
old 1-degree minimum-sweep stub (which the arc's rounded caps inflated into a
~14-degree blob hugging the notch) is replaced by a true dead zone, so a
reading fluttering around 0 psi no longer flashes. The gap edges are rounded
outward to whole degrees (LVGL truncates arc angles), making the geometry
safe at **any user-configured zero angle**; the zero marker is slightly
heavier (26 px) so the arc still emerges from fully behind it. The web
cockpit mirror renders the identical geometry. Companion apps: iOS **0.9.5
(build 5)** — the About page's Firmware row self-heals (retries the GATT
device-info read and falls back to live `/state` instead of showing "Not
connected" while connected); Android **0.9.5 (versionCode 6)** — audited, no
change needed (already uses the live-poll pattern). Carries v0.9.4's
theme-preview freeze fixes and v0.9.3's supply-save/keyboard fixes.
The same files are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

The image reports its own version on `/api/v1/state` (`firmwareVersion`).
Note: this release was built from the tagged tree but **hardware-verified
lighter than usual** — the board was not on the bench at release time, so the
cadence gate and on-glass screenshot diff are pending the user's OTA. Sim
verification covered ±0.02 psi dead-zone byte-equality vs the atmo state and
clean emergence at both zero=236.25 and zero=220; host suite 11/11.

## Files

| File | Purpose |
|---|---|
| `bootloader.bin` @ `0x0` | 2nd-stage bootloader |
| `partition-table.bin` @ `0x11000` | Partition table |
| `ota_data_initial.bin` @ `0x12000` | OTA data (boots ota_0) |
| `boost_gauge.bin` @ `0x20000` | App image — use for **web OTA** |
| `boost_gauge_merged.bin` @ `0x0` | Full-flash image for a complete reset |
| `flash.sh` + `flash_args` | Helper to flash the merged image |
| `BoostGauge-android-debug.apk` | Android companion (debug-signed, versionCode 6) |
| `BoostGauge-ios-app.zip` | iOS companion `.app` bundle (install via Xcode/devicectl) |
| `BoostGauge-0.9.5-ios.ipa` | iOS sideload IPA (AltStore/Sideloadly/etc.) |
| `SHA256SUMS` | Checksums for everything above |

## OTA update path

Serve `boost_gauge.bin` to the dashboard's firmware-update control, or flash
`boost_gauge_merged.bin` over USB with `flash.sh` for a complete reset. After
an OTA, the boot log must read `Loaded app from partition at offset 0x420000`
(ota_1); still booting `0x20000` proves the OTA never ran.

## Changelog

### Firmware (v0.9.5)
- **dyno-cell zero dead zone** — value arc draws nothing until the reading's
  own angle clears the gap edge; flutter around 0 psi no longer flashes a
  blob against the zero notch (user-reported, sim-reproduced).
- **Zero-adaptive gap edges** — `value_arc_angles()` rounds each gap edge
  outward (ceil boost start / floor vacuum end) so the effective gap is never
  smaller than nominal at any `zeroAngle` setting; fractional zero angles
  (e.g. 236.25) previously truncated asymmetrically.
- Zero marker 20 → 26 px (±3.2° at the 231 px centre radius) keeping the
  rounded cap fully hidden at emergence; web mirror in lockstep, embedded
  assets regenerated.

### iOS (0.9.5, build 5)
- About page Firmware row self-heals: retries the GATT device-info read when
  `bleInfo` is nil and falls back to live `/state`, instead of a permanent
  "Not connected" after a failed connect-time read.

### Android (0.9.5, versionCode 6)
- Version bump only — the About/Firmware row already reads the live `/state`
  poll.
