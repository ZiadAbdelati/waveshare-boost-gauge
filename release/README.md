# Firmware v0.9.6 — OTA rollback hotfix

Firmware **`v0.9.6`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. **Everyone on v0.9.5 should OTA this build.** The same files are
published on the [latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

## Fixed

- **The gauge no longer reverts to v0.9.4 after a v0.9.5 OTA.** Toggling
  App link / OBD2 on the connections page during the first ~25 s after boot
  crashed the board (two concurrent NimBLE mounts racing), and because the
  OTA-confirm gate ran behind the DHCP wait, the bootloader then rolled the
  healthy image back. Both halves are fixed: the BLE mount is serialized
  (concurrent callers block and no-op), and the dashboard server now
  listens within ~2 s of boot so the OTA image is confirmed immediately —
  the 25 s DHCP wait moved after the gate.
- Firmware version readout added on the connections-toggles page (v0.9.5,
  unchanged here).

## Files

| File | Purpose |
|---|---|
| `bootloader.bin` @ `0x0` | 2nd-stage bootloader |
| `partition-table.bin` @ `0x11000` | Partition table |
| `ota_data_initial.bin` @ `0x12000` | OTA data (boots ota_0) |
| `boost_gauge.bin` @ `0x20000` | App image — use for **web OTA** |
| `boost_gauge_merged.bin` @ `0x0` | Full-flash image for a complete reset |
| `flash.sh` + `flash_args` | Helper to flash the merged image |
| `BoostGauge-android-debug.apk` | Android companion 0.9.5 (versionCode 6) |
| `BoostGauge-ios-app.zip` | iOS companion 0.9.5 (build 5), install via Xcode/devicectl |
| `BoostGauge-0.9.5-ios.ipa` | iOS sideload IPA 0.9.5 (build 5) |
| `SHA256SUMS` | Checksums for everything above |

## Update notes

- **Web OTA:** serve `boost_gauge.bin` to the dashboard's firmware-update
  control. After the reboot, the boot log must read `Loaded app from
  partition at offset 0x420000` (ota_1); still booting `0x20000` proves the
  OTA never ran. `/api/v1/state` reports `v0.9.6`.
- Toggles on the connections page are now safe at any time, including
  immediately after an OTA.
- The image reports `v0.9.6` on `/api/v1/state` (`firmwareVersion`) and on
  the connections page.
- Hardware verification for this build: HTTP API ready at t=2.0 s after
  boot (was 27.0 s); both BLE toggles enabled and persisted across a manual
  restart; demo-mode dyno-cell fast-sweep cadence gate median 60 FPS
  (min 57); serial clean. Host suite 11/11.
- Companion apps are unchanged from v0.9.5: iOS **0.9.5 (build 5)**,
  Android **0.9.5 (versionCode 6)**.
