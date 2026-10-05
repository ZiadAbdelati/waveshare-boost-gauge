# Release notes

The latest release notes also ship in `release/` (see `release/README.md`). Prebuilt firmware is on the [releases page](https://github.com/ZiadAbdelati/waveshare-boost-gauge/releases/latest).

> Notes for v0.9.x live in `release/README.md` and `docs/regression-ledger.md`;
> this file resumes at v1.0.0.

## v1.1.1

Three fixes to behaviour that shipped inert in v1.1.0, plus an explicit recovery path. All four surfaces (firmware, dashboard, iOS, Android) change together.

- **Panel rotation was a dead setting.** The persisted quarter turn was handed to the LVGL adapter, which refuses to rotate a `PANEL_IF_OTHER` display (QSPI/CO5300) — it logs "SPI rotation is not handled by adapter" and drops the value — so the setting saved, reported `restartRequired`, and did nothing. The turn is now applied to the CO5300's own scan order at init (`esp_lcd_panel_swap_xy` / `esp_lcd_panel_mirror` plus the visible-window gap), after `esp_lcd_panel_init()` and before the display registers, with the matching touch flags for the CST9217. **Rotation 0 is byte-identical to the previous build** (gap `(6, 0)`, the vendor touch flags). The touch mapping was corrected in pre-merge review (the host guard had modelled `esp_lcd_touch`'s flag order as swap-first; the driver mirrors both axes first) and **has not been confirmed on glass in either direction**.
- **The companion apps' timezone sync could be rejected outright.** Both apps send the phone's epoch as a clock calibration; against a readable DS3231 more than 5 minutes away the gauge answers `409 clock_rejected` and writes nothing, so the timezone never landed either. Both apps now retry the identical request without `epochMs`, so the zone still saves. Over BLE the `/time` route additionally required `epochMs` — which turned that retry into a 400 on the apps' primary transport; the route now mirrors HTTP field for field.
- **A force-clock recovery path.** The DS3231 stays the clock authority for ordinary syncs, but a genuinely wrong RTC was previously unrecoverable by any client (the dashboard's own message said to pull the RTC battery). A confirmed **Force clock** action on all three clients (`force: true`) now skips **only** the disagreement comparison, still applies the plausibility floor, and still writes the RTC — clearing OSF, so the corrected time becomes the new authority.
- **A present-but-wrong-typed `/time` field is now a 400** (`invalid_time`, `invalid_force`) instead of being silently reinterpreted — the same rule `pressureUnit` already followed. A wrong-typed `epochMs` used to downgrade a clock sync to a zone-only write.

**Verification honesty: the firmware in this release is host-built and simulator-verified, NOT hardware-verified** — no board was attached when it was cut. The release was reviewed pre-merge (one blocking finding: the panel-touch guard's transform order, fixed and re-verified), the host suite is 15/15, the HTTP/BLE contract suites are 239/239 and 63/63, and the panel-orientation tap guard passes 34/34 — including the two doctored variants proving it can fail. Both app artifacts are verified from their built metadata (`versionName`/`versionCode`, the IPA's `Info.plist`). **Unmeasured on glass:** the rotated panel's visible-window insets at 90/180/270, touch alignment under rotation, cadence/tear under rotation, and the forced-path serial line with the OSF-clearing RTC write. `v0.9.7` is still the last hardware-verified release — quote it, or a fresh board run, for physical measurements.

## v1.1.0

The headline of v1.1.0 is the **pressure display reference**: the gauge, the dashboard and both companion apps can now show pressure either **Relative** (gauge — what a boost gauge normally reads, atmosphere = 0) or **Absolute** (atmosphere included), independently of the selected unit. The physical settings overlay also grew from two pages to **three** (QR → Connections → Units), and Vault-Tec's bar readout was corrected.

- **Relative / Absolute display reference.** The reference is presentation-only, exactly like the unit: `/state.psi`, the `/logs` JSON and `/logs.csv` columns, TPMS comparisons, calibration, needle position, arc geometry and the zone-colour decision all stay gauge psi on the wire. The dead band (±0.1 psi) runs FIRST, in gauge psi, then the reference is applied, then the unit conversion — **Relative mode is a byte-for-byte passthrough of the previous renderer**, and the psi path remains the 60 FPS reference. Absolute mode adds the BMP280 ambient (`absolute = gauge + ambient`, falling back to the standard atmosphere when the sensor is absent), with the dial numerals baked at scene build.
- **Three-page settings overlay.** QR → Connections → Units, with a three-dot indicator and wrap-around horizontal paging. The Units page carries the unit cycler and the **`REL/ABS`** reference toggle, whose state line reads the full word (`RELATIVE` / `ABSOLUTE`).
- **Terminology corrected.** The mode pair shipped internally as "Atmospheric / Absolute"; that is not the standard pair for this axis — it is **relative (or gauge)** vs **absolute** pressure, and "atmospheric" names the reference value, not the mode. Renamed to Relative / Absolute on every surface, including the web option, both app pickers and the sim panel.
- **Vault-Tec + bar.** The readout box is centred on the face, so bar now anchors its decimal **point** on the box centre (the web mirror always drew it there); psi keeps its historic six-slot field and kPa its own anchor.
- **Pre-release review round.** Before the cut, four independent read-only reviews (firmware, clients, harness, release readiness) found and fixed two real defects — the arc PEAK label cached on the peak alone and so went stale as the ambient drifted, and the neon readout applied the dead band a second time after the reference — plus a web dial-tick rounding mismatch, a theme-preview ambient gap, and several tests that could not fail. Details in `docs/regression-ledger.md`.

**Verification honesty: the firmware in this release is host-built and simulator-verified, NOT hardware-verified** — no board was attached when it was cut. Relative-mode psi byte-identity is measured (35/35 screenshot frames across all five themes against a side-by-side pre-change sim), the host suite is 15/15 with the new renderer call-site guard, and both app artifacts are verified from their built metadata (`versionName`/`versionCode`, the IPA's `Info.plist`). On-glass cadence, overlay paging and the reference/unit cycle remain unmeasured; `v0.9.7` is still the last hardware-verified release.

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
- **Dyno Cell true black** (`dynoTrueBlack`): the arc face can now be pure
  **`#000000`** instead of `#080808`, mirroring Night City's `hudTrueBlack` — NVS key
  `dyno_black`, served by `/themes` as `dynoTrueBlack`, written by
  `PUT /api/v1/themes/config` on both transports, with a toggle in the web, iOS and
  Android theme editors. The renderer reads it through `arc_face_color()` at both
  places the arc face colour is chosen — the cached background canvas and the scene
  root — so the darker face also survives a scene rebuild instead of the old grey
  showing through mid-transition. Measured in the simulator: the dominant face
  colour goes (8,8,8) → (0,0,0) and back on toggle, with the black blended into the
  cached face rather than laid over it.
- **Layout corrections for the converted units**: the first 1.0.0 cut laid out bar
  and kPa from per-glyph advances, so a converted readout moved whenever a digit,
  a sign or a digit *count* changed. Converted readouts are now fixed-slot
  odometers — slot position is a function of a character's place in the number, not
  of the string's width — and are optically centred rather than merely stable.
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

### v1.0.0 reissue — 2026-10-04

The 1.0.0 **artifacts were rebuilt**: same version, same tag, replaced assets, so
the published binaries carry the unit-layout corrections and the Dyno Cell
true-black option added above. Anyone who downloaded the 2026-10-02 build should
re-download — the file names and the embedded version strings are identical, so
`SHA256SUMS` is the only way to tell the two builds apart.

What changed since the first cut: converted (bar / kPa) readouts became fixed-slot
odometers, were re-centred on every theme that draws numbers, and the second
fraction digit moved onto the digit pitch so bar's tenths and hundredths no longer
collide on dyno-cell; the Dyno Cell face gained the true-black option, with its
toggle in the physical overlay path, the web, and both apps; and
`tools/sim_panel.py` — an interactive panel that streams the real firmware
renderer to a browser with controls for theme, unit, pressure, layout, fonts,
page, TPMS and the per-theme options — landed so layout changes can be eyeballed
and measured before an OTA flash.

Isolation held to the pixel: psi readouts are byte-for-byte identical (0 differing
pixels on all five themes × four states), and the converted layouts moved only
where the fix intended — dyno-cell `bar` and big-digit `kPa`. The display, cadence,
media-store and WebSocket paths are unchanged.

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
