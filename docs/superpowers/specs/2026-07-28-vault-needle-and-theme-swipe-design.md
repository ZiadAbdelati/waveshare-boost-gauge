# Vault Needle Color and Theme Swipe Design

## Goal

Add a persisted green/red Vault-Tec needle choice and let the physical AMOLED cycle through themes with vertical swipes in the exact order shown by the dashboard.

## User-visible behavior

### Vault-Tec needle

- The Vault-Tec theme editor exposes one selector named `Needle colour` with `Phosphor green` and `Signal red` options.
- Green is the existing default and retains the existing overboost behavior: the normal green needle changes to the Vault-Tec warning yellow above the configured overboost threshold.
- Red uses FLARE red `#FF3B30` at every pressure, including overboost. Existing `OVER-PRESSURE` text and the rest of the face continue to communicate overboost.
- Firmware and the browser canvas mirror use the same setting and color.
- The setting persists in NVS independently from the three editable zone colors. Resetting a theme's zone colors does not change the needle choice.

### Vertical theme swipes

- A completed upward swipe selects the next theme in `boost_theme_at(0..count-1)` order. A downward swipe selects the previous theme.
- Both directions wrap continuously. Current order is Dyno Cell, Vault-Tec, Night City, Big Digit.
- A true tap resets peak. Any meaningful drag cancels tap-to-reset. A vertical drag that crosses the swipe threshold changes exactly one theme on release.
- Long press retains the existing brightness toggle and never also resets peak or changes theme.
- Horizontal or ambiguous drags perform no action.
- Theme switching is ignored while exclusive GIF playback owns the panel.
- The selected theme is persisted through the same `boost_model_set_active_theme()` path used by the web endpoint, then reflected in `/state` and `/themes` without a second theme-order convention.

## Architecture

### Needle preference

`main/boost_theme.c/.h` owns a new persisted boolean `vaultNeedleRed`, default false, using its own short NVS key. `GET /api/v1/themes` exposes it. `PUT /api/v1/themes/config` accepts a Boolean of the same name and returns the complete updated theme payload.

`draw_vault_needle()` selects the normal needle color as follows:

- `vaultNeedleRed == true`: `#FF3B30` regardless of pressure.
- otherwise: existing green/yellow selection using `theme->text` and `theme->overboost`.

The committed needle state must include the preference in its redraw decision, so a live color change at a stationary angle invalidates and repaints the complete old/new needle region without stale pixels. `web/app.js::drawVaultGauge()` mirrors the same decision.

The settings control follows existing theme-editor selector/control construction and `queueThemeConfig()` response resynchronization. `tools/mock_server.py` mirrors the API field. Web source changes regenerate `main/generated_web_assets.c/.h`; generated files are never edited by hand.

### Gesture classification

The screen event handler records the touch point on `LV_EVENT_PRESSED` and tracks maximum displacement during `LV_EVENT_PRESSING`. On release it classifies one mutually-exclusive result:

1. long press already fired: no further action;
2. vertical displacement crosses the swipe threshold and dominates horizontal displacement: cycle one theme;
3. displacement stays inside the tap slop: reset peak;
4. otherwise: no action.

Use fixed pixel thresholds scaled for the 466 x 466 panel, not timing or velocity heuristics. Direction is determined from release/start y. A gesture changes at most one theme; it does not cycle multiple entries based on drag distance.

Theme cycling looks up the active theme's index by ID in the authoritative `boost_theme_at()` array, applies modular next/previous arithmetic, and calls `boost_model_set_active_theme(next->id)`. Unknown active IDs fall back to the default index before cycling.

### Transition

The preferred path is a vertical slide matching swipe direction. The implementation may attempt a bounded, snapshot-backed transition:

1. capture the outgoing rendered screen into a full-screen RGB565 buffer;
2. persist/apply and rebuild the incoming theme using the existing scene lifecycle;
3. capture or retain the incoming rendered scene;
4. animate the outgoing image toward the swipe direction while the incoming image enters from the opposite edge;
5. remove transition objects and free all temporary buffers at completion.

The transition is decorative, never required for correctness. Any allocation, snapshot, or animation setup failure immediately falls back to the existing atomic `destroy_scene()` / `build_scene()` cut. No partial transition state may remain. The fallback must still persist and apply the requested theme exactly once.

Temporary buffers use PSRAM. Do not move LVGL's production 20-line DMA draw buffers, alter the region-double-buffer/TE synchronization architecture, or retain two full live scene trees. Input is ignored while the short transition is active to prevent overlapping rebuilds. The implementation must preserve GIF ownership and pixel-shift offset behavior.

If simulator or hardware evidence shows that two-snapshot animation causes allocation instability, stale pixels, tearing regressions, or an unacceptable full-frame cadence hitch, ship the automatic cut fallback rather than weakening display invariants.

## API compatibility and errors

- `vaultNeedleRed` is optional on PUT; absent clients preserve the current value.
- GET always returns a Boolean.
- Non-Boolean values are ignored consistently with existing Boolean theme toggles rather than coerced.
- Unknown theme IDs are never persisted.
- NVS absence preserves the green default.
- Theme cycling does not invent a second endpoint or queue; it invokes the model and gauge directly while already executing under the LVGL/display context.

## Verification

### Host

- Test or simulator exercise proves green default, red normal, and red overboost rendering.
- Mock API GET/PUT round trip includes `vaultNeedleRed`; web response synchronization retains the selected option.
- Gesture classifier covers tap, small drag, upward swipe, downward swipe, horizontal drag, long press, wrap in both directions, and one-theme-per-release.
- Simulator builds and renders every theme after repeated next/previous cycles.
- Embedded web assets regenerate without manual generated-file edits.

### Hardware

- Flash the production build.
- Select red in the dashboard and verify the physical Vault-Tec needle stays red below and above overboost; reboot and verify persistence.
- Select green and verify the existing yellow overboost transition remains.
- Swipe up through Dyno Cell → Vault-Tec → Night City → Big Digit → Dyno Cell; swipe down through the reverse order; reboot and verify the last selection persists.
- Verify true tap still resets peak, short drags do not, long press still toggles brightness, horizontal drags do nothing, and one swipe changes only one theme.
- Observe both animated and forced/failure fallback paths where practical; require no stale rows, half-rendered scenes, crash, reset, or leaked transition image.
- Run the existing Dyno Cell demo cadence guard for 30 seconds after the display-path change and require sustained median at least 60 FPS. Restore the user’s prior source/theme state after verification.
- Serial must show no stack overflow, Guru Meditation, `ESP_ERR_NO_MEM`, `send color data failed`, or TE timeout regression.
