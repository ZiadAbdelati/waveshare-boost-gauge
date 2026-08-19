# Network, telemetry, and clock

This page documents the web control plane, the clock/RTC persistence, and the dashboard network contract. The condensed guard rails live in `AGENTS.md`; the full measurement history is in `docs/regression-ledger.md`.

## Cadence and connection contract

The physical gauge samples and updates every **16 ms (~60 Hz)**. Network telemetry is deliberately lower-rate and independently owned: the WebSocket server maintains a fixed pool of **3 clients**, rather than a single dashboard owner. Each client may have at most one in-flight, heap-owned frame; the server performs a bounded broadcast at **62.5 Hz**. A newly connected dashboard MUST NOT close or evict an existing socket, because concurrent or stale tabs otherwise force Live/Fallback churn and can produce out-of-order target jitter.

The browser sends an application heartbeat every **750 ms**; the server consumes it as part of the WebSocket protocol. If the socket is unavailable, the browser uses HTTP `GET /api/v1/state` fallback at **4 Hz**. The connection badge exposes the active path as **Live · WebSocket 60 Hz** or **Live · HTTP 4 Hz**; **Disconnected** means neither transport is delivering state. The canvas renders every `requestAnimationFrame` and uses a short **35 ms** EMA to suppress packet-step jitter without adding the previous 90 ms visual lag. It accepts only targets with a strictly newer `uptimeMs`; after a gap greater than **1 s**, its timing state resets.

The browser canvas interpolates at **60 FPS**, while the sparkline remains intentionally **4 Hz**. These are separate contracts: a smooth browser canvas does not imply 60 Hz network packets, and GIF playback is exclusive to the display path.

Background history logging is a separate **5 Hz** producer: the 18,000-sample RAM ring retains **1 hour** regardless of whether the dashboard is receiving 62.5 Hz WebSocket telemetry or 4 Hz HTTP fallback.

The ring is **432,000 bytes and lives in PSRAM**, allocated once in `boost_model_init()` with `MALLOC_CAP_SPIRAM`. It is written at 5 Hz and read only for export — no DMA, no ISR, not latency-critical — so it has no business in internal DRAM, which is shared with Wi-Fi and display DMA. A failed allocation leaves the pointer NULL and disables logging rather than failing boot; every access stays under the existing `s_lock`.

## Fourth-client behavior

The firmware owns a fixed pool of **3 WebSocket clients**. A fourth handshake is rejected/closed for that newcomer only; the three existing sockets stay open and continue receiving telemetry. The browser keeps its fallback poll at **4 Hz** (`POLL_FRAME_MS = 250`) and retries WebSocket connection every **1 s**. A successful `/api/v1/state` fallback sample shows **Live · HTTP 4 Hz**; **Disconnected** means both WebSocket and HTTP state polling are unavailable. A retry attempt must not downgrade healthy fallback, and a restored socket shows **Live · WebSocket 60 Hz**.

Slots are returned to the pool through a single `state_ws_release_locked()`, which clears `fd`/`payload`/`inflight` together and bumps a per-slot generation counter. The generation is what makes a slot safe to reuse while an async frame for the previous occupant is still queued in the httpd task: the late completion no longer matches the slot, so it frees only its own buffers. Never release a slot by clearing `fd` alone — the completion callback can then never match, and `inflight` stays set, which permanently removes that slot from the pool.

## Network modes

| Mode | When | URL |
|------|------|-----|
| **APSTA** | NVS/secrets STA SSID set | SoftAP + LAN STA · serial `BOOST_WEB_IP=` |
| **SoftAP only** | No STA SSID | Join `BoostGauge-XXXX` / `boost1234` → `http://192.168.4.1/` |

**Settings page** (`/settings.html`): cockpit navigation uses the gear icon; Settings is a real document rather than a show/hide panel, so browser back/forward works normally. It owns Wi-Fi controls (mode, SSID, password with blank-keep, scan, reconnect) and gauge fields `psiMin` / `psiMax` / `psiOverboost` (defaults **−15 / 10 / 8**) plus `zeroAngle` (default **236.25°**, allowed **180–315°**). Zero position moves the dial notch without changing sensor pressure; vacuum and boost rescale on their own sides. The optional boost-half midpoint label is omitted when it would overlap the overboost label. Invalid range PUTs are rejected with **400**. Settings persist in NVS (`boost_wifi` for network; boost config blob for gauge scale).

**Seed credentials** (optional, gitignored): `main/boost_wifi_secrets.h` from `main/boost_wifi_secrets.h.example`, used only when NVS has no Wi-Fi blob yet.

```bash
cp main/boost_wifi_secrets.h.example main/boost_wifi_secrets.h
# edit SSID/password once, then:
idf.py build flash monitor   # look for BOOST_WEB_IP=192.168.x.y
```

## Dashboard notes

- Responsive **instrument-cluster** layout: sticky gauge + sparkline on the left; cockpit console cards reflow from one column (mobile) up to three (ultrawide, capped at 2100 px). The cockpit's gear opens the separate `/settings.html` document; browser history returns to the unchanged live cockpit. No horizontal overflow at any width.
- Mobile: `overflow-x: hidden`, no horizontal rubber-band empty space.
- Dim-schedule Start/End stay side-by-side; time inputs capped for iOS Safari.
- Brightness/theme/schedule apply off the HTTP worker so the UI stays responsive.
- Sensor/model/WebSocket publication runs outside the LVGL worker, so GIF playback cannot stall dashboard telemetry. Network telemetry is decoupled from the physical 16 ms gauge loop. Station Wi-Fi modem sleep is disabled while the gauge runs; this favours live-control latency over Wi-Fi power saving.
- Browser rendering caps device pixel ratio (DPR) at **2**; the sparkline is limited to **4 Hz**, and the browser GIF preview is disabled. In the verified 30 s dashboard soak, the main-thread probe peaked at **9 ms** with no freezes longer than **500 ms**.
- **Shared error box lifetime.** `#errorBox` is written by two kinds of producer, and each retracts only what it raised. `showError(msg, source)` / `clearError(source)` take `ERR_LIVE` (unattended telemetry: `pollState`, WebSocket frames) or `ERR_USER` (the outcome of a gesture). `ERR_USER` outranks `ERR_LIVE` in both directions, so a poll can neither overwrite nor erase the message an operator is reading; a `showOk()` or a click on the box dismisses it. A transport error raised by `pollState` is `ERR_LIVE` and still self-clears the moment polling recovers. The connection badge remains the designated live-transport indicator and this path never touches it.

### Host-only UI development

```bash
python3 tools/mock_server.py --host 127.0.0.1 --port 18080
python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h
```

`mock_server.py` stubs the full dashboard API — including `/api/v1/network`, `/network/scan`, `/network/reconnect`, and config fields `psiMin`/`psiMax`/`psiOverboost`/`zeroAngle` — so `refreshAll()` succeeds and cockpit + settings views render host-only (no WebSocket, so the badge falls back to `Live · HTTP 4 Hz`).

Do not expose SoftAP/STA HTTP or OTA beyond a trusted LAN; there is no per-request auth yet.

## Clock source, persistence, and CSV timestamps

The wall clock is battery-backed by a **DS3231 RTC on the sensor I2C bus** (GPIO18/17, address 0x68). At boot, after the sensor bus comes up and *before* boot brightness is decided, the firmware seeds `settimeofday` from the DS3231 when it is present and its time is valid (oscillator-stop flag clear, BCD registers sane, date ≥ 2023) and marks the clock trusted. A night boot with a set RTC therefore comes up dim from the first frame with **no Wi-Fi involved**.

The dashboard's **Sync Time** control remains the calibration action: its `POST /api/v1/time` supplies browser `Date.now()` plus the configured UTC offset. Firmware applies the epoch with `settimeofday`, saves it (plus the monotonic checkpoint and configuration) in NVS, and **writes the DS3231** so the time survives power-off. The NVS epoch/monotonic checkpoint is refreshed at every RTC seed as well, keeping it a warm fallback if the RTC is later removed or fails.

The timezone is set once from a **Time zone dropdown** on the cockpit. Each option carries a POSIX TZ string (e.g. `EST5EDT,M3.2.0/2,M11.1.0/2`) persisted as `timezoneTz` in NVS; the firmware applies it via `setenv("TZ")+tzset()` and uses `localtime()` for the dim schedule, CSV timestamps, and the reported effective offset, so **DST transitions are handled automatically**. The dashboard refresh no longer overwrites the selection with the browser's live offset. `timezoneOffsetMinutes` remains the stored standard offset for API/back-compat; `/state` reports the current effective (DST-aware) offset. A legacy config without a TZ string falls back to a synthesized fixed offset.

Without an RTC (absent, failed, or never set — the oscillator-stop flag), boot restores the frozen NVS epoch and advances it only by the monotonic delta when applicable, the clock stays untrusted, the schedule defaults to bright, and a browser Sync (over LAN or SoftAP) is the only way to establish the time. `epoch_ms_now()` and CSV timestamps always report the best-effort clock.

CSV `timestamp_local` is formatted as `%Y-%m-%dT%H:%M:%S` with no timezone suffix; `utc_offset_minutes` remains a separate CSV field. The CSV export returns up to **18,000 rows** (1 hour at 5 Hz). Hardware verification must show `badTimestampCount: 0`.

## Finishing an OTA

`POST /api/v1/ota` streams the image, validates it, and selects the boot partition, but the new firmware does not run until the device reboots. `POST /api/v1/restart` does that (400 ms deferred, so the response is delivered first); the dashboard calls it automatically after a successful upload. A 1.6 MB image uploads in ~9 s (~190 KB/s).

To confirm an OTA actually took effect, check the boot log for the partition offset: `Loaded app from partition at offset 0x420000` is ota_1, i.e. the uploaded image. Still booting `0x20000` means the serial-flashed image is running and the OTA proved nothing.

## Debug snapshot endpoint

`GET /api/v1/debug/snapshot` re-renders the live screen into a PSRAM buffer and streams it as raw little-endian RGB565 (466×466). `tools/fetch_panel_snapshot.py` turns that into a PNG. This is how the physical face is verified without photographing the panel. Requires `CONFIG_LV_USE_SNAPSHOT=y`.
