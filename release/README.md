# v0.9.9 — Neon engine-off flicker fix (firmware) + reset-button visibility (apps) — REPLACES v0.9.8

Combined release. **This release replaces v0.9.8**, which it supersedes:
everything v0.9.8 shipped (the companion-app Neon preset fixes) is included
here, plus the firmware zone-flicker fix that landed afterwards. If you are
coming from v0.9.7 or v0.9.8, this is a single update; the firmware binaries
are NEW in this release (not a reuse of v0.9.7).

> **Verification honesty:** the firmware fix in this release is
> **simulator-verified, not hardware-verified** — the owner explicitly
> declined a hardware pass (gauge stays in the car). The zone-decision
> logic measured in the host simulator is the same shared source the device
> runs, and the fix is stateless (a fold, not a feedback loop), but the
> physical-panel conditions — the ~40–100 ms hardware zone-flip recolor
> floor, TE/tearing, and the real sensor noise spectrum — were not
> measured. First flash to hardware should park engine-off on Neon
> (marquee layout worst-case) and watch the rings for a minute.

## Firmware fixed (v0.9.9, ESP-IDF 5.5.1)

- **Neon engine-off zone flicker + marquee second-ring flashing.** With the
  engine fully off, sensor noise (~±0.05 psi) straddled Neon's raw 0.05 psi
  vacuum/boost threshold, so the zone color flipped every 16 ms sample — and
  each flip re-fired the marquee's deferred run repaint (word-first,
  arc-next-frame), visible as the second bulb ring flashing on/off. The
  v0.9.7 readout dead zone deliberately covered only the displayed number;
  this user-issued override (2026-09-09) extends the SAME ±0.1 psi fold
  (`boost_readout_display_psi()`) to the Neon zone-color decision inside
  `neon_zone_rgb()`/`neon_zone_id()` — one band definition, no hysteresis,
  no second threshold. Inside ±0.1 psi the zone is constant (vacuum color);
  outside the band, behavior is unchanged. Draw and flip detection share the
  folded decision by construction (every consumer routes through those two
  functions). Dyno-cell's arc/wedge and Vault-Tec keep raw psi (their
  existing guards make them flicker-free). Web mirror updated
  (`neonZoneDisplayPsi()` delegating to the same JS fold).

  Host-simulator evidence (probe harness + raw data in the repo at
  `preview/sim/zonefold_probe/`, sheets at `preview/sim/*/`):
  - Engine-off noise, ±0.05 psi, 620 frames × 5 seeds: pre-fix 34–54 zone
    flips per run → post-fix **0 flips, every run**.
  - Band edges: vacuum through +0.10 (byte-identical frames at
    -0.10/-0.09/0.0), boost from +0.11, overboost at +8.0 intact.
  - Marquee ring regions: **0 pixel diffs across 10 consecutive captures**
    (spin off); spin-on chase still animates (expected).
  - Real motion: constant-slew 9.789 psi/s still flips at exactly the real
    crossings on all three layouts (zone color is not frozen).

## Companion apps fixed (iOS 0.9.7 (7) / Android 0.9.7 (8))

All v0.9.8 fixes carry over (Neon preset stale-color clobber, instant
preset/layout/font apply, iOS reset-button hit area). New in this release:

- **"Reset to default colors" appears as soon as edits exist — before
  Apply.** The button used to key solely on the board's `customized` flag,
  which only flips once Apply commits the colors, so unsaved edits had no
  reset affordance. Both apps now show it when there are unsaved local
  edits OR the board reports customized; reset/apply echoes clear the edits
  and re-hide it; a failed reset keeps your edits. Verified end-to-end in
  the iOS simulator and Android emulator: edit a zone color → button
  appears immediately → tap reset → color reverts and button disappears.
- Unit tests added on both platforms (Android 109/109 green; iOS 108 passed
  with only the two known pre-existing timezone failures).

## Firmware binaries — NEW in this release

`boost_gauge.bin` (SHA-256 `70f26f49…b505c85`), `bootloader.bin`,
`partition-table.bin`, `ota_data_initial.bin`, and `boost_gauge_merged.bin`
are fresh v0.9.9 builds. This is an OTA-capable app-image release: web OTA
uses `boost_gauge.bin`; `boost_gauge_merged.bin` is for a full-flash reset
(`./flash.sh /dev/ttyACM0`). v0.9.7's hardware-verified baseline (cadence
gates, media store, WebSocket pool) is unaffected by this change-set; the
zone-fold touches only the Neon zone-decision and its web mirror.

## Files

| File | Purpose |
|---|---|
| `boost_gauge.bin` | app image for web OTA (offset 0x20000) |
| `boost_gauge_merged.bin` | full-flash image (all four partitions, offset 0x0) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | individual partitions |
| `flash.sh` + `flash_args` | one-command full flash helper |
| `BoostGauge-android-debug.apk` | Android 0.9.7 (versionCode 8), debug build |
| `BoostGauge-0.9.7-ios.ipa` | iOS 0.9.7 (build 7), sideload IPA |
| `BoostGauge-ios-app.zip` | same .app for devicectl install |
| `SHA256SUMS` | checksums for every file above |

Install the APK / sideload the IPA, or OTA the app image, then verify:
`/api/v1/state` reports firmware version `v0.9.9` (git-describe derived at
release tagging).
