# TPMS and OBD-II

## TPMS view

`main/boost_tpms_ui.c/.h` renders the processed 466×466 powertrain art on black, with four tire-shaped status capsules (`FL`, `FR`, `RL`, `RR`) aligned to the 80%-scale chassis. The physical readouts use the compiled Saira SemiCondensed Bold face and show number-only PSI values: green is normal, red is low, amber is stale, and gray is offline. `STALE` retains its last number; a wheel that has never reported shows `--.-`. The browser uses the same native-466 geometry and redraws when the image finishes loading.

The framework is split into `main/boost_tpms.c/.h` (service/model), the deterministic `main/boost_tpms_mock.c/.h` provider (`NORMAL`, `STALE`, and `DISCONNECTED` scenarios), and the pure `main/boost_tpms_protocol.c/.h` protocol/conversion layer.

## BLE OBD-II transport

`main/boost_obd_ble.c/.h` is a NimBLE central transport: it scans by advertised name `VLINK`/`OBD`/`ELM` or OBD service UUID `0x18F0`/`0xFFF0`/`0xFFE0`, connects unauthenticated, performs runtime service/characteristic discovery with a known-UUID table plus a first-writable/first-notify fallback (adapter UUIDs are often unpublished), subscribes via CCCD-write, and keeps an NVS-persisted peer for fast reconnect.

`main/boost_obd_elm.c/.h` frames ELM327 request/reply on that byte stream (`>`-paced, one command in flight). `main/boost_obd.c/.h` runs the poll loop: the standard init sequence (`ATZ`, `ATE0`, `ATL0`, `ATS0`, `ATH0`, `ATSP0`), the MX-5 ND TPMS DID reads (`ATSH 720` + `22 2A xx`, published through the conversion path below), and generic mode-01 PIDs plus `ATRV` battery voltage for the web cockpit.

ESP32-S3 is BLE-only, not Classic/SPP, so the adapter must expose a BLE GATT service — the vLinker FD+ does; the FS family (Classic BT) does not. The firmware is intentionally adapter-agnostic: it discovers at runtime and requires a verified profile before trusting vehicle responses. Any BLE ELM327 adapter (vLinker FD+, Veepeak, OBDLink MX+, cheap HM-10-style dongles) is picked up by the name/UUID scan and runtime discovery. The remembered adapter MAC is a convenience, not a binding: if a direct connect fails, the firmware falls back to scanning, and the first adapter that answers overwrites the stored MAC.

The OBD poll task is started after the web control plane so a BLE init failure can never precede OTA recovery.

## For the Mazda MX-5 ND

UDS `ReadDataByIdentifier` uses ECU/header `0x720`, with DIDs `FL=0x2A05`, `FR=0x2A07`, `RL=0x2A06`, and `RR=0x2A08`. Each answer is a single data byte (`0x62 DID-hi DID-lo value`, a 4-byte response; the parser also accepts a padded two-byte value). Conversion:

```
pressure_kPa = raw * 1.373 + additive replacement-sensor offset
pressure_psi = kPa * 0.145037738
```

The low-pressure alert threshold (default 220 kPa ≈ 32 psi, red below / green at or above) and the staleness window (default 15 s, sized above the ~4.5 s poll rotation so a single dropped DID never flips the page amber) are persisted in `boost_tpms` NVS and configurable as PSI in Settings (`/settings.html`). The drawn capsules inflate +2 px over the art's tire bounds so anti-aliased white edge pixels do not peek through.

## Header switching and protocol detection

The TPMS DID set is **CAN-only**: `720`/`7DF` are ISO 15765 CAN identifiers, and issuing them as `ATSH` on a K-line bus (ISO 9141-2 / ISO 14230) corrupts the ELM header so the ECU never answers. The poll loop re-queries `ATDP` after the `0100` prime locks the protocol (the init-time read reports `AUTO` because `ATSP0` has not locked anything yet), then only switches headers when the locked protocol is `ISO 15765` (CAN). On a legacy K-line/J1850 bus it keeps the auto-detected default header for the mode-01 PIDs (so a pre-CAN car like the 2003 Toyota Camry reads rpm/speed/coolant correctly) and skips the Mazda TPMS DID phase entirely.

## Link liveness vs. decode success

`obd.valid` and `obd.ageMs` track **link liveness**, not decode success: they are driven by the ELM reply timestamp (any complete `>`-terminated reply, including `NO DATA`), so the readouts stay populated while the link answers and blank only when the link is actually down/stale (>15 s). A `NO DATA` value reads `0` on the bench; in the car the PIDs answer instantly. DID/PID/battery query timeouts are 2 s, sized above the FD+'s worst-case "searching" delay so a slow `NO DATA` reply is consumed cleanly instead of arriving as a stray that corrupts the next request.

## Enable / disable

The link is controlled by the persisted `tpmsBle` setting (default **off**, so a fresh boot never touches the radio). Flipping it in Settings or via `PUT /api/v1/themes/config` starts/stops the BLE central immediately and survives reboots. When enabled and an adapter answers, the TPMS page switches from the simulated provider to real vehicle data and `/state.obd` carries the live PIDs; when disabled, the mock runs as before. `BOOST_TPMS_BLE_ENABLED` in `main/Kconfig.projbuild` (default `y`) is the compile gate: set it `n` to build a BLE-less image (the `bt` component is still required by `main` because the discovery pass runs before Kconfig, but it contributes nothing with `CONFIG_BT_ENABLED=n`).

Only the TPMS DID set is vehicle-specific (Mazda MX-5 ND); the mode-01 PIDs and `ATRV` work on any OBD-II car.

## Simulator

```bash
./sim/build/boost_gauge_sim --tpms [normal|stale|disconnected]
```

The default `--screenshot` mode also emits `tpms_*.raw` for all three scenarios. On hardware, `main/main.c` calls `boost_tpms_init()` / `boost_tpms_start()` and runs a 250 ms LVGL timer that ticks the mock provider (when the BLE link is off) or ages the BLE-published snapshot, then feeds `boost_page_update_tpms()`.
