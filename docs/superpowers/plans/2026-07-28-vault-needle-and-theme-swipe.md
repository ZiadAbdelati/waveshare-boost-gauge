# Vault Needle Color and Theme Swipe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a persistent red Vault-Tec needle option and one-theme-per-vertical-swipe navigation with a safe snapshot-backed slide and atomic-cut fallback.

**Architecture:** Extend the existing theme-config store/API for the needle choice, then factor touch classification and theme-index wrapping into host-testable helpers used by the physical screen event handler. Apply a swipe through the existing model persistence and scene lifecycle; decorate it with two temporary PSRAM RGB565 snapshots when allocation succeeds, otherwise retain the current immediate rebuild.

**Tech Stack:** ESP-IDF 5.5.1, C11, LVGL 9.4, browser JavaScript/HTML/CSS, Python mock server, CMake desktop simulator.

## Global Constraints

- Use only OMP or Orca subagents; implementation agent model is GPT-5.6 Luna.
- Green remains default and changes to existing Vault warning yellow in overboost.
- Red is exactly `#FF3B30` and stays red through overboost.
- Swipe up advances and swipe down reverses the authoritative `boost_theme_at()` order; both wrap and one release changes at most one theme.
- Only a true tap resets peak; meaningful drags cancel tap behavior; long press remains brightness toggle; horizontal/ambiguous drags do nothing.
- Ignore theme swipes during exclusive GIF playback and during an active transition.
- Persist swipe selection through `boost_model_set_active_theme()`; do not create another theme-order convention or API.
- Animation is decorative: any allocation/snapshot/setup failure must produce one clean immediate cut with no retained partial transition state.
- Temporary transition buffers belong in PSRAM. Do not alter the two 20-line internal-DMA draw buffers, TE sync, or region-double-buffer architecture, and do not retain two full live scene trees.
- Edit web sources, then regenerate `main/generated_web_assets.c/.h` with `python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h`; never hand-edit generated assets.
- Significant display changes require simulator evidence, production build, COM3 flash, physical interaction checks, serial health, and the 30-second Dyno Cell/demo cadence guard with median at least 60 FPS.
- Update both `README.md` and the `AGENTS.md` regression ledger with the final architecture and measured verification before completion.

---

### Task 1: Persist and Render the Vault Needle Choice

**Files:**
- Modify: `main/boost_theme.h`
- Modify: `main/boost_theme.c`
- Modify: `main/boost_gauge.c`
- Modify: `main/boost_web.c`
- Modify: `web/app.js`
- Modify: `tools/mock_server.py`
- Regenerate: `main/generated_web_assets.c`
- Regenerate: `main/generated_web_assets.h`
- Test/extend: existing simulator or focused host test target following repository convention

**Interfaces:**
- Produces: `bool boost_theme_vault_needle_red(void)` and `void boost_theme_set_vault_needle_red(bool enabled)`.
- Produces API field: top-level `/api/v1/themes` Boolean `vaultNeedleRed`; optional `/api/v1/themes/config` Boolean input of the same name.
- Produces browser state/control: `state.vaultNeedleRed` and `Needle colour` selector values `green`/`red`.

- [ ] **Step 1: Add a failing host test for the theme preference**

The test must prove the default is false, setting true reads true, and setting false reads false in the host/simulator build. Run the focused test and confirm it fails because the two accessors do not exist.

- [ ] **Step 2: Implement the minimal theme preference store**

Add a false-initialized Boolean, a short NVS key no longer than 15 characters, load it in `boost_theme_init()`, persist it alongside existing theme settings, and expose the accessors in `boost_theme.h`. Re-run the focused test and require PASS.

- [ ] **Step 3: Add failing rendering assertions**

Exercise the Vault needle color decision at normal and overboost samples. Required contracts:

```text
green + normal    -> theme->text
green + overboost -> theme->overboost
red + normal      -> #FF3B30
red + overboost   -> #FF3B30
```

Confirm failure because rendering does not consult the new preference.

- [ ] **Step 4: Implement firmware redraw-safe color selection**

Factor a small color selector used by `draw_vault_needle()`. Include the red preference in committed needle draw state/invalidation so changing it at a stationary angle repaints the old and new needle bounds without stale pixels. Re-run focused rendering tests and the Vault simulator audit; require PASS and zero severe mismatches.

- [ ] **Step 5: Add the API and web mirror**

Extend `themes_get()` and `themes_config_put()` with `vaultNeedleRed`. In `web/app.js`, resynchronize it in every complete theme-config response, add the Vault-only two-option `Needle colour` selector, and make `drawVaultGauge()` use `#FF3B30` throughout when selected. Mirror GET/PUT behavior in `tools/mock_server.py`.

- [ ] **Step 6: Exercise the host web contract**

Run the mock server, fetch `/api/v1/themes`, PUT true and false to `/api/v1/themes/config`, and verify each response. Load the settings page through the Orca browser; confirm the selector renders, updates, and survives a refresh. Verify red below and above overboost in the browser canvas.

- [ ] **Step 7: Regenerate embedded assets and build**

Run the repository embed command, review generated diffs, build the simulator, run the Vault audit, and run `idf.py build`. Expected: all commands exit 0; no manual generated-file edits.

- [ ] **Step 8: Commit the needle feature**

Commit source, generated web assets, and focused host tests as one narrow feature commit.

---

### Task 2: Classify Touch Gestures and Cycle Theme Order

**Files:**
- Modify: `main/boost_gauge.c`
- Modify: `main/boost_gauge.h` only if a host-test seam is required
- Modify: `sim/main.c` or create a focused simulator test following current CMake convention

**Interfaces:**
- Consumes: `boost_theme_at(size_t)`, `boost_theme_count()`, `boost_model_active_theme()`, `boost_model_set_active_theme(const char *)`.
- Produces: mutually exclusive gesture result `NONE`, `TAP`, `SWIPE_UP`, or `SWIPE_DOWN` from start/end/max displacement plus long-press state.
- Produces: modular theme-next/previous selection with authoritative array order.

- [ ] **Step 1: Write failing gesture-classifier tests**

Cover true tap, small vertical drag outside tap slop, upward swipe, downward swipe, horizontal drag, diagonal/ambiguous drag, and long-press suppression. Tests must assert a meaningful drag is `NONE`, not `TAP`, unless it reaches a valid vertical swipe. Confirm expected failures before implementation.

- [ ] **Step 2: Implement the pure classifier**

Use fixed pixel constants sized for 466 x 466, track maximum absolute displacement, require vertical displacement to cross the swipe threshold and dominate horizontal displacement, and ensure a drag never becomes a tap. Re-run focused tests and require PASS.

- [ ] **Step 3: Write failing theme-wrap tests**

Cover Dyno Cell previous -> Big Digit, Big Digit next -> Dyno Cell, every adjacent next/previous pair, and unknown/current-null fallback. Confirm failure before adding the helper.

- [ ] **Step 4: Implement modular authoritative ordering**

Find current theme by ID by walking `boost_theme_at()`. Apply `+1` for swipe up and `-1` for swipe down modulo `boost_theme_count()`. No duplicated ID list. Re-run focused tests and require PASS.

- [ ] **Step 5: Wire LVGL screen events**

Record start point on `LV_EVENT_PRESSED`, maximum displacement during `LV_EVENT_PRESSING`, and classify on release. Preserve long-press brightness. True tap alone calls `reset_peak_ui()`. Valid swipe calls `boost_model_set_active_theme()` once and applies the selected theme. Suppress swipe while GIF playback exists or a transition is active. A swipe never also resets peak.

- [ ] **Step 6: Run simulator gesture/theme cycling exercise**

Exercise repeated up/down cycling through every theme, render after every switch, and require no crash, unknown theme, or stale-pixel mismatch. Build all simulator targets and `idf.py build`.

- [ ] **Step 7: Commit gesture navigation**

Commit classifier tests, authoritative wrap helper, and screen-event integration as one narrow commit.

---

### Task 3: Add Snapshot-Backed Vertical Slide With Safe Cut Fallback

**Files:**
- Modify: `main/boost_gauge.c`
- Modify: simulator/focused test files used by Task 2

**Interfaces:**
- Consumes: gesture direction and selected target theme from Task 2.
- Produces: transition start routine returning whether animation started; failure means caller leaves the already-applied target scene visible as an immediate cut.
- Owns: temporary outgoing/incoming RGB565 PSRAM buffers, image descriptors/objects, animation completion cleanup, and `s_transition_active` guard.

- [ ] **Step 1: Write failing lifecycle/fallback tests**

Test successful start/cleanup, first allocation failure, second allocation failure, snapshot failure, and repeated input while active. Each failure must leave the target theme applied exactly once, `s_transition_active == false`, no transition object, and no retained buffer. Confirm tests fail before implementation.

- [ ] **Step 2: Implement bounded snapshot ownership**

Allocate two full-screen RGB565 buffers through the existing PSRAM-aware allocation convention. Capture outgoing content, apply/rebuild target exactly once, capture incoming content, and create two non-clickable transition image objects. Every failure path frees all resources and exposes the target scene immediately.

- [ ] **Step 3: Animate and clean up**

For swipe up, move outgoing from y=0 to y=-466 and incoming from y=466 to y=0; reverse for swipe down. Keep duration short and fixed. Input remains ignored while active. Completion deletes image objects, frees both buffers, clears descriptors and the active flag, and invalidates the target scene. Preserve current pixel-shift offset and GIF ownership.

- [ ] **Step 4: Run transition stress and forced-fallback exercises**

Cycle up/down repeatedly in the simulator. Force each allocation/snapshot failure seam and verify immediate cuts. Run per-theme audits after transitions and require zero severe mismatches and no memory/lifecycle errors.

- [ ] **Step 5: Build production firmware**

Run the full simulator build, focused tests, audits, and ESP-IDF build. Inspect image size and build warnings. Commit the transition implementation only after these pass.

---

### Task 4: Hardware Acceptance, Documentation, and Final Integration

**Files:**
- Modify: `README.md`
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: production app image, board at COM3 and `http://192.168.50.102`.
- Produces: hardware-observed behavior and cadence/serial evidence recorded durably.

- [ ] **Step 1: Record and preserve the board’s current state**

GET `/api/v1/themes`, `/api/v1/state`, and relevant config before flashing so theme, demo/source, TE, region-dbuf, pixel-shift, and needle choice can be restored.

- [ ] **Step 2: Flash through COM3 and verify boot/control plane**

Build and flash with ESP-IDF 5.5.1. Verify `/state` reports the exact new firmware version and serial reaches `HTTP API ready` without stack overflow, Guru Meditation, `ESP_ERR_NO_MEM`, `send color data failed`, or TE timeout errors.

- [ ] **Step 3: Verify Vault needle behavior and persistence**

From the dashboard choose Signal red. Verify on glass below and above overboost that the needle remains red. Reboot and verify the setting persists. Switch to Phosphor green and verify the existing yellow overboost transition remains.

- [ ] **Step 4: Verify physical touch contracts**

On glass, swipe up through Dyno Cell -> Vault-Tec -> Night City -> Big Digit -> Dyno Cell and down through the reverse, one theme per release. Verify wrap, last-theme reboot persistence, true-tap peak reset, short-drag no-op, horizontal-drag no-op, long-press brightness, GIF suppression, overlap suppression, visible slide direction, and immediate-cut fallback where the failure seam can be exercised safely.

- [ ] **Step 5: Run the display cadence gate**

Set Dyno Cell + demo + regionDBuf to the required gate state and run:

```powershell
C:\Users\aliab\.espressif\python_env\idf5.5_py3.13_env\Scripts\python.exe tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30
```

Expected: median physical FPS at least 60. Inspect serial again for display/memory/TE errors.

- [ ] **Step 6: Restore user state and document observed results**

Restore the recorded pre-test state. Update `README.md` architecture/controls and append one exact evidence row to `AGENTS.md`, including animation fallback behavior and measured cadence rather than inferred claims.

- [ ] **Step 7: Run final verification and commit documentation**

Run focused host tests, simulator audits, generated-asset consistency, `idf.py build`, hardware API/state checks, and `git diff --check`. Commit docs/ledger separately. Request independent whole-branch code review, fix all Critical/Important findings through the same GPT-5.6 Luna implementation agent, re-verify, then fast-forward local `main` and push `origin/main` as explicitly requested for this feature series.
