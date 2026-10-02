# Release notes

The latest release notes also ship in `release/` (see `release/README.md`). Prebuilt firmware is on the [releases page](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

> Notes for v0.9.x live in `release/README.md` and `docs/regression-ledger.md`;
> this file resumes at v1.0.0.

## v1.0.0

The headline of v1.0.0 is a **global pressure unit**: the gauge, the dashboard and
both companion apps can now show pressure in **psi**, **bar** or **kPa**, switched
from the physical panel, the web Range page, or either app.

- **One unit, every surface.** The unit is persisted in the theme store (NVS key
  `unit`), served by `GET /api/v1/themes` as `pressureUnit`, written by
  `PUT /api/v1/themes/config`, and — because the panel's UNITS button has no other
  way to reach an already-open dashboard — carried in **`GET /api/v1/state`**, from
  which every client adopts it on every sample.
- **Presentation only.** All canonical values stay PSI (`/config`,
  `/state.psi`/`peakPsi`, `/logs`) or kPa (sensor and calibration diagnostics).
  Clients convert at the display and input boundaries. Gauge geometry, the arc
  wedges and the zero marker are always PSI and are never rescaled, so every psi
  readout path remains byte-for-byte the pre-units render.
- **Decimals**: psi 1, bar 2, kPa 0. The ±0.1 psi readout dead zone is defined in
  PSI and folded **before** conversion; Vault-Tec deliberately stays raw in every
  unit. The Neon zone-colour decision folds through the same band, so engine-off
  noise can no longer flip the ring colour between samples.
- **Firmware**: converted readouts use separate advances-centred layouts, and Big
  Digit's bar/kPa value is drawn as one label cell **per character** — a single
  full-width label invalidated its whole 466×120 box on every 0.01-bar step and
  measured 3.56× the psi flush per cycle (now 1.81× bar, 1.11× kPa). Converted Big
  Digit values keep the custom minus widget (`alvida_big` has no `-` glyph, so a
  literal sign in the string was painted as nothing — `-1.0` psi rendered as `0.07`
  bar). TPMS values and the low-pressure threshold follow the unit.
- **Physical panel**: the two-finger overlay's OBD2/App rows are replaced by a
  **2-up + 1-down cluster of three rounded buttons** — APP BLE, OBD BLE, UNITS.
  UNITS cycles psi → bar → kPa and persists. Button presses participate in the
  existing swipe classifier, so a drag that starts on a button is still a theme
  swipe, not a tap.
- **Dashboard**: a **Pressure unit** selector on the Settings page (Range section).
- **Version hygiene**: the release version now has exactly one source,
  `version.txt`. ESP-IDF bakes it into the app descriptor instead of deriving it
  from `git describe` (which is how a v0.9.7 release shipped a binary reporting
  `v0.9.6-3-gee90519`), and a new host test plus release gate
  (`tools/tests/test_version_consistency.py [--release]`) fails on any drift
  between the firmware image, both apps, the mock and `SHA256SUMS`.

> **Verification honesty:** the firmware in this release is **host-built and
> simulator-verified, not hardware-verified** — no board was attached. The units
> work is host-tested (12 host tests, geometry assertions, stale-pixel audits at
> 0 mismatches, simulator renders per theme × unit) and the app builds are
> verified from their artifacts, but the on-glass cadence A/B for the converted
> Big Digit readout and the physical feel of the overlay's UNITS button were not
> measured. **v0.9.7 remains the last hardware-verified baseline**; first flash
> should exercise the UNITS button and a bar/kPa Big Digit soak.

## v0.8.1

v0.8.1 adds **Doto** as the second Neon readout face and fixes two rendering regressions found while validating the new font.

- **Doto Neon readout.** Tube, Segments, and Marquee can use a modular Doto ROND 100 / weight 700 readout, persisted through NVS and mirrored in the dashboard. Doto uses raw A8 coverage without the SF Alien halo, plus a custom three-dot minus with tested signed spacing.
- **Neon ATMO label.** The zero-pressure zone now reads `ATMO` in white in both firmware and the web mirror.
- **Marquee wrap fix.** Spin and zone-flip invalidation now wrap complete bulb indices, preventing the outer-ring bulb at 12 o'clock from retaining stale colour across circular group boundaries.
- **Boot reliability.** The ESP-IDF main-task stack is 8,192 bytes so a persisted Neon scene can finish its synchronous glyph bake before brightness and networking start.
- **Reproducible assets.** The prepared static Doto dashboard font and SIL OFL license ship with the firmware; `tools/generate_doto_font.py` deterministically regenerates the LVGL subset.

Host verification covers the native geometry assertions, firmware/web parity, deterministic font and embedded-asset generation, Python/Node syntax, MAP conversion, RTC epoch conversion, and the ESP-IDF build. Hardware acceptance results are recorded in `release/README.md` and the regression ledger.

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
