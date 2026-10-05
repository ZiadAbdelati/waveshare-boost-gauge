# GUI guide: gestures, pages, and the simulator

## Two-page layout and gestures

`main/boost_page.c/.h` owns two persistent pages: page 0 is the boost gauge and page 1 is the TPMS view. On the boost page, swipe **LEFT** to TPMS; on TPMS, swipe **RIGHT** to return to boost. There is no wrap-around: outward swipes at either end are ignored. Vertical theme swipes work only on page 0, and a tap resets peak only on page 0. A one-second hold toggles brightness on either page. A two-finger tap-and-hold (both fingers down) for three seconds shows a full-screen QR code that joins the SoftAP (`BoostGauge-XXXX` / `boost1234`); the 1 s hold-to-dim is suppressed while both fingers are down.

While the settings overlay is open, a **horizontal** drag steps one page (left = forward, wrapping); a **vertical** drag does nothing there (the theme stays put — an opaque settings cover gives no affordance for a theme change, and an accidental diagonal swipe read as the overlay "changing themes"); and a **fresh tap** dismisses. A drag never acts as a tap, by either route — a classified swipe or simply movement past the 12 px tap slop — and that applies identically on the overlay background and on the square switches: a 20–47 px flick leaves the overlay open and leaves a switch's setting alone. One gesture is one touch-down — the origin and the one-shot classification latch are seeded at every touch-down and cleared at release, so a background flick can never be measured from the previous gesture's touch-down point, and a page-stepping drag cannot re-arm itself and step twice (`show_qr()` is also the mid-drag rebuild, so it must not touch gesture state; the rebuild calls `qr_gesture_rebuild()` and the classifier consults its latch before re-seeding).

Pointer-device events own the hold deadline independently of target jitter, and brightness is committed only after the panel command succeeds. The page coordinator forwards MAP samples only to the boost scene and TPMS snapshots only to the active TPMS scene.

Theme swipes use the ordered `boost_theme_at()` table, which is also the order emitted by `/api/v1/themes` and consumed by the web picker. The classifier tracks maximum movement during the press, not only the release coordinate: only movement within the 12 px tap slop resets peak, movement from 12 through 47 px is a rejected drag, a valid predominantly vertical drag at 48 px or more changes one theme, and horizontal or ambiguous drags do nothing. Returning to the start after a meaningful drag is still a drag, not a tap. Theme changes persist through the active-theme model path and rebuild the LVGL scene in the existing locked LVGL context.

There is intentionally no crossfade or slide animation. The live gauge uses partial internal-DMA strips, and rich faces cache static art in PSRAM; a smooth transition would require unsafe full-frame dual-scene storage or an expensive full-panel repaint that would violate the 16 ms display budget. The bounded transition is an immediate single-scene swap rather than a misleading effect.

GIF playback suppresses tap-reset, theme swipes, and page switches, but the one-second hold-to-dim still works over media.

## Dashboard behavior

- **Live / Fallback / Disconnected:** the connection badge always exposes the active transport.
- **Canvas smoothing:** 35 ms EMA over WebSocket targets; rendering is decoupled from packet cadence.
- The **gear icon** opens `/settings.html` (Wi-Fi, gauge range/zero, themes, TPMS, clock) — a separate document so browser back/forward works normally.

## Desktop simulator

Same LVGL UI, no board required:

```bash
cmake -S sim -B sim/build
cmake --build sim/build -j"$(nproc)"
./sim/build/boost_gauge_sim --screenshot preview/sim
python3 sim/raw_to_png.py preview/sim
```

Headless LXC/CI works as-is (memory display + snapshot). Windowed:

```bash
xvfb-run -a ./sim/build/boost_gauge_sim --window
```

See `sim/README.md`. Rendered screenshots live under `preview/sim/`.

The sim does **not** inherit the device's persisted settings; it calls `boost_theme_init()` at startup and renders the defaults unless told otherwise. Useful flags: `--theme <id>`, `--neon-layout tube|segments|marquee`, `--neon-preset 0..3`, `--neon-spin` (enable the marquee chase), `--tpms [normal|stale|disconnected]`, and `--screenshot [dir]`. This is the fastest verification loop — use it before burning a flash cycle.
