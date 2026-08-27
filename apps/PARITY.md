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
| 2 | Display | Brightness high / low steppers, dim schedule toggle + start/end times, **Companion app advertising** toggle (`appBle`), Save button |
| 3 | Range | psiMin, psiMax, psiOverboost, zeroAngle fields, Save button |
| 4 | Theme & demo | **Demo mode** toggle + when ON, **Demo waveform** dropdown: `Organic swell` (= `demoFastSweep` false) / `Linear sweep (9.789 psi/s)` (= true); plus rotation, regionDBuf, teSync, teScanline, pixelShift (+interval), Save button. THEME-SPECIFIC settings (vaultNeedleRed, vaultNeedleTail, bigDigitStaticBg) NEVER appear here — they live exclusively in the Themes tab inside the matching theme's editor dropdown |
| 5 | Clock & timezone | Timezone dropdown (curated list + Custom), one full-width primary button labelled exactly **"Sync timezone to gauge"** |
| 6 | TPMS | lowPsi threshold, staleness (staleAfterMs), TPMS BLE link toggle |
| 7 | OBD2 Scanner | Live OBD state pill (`Scanning` / `Connecting to <name>` / `Connected` / `Idle` + lastError), peer row (name + address) + **Forget** button (clears `obd_peer` NVS), and a one-line helper: "Gauge → OBD2 dongle link". Read-only when idle; no scan trigger needed (gauge auto-scans when `tpmsBle` is on) |

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
editor dropdown — never in Settings → Theme & demo. Both apps must expose the
same per-theme controls with the same labels.

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
