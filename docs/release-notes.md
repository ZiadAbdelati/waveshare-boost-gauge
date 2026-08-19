# Release notes

The latest release notes also ship in `release/` (see `release/README.md`). Prebuilt firmware is on the [releases page](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

## v0.8.0

The headline of v0.8.0 is the **battery-backed clock**: the wall clock is now authoritative from a **DS3231 RTC on the sensor I2C bus**, so it survives power-off without Wi-Fi, the dim schedule stays correct, and a wrong browser clock can no longer corrupt it. Timezones are now **DST-aware** via a POSIX TZ string, and a pair of latent bugs in the RTC write path and the `/state` offset were fixed on hardware.

- **DS3231 RTC as boot-time clock authority.** `boost_sensors_rtc_read/write` (probe 0x68 on the shared sensor bus) reject OSF/garbage/implausible time; the seed runs before boot brightness is decided, so a night boot with a set RTC comes up dim from the first frame with no Wi-Fi. A browser Sync writes the RTC as calibration. Hardware-verified across soft resets and a full power-off.
- **RTC is the write authority too.** `POST /api/v1/time` more than 5 min from a valid DS3231 is rejected with `409 clock_rejected` *before* touching the system clock/NVS/RTC.
- **OSF cleared on write.** The DS3231 does *not* auto-clear its oscillator-stop flag when the time registers are written; `boost_sensors_rtc_write()` now clears it explicitly (status 0x0F) under the bus-admin lock.
- **DST-aware timezone (POSIX TZ string).** Config stores `timezoneTz` (e.g. `EST5EDT,M3.2.0/2,M11.1.0/2`) applied via `setenv("TZ")+tzset()`; the dim schedule, CSV, and the effective offset use `localtime()`, so DST is automatic. `/state` reports the DST-effective offset; `/config` keeps the stored standard offset for the dropdown.
- **`/state` offset stability.** `publish_sample()` no longer clobbers the DST-effective `timezoneOffsetMinutes` with the stored standard offset, which previously made the dashboard clock flicker by one hour across DST.
- **I2C bus hardening.** RTC read/write now hold the bus-admin mutex (the reset takes no lock); recovery honors ADS/BMP re-config returns (no silent 0 V false-good) and re-probes devices absent at boot; the live scan is time-capped (5 s).
- **Web fixes.** The timezone dropdown no longer reverts to the old zone when saving; the `UTC-04:00` entry is relabeled **Atlantic Time**; the Sync button is renamed **Save**.

Hardware verification this release covered boot, LAN + SoftAP network access, the DS3231 seed/authority and OSF clearing, the DST-effective `/state` offset, the bus scan and RTC coexistence, and the served web assets. The display cadence/media paths are unchanged from v0.7.1 (this release touches clock, I2C, and web only).

## Earlier releases

See the [GitHub releases page](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases) and the tags for notes on prior versions.
