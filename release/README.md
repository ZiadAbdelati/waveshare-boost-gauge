# v1.1.2 — the settings overlay stops fighting the finger **(re-cut)**

Firmware 1.1.2 (ESP-IDF 5.5.1) · iOS 1.1.2 (11) · Android 1.1.2 (versionCode 13,
Android 10+). **Firmware-only** — no dashboard or companion-app source changed; the app
artifacts are re-cut only because the version surfaces move together.

> **This is a RE-CUT of v1.1.2, published 2026-10-05.** The first v1.1.2 artifact set shipped
> one further settings-overlay defect found on glass — the BLE buttons applied their toggle but
> never repainted, so they looked dead until a page change — so the release and its tag were
> **deleted and republished** with the fix included. **Same version number, different firmware
> bytes:** if you downloaded v1.1.2 before this, re-download. The fix is PR #10
> (`qr_toggle_apply_cb` now ends every fall-through branch with `qr_goto_page(s_qr_page)`);
> the tag `v1.1.2` points at the new commit, and every artifact here was rebuilt from it.

> **Verification honesty: the firmware in this release is host-built and
> simulator-verified, NOT hardware-verified.** No board was attached; the bug report came from
> glass and the fix has not been there. Both changes were reviewed pre-merge (`ship`, every
> finding closed), the host suite is 15/15, the gesture-constants contract is 29/29 — doctored
> both ways, to prove the new repaint guard fails when the tail rebuild is removed and again
> when a BLE branch is given its own `return` — and the overlay harness produces **3 failures on
> the pre-fix source and 0 after**, then drives the same overlay through LVGL's **real** dispatch
> with a synthetic pointer indev (14 further assertions: both BLE squares toggling and repainting
> on the page they are on, no `CLICKED` bubbling to the overlay, a 30 px drag neither dismissing
> nor stepping, a vertical flick doing nothing, a 7 px jittered tap still toggling, UNITS /
> REL-ABS, and a tap on the QR code falling through). Both app artifacts were verified from their
> built metadata (`versionName`/`versionCode`, and the IPA's `Info.plist` read from *inside* the
> IPA). **Unmeasured on glass:** the overlay's flick classification against a real CST9217 — if a
> tap habitually drifts ≥12 px before release it now reads as a drag, which is the deliberate
> 12 px tap slop page 0 has always used — plus everything v1.1.1 left open: the rotated panel's
> visible-window insets at 90/180/270, touch alignment under rotation, cadence/tear under
> rotation, and the forced-clock RTC write. `v0.9.7` is still the last hardware-verified
> release — quote it (or a fresh board run) for physical measurements.

## What this changes

- **The BLE buttons never repainted after a tap.** Each square's glow, status LED and ON/OFF line
  are baked by `show_qr()` from the live link state, and nothing else ever repaints the overlay
  (the 16 ms gauge path is gated on `s_qr_active`). The deferred applier rebuilt the scene for the
  unit cycle and called `qr_goto_page()` for the reference toggle only, so a BLE toggle left the
  button looking dead until a page step rebuilt the overlay — even though the state had changed.
  The applier now ends every fall-through branch with `qr_goto_page(s_qr_page)`: same in-place
  turnover, same page, so a toggle never appears to navigate. *(New in the re-cut.)*
- **A drag was dismissed as a tap.** `qr_click_cb()` swallowed the release's `CLICKED` only when
  the drag had *classified* as a swipe — ≥48 px **and** winning one of two axis-ratio tests. A
  20–47 px flick on the overlay background (past the 12 px tap slop, under the swipe threshold)
  set no latch and fell straight through to `hide_qr()`, closing the whole settings overlay; a
  ~45° drag hit the same dead cone between the two ratio tests. A gesture that moves past the tap
  slop is now a drag **by either route** (`s_qr_drag_seen`), at the overlay background **and** at
  all four square switches.
- **The drag origin and the one-shot latch outlived the gesture.** After a tap on a square — or
  after a vertical (theme) classification, which never routed through the page-rebuild path — the
  next background drag was measured from the *previous* gesture's touch-down point, so a 20 px
  flick classified as a 200 px one (a theme change out of nowhere), and because the one-shot latch
  stayed set the classifier then returned early for the rest of that gesture **and for every later
  one**: the overlay stopped stepping pages and stopped dismissing until it was reopened. One
  gesture = one touch-down now.
- **A vertical drag on the settings overlay does nothing** (user decision) — the theme sits behind
  a fully opaque cover, so that branch had no affordance, and a slightly diagonal swipe looked
  like the overlay "changing themes" for no reason. The gauge's own page-0 vertical theme swipe is
  unchanged.
- **Review-round corrections**, all found by pre-merge reviews of the changes themselves: the four
  switch callbacks apply the same not-a-tap rule as the overlay background; `show_qr()` no longer
  touches gesture state (it is also the mid-drag rebuild path); the rebuild witness is the page,
  not the object pointer; and the repaint guard now also requires exactly three early returns, so
  a BLE branch cannot `return` past the rebuild.

## Version hygiene

The version has exactly ONE source, the repo-root `version.txt` (bare `MAJOR.MINOR.PATCH`);
ESP-IDF reads it at build time so the firmware image cannot disagree with its tag, and the app
versions, the docs that state the current release, the app-image descriptor, the merged image and
`SHA256SUMS` are all checked against it by `tools/tests/test_version_consistency.py` — source mode
on every commit, and `--release` (artifact mode) before publishing. The artifacts in this directory
were built from the re-cut release commit; `SHA256SUMS` matches the bytes on disk and covers every
shipped file.

## Firmware binaries

`boost_gauge.bin` (2,682,832 bytes), `bootloader.bin`, `partition-table.bin`,
`ota_data_initial.bin` and `boost_gauge_merged.bin` (2,813,904 bytes) are fresh 1.1.2 builds of the
re-cut tree. The app image is `0x28efd0` bytes — 36 % of `ota_0` free (`0x171030`), 16 bytes above
the first v1.1.2 cut, consistent with a firmware-only gesture change. The bootloader, partition
table and `ota_data` are **byte-identical to v1.1.1 and to the first v1.1.2 cut** (verified by
checksum), as nothing in their inputs changed.

This is an OTA-capable app-image release: web OTA uses `boost_gauge.bin` (offset `0x20000`);
`boost_gauge_merged.bin` is for a full-flash reset (offset `0x0`). Both images carry
`esp_app_desc.version = 1.1.2` (verified at offset `0x30` in the app image and `0x20030` in the
merged image, i.e. the `ota_0` offset plus the same descriptor offset), the merged image's four
slices were verified byte-identical to the individual partition files, and it is exactly `0x20000`
bytes larger than the app image.

The display panel geometry (rotation 0), cadence, media-store, WebSocket and `/time` paths are
unchanged; this change-set touches only the settings overlay in `main/boost_page.c`.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions (unchanged) |
| `flash.sh` + `flash_args` | one-command full flash helper (`./flash.sh /dev/ttyACM0`) |
| `BoostGauge-android-debug.apk` | Android 1.1.2 (versionCode 13, minSdk 29 — installs on Android 10+ head units), debug build |
| `BoostGauge-1.1.2-ios.ipa` | iOS 1.1.2 (build 11), unsigned sideload IPA (arm64 device build) |
| `BoostGauge-ios-app.zip` | the same `.app` at the zip root, for `devicectl` install |
| `SHA256SUMS` | checksums for every file above |

The IPA is **unsigned** — no provisioning profile for this bundle id is installed on the build
machine, so the Release archive is built with `CODE_SIGNING_ALLOWED=NO`. Sideloading tools re-sign
it on install. The Android APK is a debug build, as in previous releases. Both app artifacts were
rebuilt from this commit even though no app source changed, so every shipped file comes from the
same tree.

## Verify

- Gate: `python3 tools/tests/test_version_consistency.py --release` → every shipped artifact
  reports 1.1.2 and `SHA256SUMS` matches the bytes.
- On a board: `GET /api/v1/state` must report `"firmwareVersion":"1.1.2"`.
- Apps: iOS About → `1.1.2 (11)`; Android About → `1.1.2 (13)`.
- Harness: `./sim/build/boost_gauge_sim --qr-test DIR` → 0 failures, including the 14 real-tap
  assertions.
- On the glass (the acceptance step for this release): open the two-finger settings overlay and
  confirm, on the Connections page, that **tapping OBD BLE / APP BLE flips the square's glow, LED
  and ON/OFF line on the spot** (the re-cut's fix) without changing page. Then the rest: a
  **horizontal** flick steps one page with wraparound, a **sub-48 px** flick leaves the overlay
  open, a swipe **after** any flick still steps a page, a short flick starting **on** a switch does
  not flip it while a tap still does, and a **vertical** flick on the overlay does nothing at all —
  while a vertical swipe on the gauge face itself still switches themes. If overlay taps feel
  unresponsive, that is the 12 px tap slop vs real touch jitter. Note that a tap in the 40 px gap
  between the two squares dismisses the overlay (the documented fresh-tap rule).
- Still open from v1.1.1 (unchanged by this release): the rotated panel's insets/touch at
  90/180/270 and the forced-clock RTC write; and `tools/check_hardware_gates.py` on the dyno-cell
  face in demo mode.
