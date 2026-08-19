# Documentation

Detailed development notes, architecture, and measurements for the Boost Gauge project. The top-level `README.md` is the user-facing quick start; everything substantive lives here.

## Getting started

- [Architecture](architecture.md) — scene system, cached faces, drawing/invalidation rules, cost model
- [Display & cadence](display-and-cadence.md) — AMOLED bring-up, internal-DMA buffers, the 60 Hz render contract
- [Network & telemetry](network-and-telemetry.md) — WebSocket/HTTP control plane, clock/RTC persistence, GIF upload
- [GIF playback](gif-playback.md) — pipeline and animation performance contract
- [TPMS & OBD](tpms-obd.md) — BLE adapter support, Mazda MX-5 ND DID set, simulator
- [Sensors & calibration](sensors-and-calibration.md) — MAP sensor bus, GM conversion, atmosphere calibration
- [GUI guide](gui-guide.md) — gestures, two-page layout, dashboard notes
- [Themes](themes.md) — theme system, Neon internals, UI design tokens
- [Release notes](release-notes.md) — per-version highlights

## Engineering history

- [Regression ledger](regression-ledger.md) — the full measurement history behind every guard rail

Also see the top-level `AGENTS.md`, which carries the currently-actionable invariants (guard rails) repo-wide.
