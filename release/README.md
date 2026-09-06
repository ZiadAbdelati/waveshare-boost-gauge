# Firmware v0.9.7 — timezone, scan, and readout fixes

Firmware **`v0.9.7`**, built with **ESP-IDF 5.5.1** for **ESP32-S3**, 16 MB
flash. Everyone on v0.9.6 or earlier should OTA this build. The same files
are published on the
[latest GitHub release](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

## Fixed

- **Dim schedule fires at the correct local time.** A companion-app write
  could store the timezone as a fixed offset (`UTC5`) without the
  daylight-saving rule, leaving the board an hour behind during DST — the
  20:20 dim then engaged at 21:20. Sync the time/timezone once (dashboard
  Sync button, or Settings → timezone on the current app) and the DS3231
  RTC recalibrates in the same write, so the schedule is correct across
  power cycles.
- **Wi-Fi scan from the phone app no longer fails.** Under BLE coexistence
  the Wi-Fi driver rejects a custom scan dwell (`scan_failed`, and once the
  phone's "did not respond" timeout); scans now use the driver's default
  dwell, and a scan that collides with the background saved-network scan
  retries once instead of surfacing an error.
- **Dyno Cell readout dead zone.** With a real MAP sensor idling at
  atmosphere, the readout no longer flaps between `0.0` and `-0.1`:
  readings within ±0.1 psi display as a solid `0.0`; outside the band the
  true value shows. The dashboard mirror matches.

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
  OTA never ran. `/api/v1/state` reports `v0.9.7`.
- **After updating, re-sync the timezone once** (dashboard Sync button or
  the app's timezone page). The gauge's dim schedule is only as correct as
  the stored timezone; this update stops future overwrites on the app side
  only after the app itself is updated.
- The image reports `v0.9.7` on `/api/v1/state` (`firmwareVersion`) and on
  the connections page.
- Hardware verification for this build: 30 s demo-mode dyno-cell
  fast-sweep cadence gate median 61 FPS (min 58); serial clean; served
  dashboard verified to carry the readout dead zone (decompressed). Host
  suite 12/12.
- Companion apps are unchanged from v0.9.6: iOS **0.9.5 (build 5)**,
  Android **0.9.5 (versionCode 6)**.
