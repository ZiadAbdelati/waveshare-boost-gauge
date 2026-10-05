# Cross-platform parity spec (iOS ⇄ Android)

Both companion apps MUST present the same information architecture, the same
screen inventory, and equivalent controls. Divergence between the two apps is
a defect, not a platform flourish. Every UI change lands on both platforms in
the same change-set; an agent that touches one app without mirroring the other
has not finished.

## Settings sub-page inventory (canonical order, exact titles)

| # | Page title | Contents |
|---|-----------|----------|
| 1 | Connection | BLE device picker, current selection, connection state; **Saved gauge row** shows the remembered peer identity at ALL times when a peer is known — Connected (name+address, "Connected" tag, no button), Reconnecting (identity, no button), Disconnected (identity + Connect button). "No gauge found" only after an empty user-initiated scan |
| 2 | Display | 3 grouped sections: **Brightness** (high/low steppers), **Dim schedule** (toggle + start/end), **Display** (rotation dropdown, regionDBuf, teSync, teScanline, pixelShift + interval); single **Save display settings** button. `appBle` is NOT exposed in the app (firmware `PUT /api/v1/config {"appBle":bool}` only; web UI also hidden — toggling via BLE would trap the app disconnected with no UI to re-enable) |
| 3 | Range | psiMin, psiMax, psiOverboost, zeroAngle fields; **Pressure unit** dropdown (PSI / bar / kPa, saves immediately via `PUT /themes/config {"pressureUnit"}`); **Pressure reference** dropdown directly beneath it (Relative / Absolute, saves immediately via `PUT /themes/config {"pressureAbsolute"}`, default Relative); Save button. Range field values are shown/entered in the selected unit and converted back to PSI before the `PUT /config` |
| 4 | Demo mode | **Demo mode** toggle + when ON, **Demo waveform** dropdown: `Organic swell` (= `demoFastSweep` false) / `Linear sweep (9.789 psi/s)` (= true); **Save demo settings** button. THEME-SPECIFIC settings (vaultNeedleRed, vaultNeedleTail, bigDigitStaticBg) NEVER appear here — they live exclusively in the Themes tab inside the matching theme's editor dropdown |
| 5 | Clock & timezone | Timezone dropdown (curated list + Custom), one full-width primary button labelled exactly **"Sync timezone to gauge"**, and a secondary **"Force clock"** action behind a confirmation. The primary button POSTs `/time` with the phone epoch (a calibration attempt: the gauge's RTC stays the write authority and rejects a disagreement over 5 minutes with 409 `clock_rejected`) and, on that 409, retries the identical body without `epochMs` so the timezone still lands — the resulting message must say the zone was saved but the gauge's clock was rejected and name Force clock. Force clock is the explicit recovery path for a genuinely wrong RTC: it POSTs the same body with `force: true`, is never retried on 409, and is only offered behind a confirmation warning that it overwrites the gauge's clock and RTC. `timezoneOffsetMinutes` is the only field the firmware requires |
| 6 | TPMS & OBD2 (merged page) | TPMS BLE link toggle (instant `PUT /themes/config tpmsBle`), lowPsi threshold, staleness (staleAfterMs; iOS picker auto-includes a saved custom value so the real state always shows), Save button, then the OBD link status: pill (`Scanning` / `Connecting to <name>` / `Connected` / `Idle` + lastError), peer rows (name + address) + **Forget** (clears `obd_peer` NVS). No scan trigger needed (gauge auto-scans when `tpmsBle` is on). Standalone TPMS page removed on both apps |
| 8 | Wi-Fi | STA/AP status (mode, staConnected staSsid staIp rssi, apSsid apIp, saved list), **Scan networks** button → results (ssid rssi auth, tap to fill SSID), SSID + password fields + **Save** (`PUT /network` apsta), saved-network **Delete** (`DELETE /network`), **Reconnect** (`POST /network/reconnect`). All via BLE Control over the live link — no SoftAP join needed; STA stays usable alongside BLE. **Use-phone-network**: Android reads the phone SSID (`WifiManager`, needs FINE location runtime grant); iOS CANNOT read the SSID without the paid Access WiFi Information entitlement — the button explains this and points to the scan-list join. Both apps request permissions in-line from the tap |
| 9 | About | Dedicated sub-page (not an inline footer): **App** `1.1.2 (11)` on iOS and `1.1.2 (13)` on Android, where the parenthesised number is the store build number (`CFBundleVersion` / `versionCode` — they are separate counters and are not required to match), bundle/package, **Gauge firmware** + API/device when connected, and source link. Both platforms use the same grouped-card spacing (`GroupedSection` 8 dp vertical padding, `HorizontalDivider` 0.08 α, `BoostMetric` / `BoostMetricValue`) |

## Connection state arbitration (2026-08-26)

The transport is the single source of truth for link liveness: when the
GATT connect/subscribe succeeds, `connectionState` MUST go `.connected` and
any reconnect loop MUST cancel itself immediately — never gate the transition
behind a later device-info read, and never keep counting reconnect attempts
over a live link (observed: "Reconnecting… (attempt 12)" while the board
reported the phone connected and streamed status for 18+ minutes). The
reconnect loop must also stop within one tick if it finds the link already
healthy at its iteration top. Link loss is reported by the transport's own
didDiscover/didDisconnect event path, which re-enters the loop; the reconnect
banner ("Reconnecting… (attempt N)") is only ever shown when the link is
actually down and a peer is known.

Rules:

- `tpmsBle` lives ONLY on the TPMS page on both platforms.
- The clock page has NO descriptive paragraphs: dropdown + button only. The
  button is full-width and single-line.
- Root menu rows appear in the table order above with the exact titles above.
- Field labels match the firmware config keys where a key exists (psiMin,
  psiMax, psiOverboost, zeroAngle) — same wording both platforms.

### Saved gauge row visibility (2026-08-26)

The **Saved gauge** row (name + address from the persisted selection) is
visible whenever a peer is remembered AND the link is not connected — including
while auto-reconnecting and on a fresh launch before the first connect. It is
hidden entirely while connected (a Connect action against a live link is
meaningless). While auto-reconnecting the row is shown with **no Connect
button** — the "Reconnecting… (attempt N)" banner carries the reconnect state,
so the word "Reconnecting…" appears exactly once on the page (the pill). After
an explicit disconnect the row shows its **Connect** action.

"No gauge found. Make sure the gauge is advertising." appears **only** for an
empty scan-result list after a user-initiated scan, and never while a peer is
remembered (it would contradict the Saved gauge row). The disconnect
confirmation must not echo the status word — "Disconnected" (or "Not
connected") is rendered exactly once, by the single `displayLabel`/connection
pill helper shared by the Connection page and the dashboard footer. Android
reference: `savedRowAction()` in `SettingsScreen.kt`; the visibility matrix is
pinned by `SavedRowVisibilityTest`.

### Transport lifecycle (2026-08-26)

Every path that drops or tears down the transport must fully close the
underlying `BluetoothGatt` — `close()`, not just `disconnect()` — so the board
releases the ACL and restarts advertising. This covers: explicit disconnect,
repository/reconnect-loop teardown, a GATT link-loss event (the transport
closes the gatt on its own didDisconnect path), a failed connect, and a
connect timeout. A leaked gatt object leaves the board advertising nothing and
the app stuck on "Reconnecting…" while a stale link still answers control
writes (field-report round 8: control writes continued with NO phone-connected
event). A connect/request timeout must surface as a transport error, never a
`CancellationException`, or the reconnect loop treats it as its own
cancellation and dies forever ("never reconnects after restart"). Android
reference: `BleTransport` gatt lifecycle + `BleTransportGattLifecycleTest`.

## Theme preview

The theme preview is a CIRCLE with the web `.gauge-device` bezel: an ~8 px
`#0c0e12` pod ring plus a hairline rim, transparent corners, no offset drop
shadow. Never render the preview as an unclipped square. iOS reference:
`ThemesView.themePreview`. Android must produce the same silhouette.

A canonical preview must also seed the renderer's live `ambientKpa` from
`/state.sensors.ambientKpa` (never the standard atmosphere) whenever the
reference mode is Absolute, so the preview numeral matches the dashboard hero
for the same sample.

## Process rules for agents

1. Read this file before any view work. Re-read it if your task mentions
   settings, pages, previews, or navigation.
2. Any new setting, page, or control is added to BOTH apps in the same task,
   at the same position in the IA.
3. Before reporting done, diff your result against this file page-by-page and
   state "PARITY: conformant" plus any intentional exception in your report.
4. Exceptions require coordinator sign-off and get recorded here first.

## Theme-specific settings (2026-08-26)

Settings that only affect one theme (vaultNeedleRed, vaultNeedleTail,
bigDigitStaticBg) are edited ONLY in the Themes tab, inside that theme's
editor dropdown — never in Settings → Demo mode. Both apps must expose the
same per-theme controls with the same labels.

**True black background** is a per-theme toggle in the theme editor: Night City
(`hudTrueBlack`) and Dyno Cell (`dynoTrueBlack`) each get one, labelled
"True black background" on both apps and saved via `PUT /themes/config` with
the theme's other options.

## Logs graph window (2026-08-26)

The logs graph shows the LAST 5 MINUTES (`GET /logs?seconds=300`), never the
full hour ring; full-history fetch over BLE is too slow and an hour of dense
sweep is not readable. Both apps use seconds=300 for the graph.

## Orientation (2026-08-26)

Both apps support portrait AND landscape on phones. Landscape must make
elegant use of the extra width (side-by-side panes where natural — e.g.
status/dashboard, themes grid), never letterbox, stretch, or merely center a
portrait layout. Visual style is unchanged: same colors, bezels, typography,
and component shapes. Settings/log lists may stay single-column if that reads
better. Both platforms must behave equivalently.

## Logs window (2026-08-26)

Graph fetches `GET /logs?limit=1500` (= last 5 minutes at the 5 Hz log rate)
on BOTH transports. Never default to the full-hour ring (18000 samples): it
times out over BLE into the 8-sample diagnostic fallback and does not render
meaningfully. The 8-sample BGL1 window remains a last-resort error state only.

## BLE session resilience (2026-08-26)

Both apps reconnect to a known BLE peer indefinitely with the same exponential
backoff (1 s → 2 s → 5 s → 10 s → 30 s → 60 s cap, resetting on any connect) and
show the same banner string **"Reconnecting… (attempt N)"** while retrying,
never "Disconnected" when a peer is known. Backoff reference: Android
`GaugeRepository.backoffDelayMs`, iOS `BLEBackoff`.

Intentional divergence (coordinator-signed, recorded here): Android keeps the
link alive with a **foreground service**; iOS uses the platform-native
**`UIBackgroundModes bluetooth-central` + CoreBluetooth state restoration**
(`CBCentralManagerOptionRestoreIdentifierKey`) instead, which lets the system
retain/relaunch the BLE link while the app is backgrounded. iOS has no
equivalent of a user-visible foreground notification for an app of this class,
so no notification is shown — the reconnect banner is the only indication.
The reconnect loop runs while the app is foreground; a suspended iOS app resumes
retrying on foreground (scenePhase .active → `refreshBLELinkState`).

## OBD2 Forget contract (2026-08-26)

The OBD2 Scanner page's Forget button calls `POST /api/v1/obd/forget`
(firmware clears NVS `obd_peer`, drops any live link, central returns to
idle). No other endpoint or config field touches the OBD peer.

## Calibration supply voltage edit (2026-08-30)

The Calibration page edits the MAP supply voltage on BOTH platforms: an
editable numeric field pre-filled with the current supply and a
**Save supply voltage** button that PUTs `/sensors/supply`
`{"supplyVolts":N}` and refreshes the calibration from the response. Values
outside 4.5–5.5 V (firmware `BOOST_MAP_SUPPLY_MIN`/`BOOST_MAP_SUPPLY_MAX`)
are rejected client-side before the send; a firmware 400 is surfaced the
same way. Save success and save/load-refresh failures are **transient
bottom toasts** (iOS `WindowToast`, Android M3 `SnackbarHost`), never inline
list rows; the client-side out-of-range validation error is shown inline
next to the field on iOS and via the toast on Android. Same section title
(**MAP supply voltage**) and field label (**Supply (V)**) on both platforms.

## Pressure units (PSI / bar / kPa) (2026-10-02)

A single global display unit is persisted on the gauge (theme store, NVS `unit`)
and served by `GET /themes` as `pressureUnit` (`"psi"|"bar"|"kPa"`, default
`"psi"`). It is presentation-only: every wire value stays PSI (boost, TPMS,
range config, logs) or kPa (sensor/calibration diagnostics); clients convert at
display/input boundaries. Decimals: psi 1, bar 2, kPa 0; bar = psi x
0.0689475729, kPa = psi x 6.89475729.

Both apps surface the dropdown on the **Range** page and save it immediately
via `PUT /themes/config {"pressureUnit": ...}` (patch A of the themes payload).
Every pressure readout (dashboard hero + unit label, peak, TPMS cards and
threshold, logs min/max/crosshair/axis labels, calibration `offsetPsi` row)
converts; sensor/calibration kPa rows and the TPMS low-threshold comparison stay
canonical (kPa / PSI). The theme-preview payload must include `pressureUnit` so
the bundled canonical `web/app.js` renders the selected unit. The physical
panel cycles the same unit from the two-finger overlay's UNITS button.

## Pressure display reference (Relative / Absolute) (2026-10-04)

A second global display setting rides beside the unit: `pressureAbsolute`
(`GET /themes`, default `false` = Relative/gauge), written by
`PUT /themes/config {"pressureAbsolute": bool}`. `Absolute = gauge + the BMP280
ambient`, so an engine-off 0.0 psi reads ~14.7 psi / ~1.01 bar / ~101 kPa. The
baseline comes from the `/state` field both apps already parse
(`sensors.ambientKpa`), falling back to 101.325 kPa when it is missing or <= 0;
no new sensor field was added. The flag itself also ships in `/state` beside
`pressureUnit`, and both apps adopt it from EVERY state sample (not only
`/themes`) so a mode flipped on the physical panel reaches an already-open app —
the same live-path rule the unit has.

Both apps surface a **Reference** row directly beneath **Pressure unit** on the
Range page, same control type as the unit row, saving immediately (no Save
button, no text entry, so the keyboard-dismissal contract is not involved).
Every boost-side numeral converts — the dashboard hero, peak, the logs chart
axis labels, crosshair pill and min/max rows, and the theme previews — while
TPMS values/thresholds, calibration diagnostics, the live gauge canvas geometry
and everything derived from raw psi stay on the gauge value. The `/logs` JSON
psi and the `/logs.csv` columns are wire values and stay gauge psi; only the log
numerals a client renders are reference-adjusted. The zone word and
colour stay gauge-relative too: a positive absolute reading in vacuum still
reads VAC. The physical panel toggles the same mode from the two-finger
overlay's third page (the `REL/ABS` button).

## Status badge layout (2026-08-30)

The dashboard hero shows the zone word in a large headline capsule
(`.headline` / `BoostSectionTitle` bold, 12/4 padding), horizontally
centered in the card, with the LIVE/DEMO capsule in the ORIGINAL smaller
caption size (`.caption.weight(.semibold)` / `BoostCaptionSemibold`, 10/4
padding) centered directly beneath it — a vertical stack, not side-by-side.
Same sizes, padding, colors, and vertical ordering on both platforms.
