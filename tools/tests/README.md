# Boost Gauge regression tests

Two runners keep the AGENTS.md guard rails and the regression ledger
(`docs/regression-ledger.md`) executable:

| Runner | When | What it verifies |
|---|---|---|
| `python3 tools/test_suite.py` | **before every commit** | all host-side tests (existing harnesses + ledger-derived contract tests) |
| `python3 tools/check_hardware_gates.py` | **before every release / flash-to-car** | the live board's physical gates (cadence, WebSocket pool, media, OTA, logs, …) |
| `python3 tools/tests/test_version_consistency.py --release` | **before publishing a release** | the SHIPPED artifacts embed the `version.txt` release version (firmware app descriptor, IPA/APK build metadata) and `SHA256SUMS` matches the bytes on disk |

All three are **stdlib-only** (plus the repo's own scripts). Only
`check_hardware_gates.py` talks to a board, and it never flashes; its optional
`--serial` capture is read-only (DTR/RTS off) - if another session owns the port
it reports SKIP, never FAIL. The version gate never needs a board at all.

## Host suite

```bash
python3 tools/test_suite.py                 # full run, one summary table
python3 tools/test_suite.py --verbose       # stream each test's output
python3 tools/test_suite.py --filter gatt   # regex on test basenames
python3 tools/test_suite.py --quick         # skip slow-flagged tests
python3 tools/test_suite.py --list          # show the registry
```

The runner invokes the four pre-existing standalone harnesses as subprocesses
(`test_mock_api.py`, `test_ble_sim.py`, `test_map_conversion.py`,
`test_rtc_epoch.py`) and then the ledger tests under `tools/tests/`. Exit code
is non-zero if anything fails. `--quick` skips the tests flagged slow in the
registry — today `test_obd_ble_lifecycle.py`, which costs ~35 s because the OBD2
driver's own 10 s post-disconnect delay is part of the behaviour it exercises.

### tools/tests/ ledger tests

| Test | Guards (regression-ledger rows) |
|---|---|
| `test_version_consistency.py` | Version drift (2026-10-02): `version.txt` is the single release version. Source mode asserts monotonicity against the highest git tag, that nothing shadows it for ESP-IDF (`PROJECT_VER` override, `CONFIG_APP_PROJECT_VER_FROM_CONFIG`), Android `versionName`/`versionCode`, iOS `project.yml` AND every occurrence in the generated pbxproj, the mock deriving from `version.txt`, that `version.txt` is tracked (an untracked copy falls back to `git describe`), and that no superseded version literal survives in code. `--release` additionally parses the shipped `boost_gauge.bin` + merged-image `esp_app_desc`, the IPA's built `Info.plist`, the APK's `versionName`/`versionCode` via `aapt2`, and re-hashes every `SHA256SUMS` entry |
| `test_web_api_contract.py` | Every `/api/v1` schema the apps parse (`/state` incl. tpms/obd/display, `/config` incl. `appBle`, `/themes` order Dyno Cell→Vault-Tec→Night City→Big Digit→Neon, `/tpms/config` bounds 100-400 kPa / 2000-120000 ms, `/time` + firmware `409 clock_rejected` 5-min rule, `/logs.csv` header, `/media` 409 overlap + repeat DELETE, `/ota` 0xE9 magic, `/restart`, `/network` GET/PUT/DELETE + saved-list semantics) |
| `test_gatt_contract.py` | Three-way consistency of `docs/bluetooth-gatt.md`, `main/boost_app_ble.c`, `tools/ble_gauge_sim`: UUIDs, 480 B/413 rule, BGL1 log format, zone tokens VAC/ATMO/BOOST/OVER, one-in-flight, `api:1` |
| `test_theme_store_invariants.py` | `s_defaults[]` order + names; sport-cluster token absence; `demoFastSweep` persisted separately from `demoMode` (`demo_fast_sweep` vs `demo_mode`) and never reset by `boost_sim_init()`; firmware/mock/sim order agreement |
| `test_nvs_key_inventory.py` | Every NVS key has an ESP_OK-gated restore (absent → default) and the ledger bounds: tpms low/stale, brightness 0-100, `BOOST_PXSHIFT_SEC_*` 30-3600, rotation 0/90/180/270, vignette ≤90, neon layout/preset clamp, saved-count ≤5, MAP supply and calibration schema gating, `app_ble` default off |
| `test_network_semantics.py` | `BOOST_NET_MAX_SAVED == 5`, scan/reconnect suspension while AP clients connected or STA has IP, timer-only auto-reconnect backoff, `BOOST_AP_PASSWORD == "boost1234"`, AP SSID `BoostGauge-%02X%02X`, QR payload format, saved-list MRU/DELETE semantics |
| `test_gesture_constants.py` | `TAP_SLOP_PX 12`, `SWIPE_MIN_PX 48`, 4:5 ratio classifier expressions, `HOLD_DIM_MS 1000`, `QR_HOLD_MS 2200` (ledger/source; AGENTS.md top-text "3 s" is flagged as doc drift), `TPMS_CAPSULE_GROW 2` in firmware + web mirror |
| `test_cadence_guard_math.py` | Pass/fail math of `check_display_cadence.py` (warmup, median ≥60, 59/60 edges, insufficient samples) and `bench_theme_matrix.py` demand coverage (≥95%, zero-demand idle, per-window cap) cross-checked against the real modules on synthetic samples |
| `test_readout_deadzone.py` | dyno-cell readout ±0.1 psi dead zone: fold shape in `arc_readout_display_psi()` (boost_gauge.c) and `arcReadoutDisplayPsi()` (web/app.js), firmware/web band equality, edge-value reference table (incl. the sign-at-edge case a one-band shift would corrupt), wedge/zone excluded from the band, BLE-coex scan contract (default dwell at both `esp_wifi_scan_start(&)` sites, `WIFI_SCAN_ACTIVE_*` gone, background-scan retry); Neon zone colour/id fold (2026-09-09): `neon_zone_rgb`/`neon_zone_id` reference `boost_readout_display_psi` with no raw-`0.05` zone threshold left at those sites, web `neonZoneDisplayPsi` delegates (no second band constant) and both `drawNeonGauge` zone sites consume it |
| `test_sim_panel.py` | `sim/main.c --stream` harness behind `tools/sim_panel.py`: stdout is pure BGFR framing (a leaked firmware log line desyncs it), 466x466 frames with `w*h*4` bytes, a `unit` command actually alters the rendered pixels, and `quit` exits rc 0. SKIPs when the sim binary is not built |
| `test_obd_ble_lifecycle.py` | OBD2 BLE central off→on lifecycle (2026-10-05). Builds and drives the **real** `main/boost_obd_ble.c` on a fake-NimBLE host (`tools/nimble_fake/`, which models `BLE_HS_EDONE` for an existing connection, `EALREADY` for `ble_gap_conn_cancel()`, and no cleanup for an ignored CONNECT event) and requires three invariants: a plain stop→start reaches READY, a connect completing in the **disabled** window still reaches READY, and no connection survives `boost_obd_ble_stop()`. Reproduces the reported permanent failure against the unfixed source (a phantom link, `connect failed: status=0x000e` = `BLE_HS_EDONE`, retrying forever). Flagged **slow** (~35 s: the driver's own 10 s post-disconnect delay is part of the behaviour under test). SKIPs when the sim tree is not configured |

## Hardware gates

```bash
# Full release gates (this Mac without Wi-Fi: add --skip ap)
python3 tools/check_hardware_gates.py --url http://192.168.50.102

# Skip the risky local-machine gates: AP-join (disassociates this Mac's WiFi)
# and BLE advertisement (needs blueutil/system BT data)
python3 tools/check_hardware_gates.py --skip ap --skip ble

python3 tools/check_hardware_gates.py --json           # machine-readable report
python3 tools/check_hardware_gates.py --serial /dev/cu.usbmodem14101   # read-only serial capture
```

Gates (each prints PASS/FAIL/SKIP + measured numbers + the ledger rows it
guards): `boot-health`, `cadence` (30 s soak, median ≥60), `fast-slew`
(per-theme constant-slew sweep, median ≥60, prior theme/config restored),
`ws-pool` (3 clients receive; a 4th is rejected/closed for itself only),
`http-latency` (p50 < 400 ms, informational), `ap-join` (BoostGauge-* SoftAP,
password `boost1234`, DHCP, GET /state, **always** restores the Mac's prior
Wi-Fi in a `finally` block), `ble-advertise`, `media-smoke` (tiny GIF upload →
commit → delete → empty, repeat delete harmless), `ota-state` (bad-magic body
rejected 400; no real OTA image is ever posted), `logs-ring` (strictly
increasing `t_ms`, exact CSV header, DELETE clears; the live 5 Hz ring starts a
fresh rebuild within ~100 ms).

Exit code is non-zero if any gate FAILs (SKIP is allowed). The gate runner
reboots the board via `POST /api/v1/restart` during the per-theme sweep (that
is `tools/bench_fast_motion.py`'s own precondition) and restores the pre-gate
theme/demo/pixel-shift state afterwards.

## Convention

1. Run `python3 tools/test_suite.py` before every commit - it must be green.
   (It includes `test_version_consistency.py`, which fails the moment any surface
   drifts from `version.txt`.)
2. Run `python3 tools/check_hardware_gates.py` (with `--skip ap` on a Mac you
   cannot disassociate, and `--skip ble` where blueutil is unavailable) before
   every release and before flashing the car.
3. Run `python3 tools/tests/test_version_consistency.py --release` before
   publishing: it verifies the SHIPPED bytes (firmware app descriptors, the IPA's
   `Info.plist`, the APK's version metadata, `SHA256SUMS` against the files)
   rather than the sources a maintainer already edited.
4. A hardware measurement claim must quote the gate output, never a host-only
   run.
