# Agent guidance

This repository is an ESP-IDF 5.5.1 firmware/dashboard for an ESP32-S3 AMOLED boost gauge. These rules are load-bearing. A fresh agent must be able to resume from this file alone; do not rely on chat history.
Verified repair baseline is firmware `0.2.0-web`; preserve that identity in hardware/release notes when referring to these measurements.

## Durable coordination and delegation

- For every non-trivial change, the coordinator MUST use `Task` subagents heavily: delegate independent research, implementation slices, and verification/testing slices in parallel where possible. The coordinator owns the top-level contract, integration, and final acceptance; do not delegate away the architecture decision.
- Before assigning work, map the affected files and symbols. Announce ownership through the coordination channel. One agent owns a file at a time; agents MUST NOT overwrite unexpected work. Re-read after another agent edits, and integrate only the intended diff.
- Every subagent must return observed evidence, decisions, files touched, and remaining risks. Continuously append durable findings/decisions/architecture notes to this `AGENTS.md` and the relevant `README.md` section while work is in progress; do not keep critical reasoning only in the session. Whenever architecture or a regression changes, update both the README and the AGENTS ledger in the same change.
- No unverified edit is acceptable. First inspect the existing implementation and callers, then make the smallest source change, then exercise the affected path. Never invent measurements, claim a test was run when it was not, or replace a failing guard with a weaker one.
- Do not edit generated C assets or release binaries by hand. Do not add compatibility shims, alternate implementations, or a second convention beside an existing one.

### Source ownership

- `main/boost_display.c/.h`: AMOLED bring-up, LVGL/DMA buffers, panel transfer and display lock.
- `main/boost_gauge.c/.h`: gauge rendering and exclusive GIF playback lifecycle.
- `main/boost_media_store.c/.h`: raw `media` partition format, upload transaction, CRC, mapping, and deletion.
- `main/boost_web.c`: HTTP/WebSocket API and upload/delete request serialization.
- `main/boost_model.c/.h`: sensor/model state and publication.
- `web/`: dashboard source. `main/generated_web_assets.c/.h` are generated outputs only.
- `tools/embed_web.py`, `web.mk`: web asset regeneration. `release/`: explicitly produced release artifacts only.

## Cadence contract

Keep these rates distinct; never use one as a substitute for another:

- Sensor sampling and the physical gauge render/update path: every **16 ms**, approximately **60 Hz** (the physical gauge is the hardware gate).
- Network telemetry: a fixed pool of **3 WebSocket clients**, each with at most one in-flight heap-owned frame; bounded broadcast targets **10 Hz**.
- Browser application heartbeat: **750 ms**, consumed by the server.
- Browser live canvas: renders on every `requestAnimationFrame`, uses EMA smoothing, and accepts only strictly newer `uptimeMs` targets; timing resets after a gap greater than **1 s**.
- HTTP fallback state polling: **4 Hz**.
- Sparkline: **4 Hz**.
- Browser connection badge: only **Live** or **Disconnected**; it MUST NOT expose rate labels.
- Browser device-pixel ratio: cap at **2**.
- Browser GIF preview: disabled; do not reintroduce a large client-side preview path.

Do not add display timers/dividers, throttle the 16 ms gauge readout, or judge WebSocket/canvas cadence with the physical-display guard. GIF playback is an exclusive full-frame path and is not expected to satisfy the live-gauge FPS threshold. A new dashboard MUST NOT evict an existing WebSocket client: single-owner behavior caused concurrent/stale tabs to force Live/Fallback churn and out-of-order target jitter.

### Fourth-client and GIF regression invariants

- The WebSocket pool is exactly three clients. A fourth handshake may be
  rejected/closed for the newcomer, but MUST NOT close or disturb any existing
  client. Browser fallback remains `POLL_FRAME_MS=250` (4 Hz), while WebSocket
  retries are 1 s. A successful HTTP state sample keeps the badge `Live`; a
  retry attempt MUST NOT mark it `Disconnected`. `Disconnected` requires both
  WebSocket failure and HTTP state-poll failure.
- GIF decompression is not project-written. `main/boost_gauge.c` supplies the
  custom locked widget/descriptor integration; `main/boost_media_store.c` owns
  raw dual-slot CRC publication and `esp_partition_mmap`; LVGL
  `managed_components/lvgl__lvgl/src/libs/gif/lv_gif.c` and bundled
  `AnimatedGIF/src/gif.c` perform parsing, LZW, frame timing, and composition
  (`GIF_openRAM`/`GIF_playFrame`). Local LVGL edits zero the full framebuffer
  and disable turbo (`pTurboBuffer = NULL`) because turbo failed delta/disposal
  composition. Keep mapped bytes alive through widget destruction: lock display,
  destroy widget, then unmap. Do not describe this as a custom decoder.

## AMOLED DMA invariants and hardware gate

- Start the gauge with `boost_display_start()`, never stock `bsp_display_start()`.
- LVGL draw buffers MUST stay in internal DMA-capable memory (`use_psram = false`). Production strips are 20 lines: 18,640 bytes per buffer / 37,280 bytes double-buffered. Keep transfers capped to one strip and queue depth at 4 (`CONFIG_BSP_LCD_TRANS_QUEUE_DEPTH=4`). Preserve the display lock and internal-memory reservation needed for Wi-Fi and DMA to coexist.
- Preserve LVGL partial refresh and the CO5300 even/odd rounder. Do not move full-frame buffers to PSRAM, enlarge strips for an unmeasured speed claim, restore a marker/knob, replace the arc with retained line segments, or invalidate stationary arc endpoints without before/after evidence.
- After any display-path change, flash hardware and run the cadence guard for a 30-second live-gauge soak:
  `python3 tools/check_display_cadence.py --url http://<board-ip> --seconds 30`
  Accept only a sustained median of at least 60 physical FPS; the verified reference check was min 61, median 63 over 112 samples. Serial must show the internal-DMA path and no `ESP_ERR_NO_MEM` or `send color data failed`. A hardware result is required before declaring the change complete.

## Raw media is authoritative (never SPIFFS)

The media store MUST remain a raw dual-slot partition; do not reintroduce SPIFFS, a second staging file, or direct replacement of `active.gif`.

- Partition label is `media`, offset `0x820000`, size `0x7E0000`, split into two slots. Boot scans headers and accepts only CRC-valid headers, selecting the newest generation.
- Upload targets the inactive slot. Erase only the required aligned range, stream payload and CRC, and write the committed header last. A header is not publication: commit is atomic only after payload and validation are complete.
- Playback maps the committed payload with the raw partition mmap API and gives LVGL a variable image descriptor. Do not free mapped storage until LVGL has been destroyed/unmapped.
- Abort or any failed upload MUST preserve the previously committed GIF. Delete removes the committed slot only after playback is stopped; repeated deletes must remain harmless (two repeated deletes are verified).
- The verified hardware benchmark is a complete **1,379,129-byte** GIF upload through the raw dual-slot store in **7.504 s**. The earlier SPIFFS history includes approximately **172 s** writes; an earlier tuned SPIFFS result of **7.37 s** was fragile. The raw store is the architectural fix, not an optimization invitation to restore SPIFFS.

### Upload/delete state machine

`Idle → Uploading → Validating/Committing → Published → Playback` is the only publication path. `Uploading` is serialized; an overlapping upload or delete is rejected by the server with **409**. On failure or cancellation, transition through abort/cleanup back to `Idle` while retaining the prior committed slot. In the browser, clicking Delete during upload MUST abort the XHR, wait for its settlement, then issue `DELETE`; never fire concurrent replacement/delete requests. Preserve this ordering and the server-side 409 guard.

## Web assets and release artifacts

- Edit `web/index.html`, `web/app.js`, and `web/styles.css`, then regenerate embedded assets with:
  `python3 tools/embed_web.py web main/generated_web_assets.c main/generated_web_assets.h`
  Review the generated diff; never hand-edit `main/generated_web_assets.c/.h`.
- A release is not complete until the verified ESP-IDF build produces the app image and the release directory contains the bootloader, partition table, OTA data, app image, merged full-flash image, flash helper, and refreshed `SHA256SUMS`. The merged image is for resetting the complete layout; later web OTA uses the app image, not the merged image. Do not publish artifacts from an unverified build or claim hardware behavior from a host-only run.
- Hardware release verification must cover boot, network access, the live physical cadence gate, media upload/abort/delete behavior, and serial error absence. Record measured outputs in the README and this ledger before committing.

## Commit hygiene

Keep commits narrow and reviewable: source, generated web output, documentation/ledger, and release artifacts should be separable when practical. Never mix drive-by formatting or unrelated refactors with a regression fix. A commit that changes web sources must include regenerated embedded assets; a commit that changes architecture or a regression must include the README/AGENTS ledger update. Before handoff, report exact files changed, commands actually run, hardware versus host-only evidence, and any unverified risk.

## Chronological regression ledger

| Sequence | Symptom / finding | Root cause | Fix | Guard now required |
|---|---|---|---|---|
| Earlier | AMOLED became half-white/half-green with `ESP_ERR_NO_MEM` and `send color data failed`. | Stock BSP placed LVGL buffers in PSRAM; ESP32-S3 SPI GDMA could not stream them and the bounce-buffer path failed under partial refresh. | Dedicated `boost_display_start()` with internal DMA buffers, bounded strips, and the existing partial-refresh/rounder path. | Hardware 30-second soak, no serial display errors, and median physical cadence ≥60 FPS. |
| Earlier | Large media writes reached approximately 172 s; a tuned SPIFFS run measured 7.37 s but remained fragile. | SPIFFS garbage collection/large-file staging was the bottleneck and was not a robust atomic media store. | Raw `media` partition with inactive-slot streaming, payload CRC, newest-valid-generation scan, and committed-header-last publication. | Complete 1,379,129-byte hardware upload in 7.504 s; verify reboot selection and preservation on abort. |
| Subsequent | Upload replacement and Delete must not race, and a canceled transfer must not destroy the old GIF. | Publication and request ordering must be serialized; an incomplete inactive payload is never a committed media slot. | Browser aborts XHR and waits for settlement before DELETE; firmware serializes operations and returns 409 for overlap; abort preserves the old slot. | Exercise cancellation, overlap rejection, and two repeated deletes on hardware. |
| Current architecture | Physical, network, browser, fallback, and sparkline rates can be mistaken for one cadence. | Different producers and renderers have different budgets. | Explicit 16 ms/~60 Hz physical path; fixed 3-client WebSocket pool with one heap-owned in-flight frame per client, 750 ms application heartbeat, and 10 Hz bounded broadcast; rAF/EMA smoothing with ordered `uptimeMs` targets; 4 Hz HTTP/sparkline paths; DPR cap 2 and GIF preview disabled. | Keep rate-specific metrics and verify physical cadence separately; dashboard soak reference has max main-thread probe 9 ms and no freeze over 500 ms. |
| Current architecture | Concurrent or stale dashboards evicted each other, causing Live/Fallback churn and out-of-order target jitter. | Single-owner WebSocket server forcibly closed the old file descriptor whenever a new dashboard connected. | Fixed pool of 3 WebSocket clients; each client has one in-flight heap-owned frame, bounded 10 Hz broadcast, and a 750 ms browser heartbeat consumed by the server. | Two concurrent dashboards remained WebSocket OPEN for 30 s with heartbeat active and no polling; client A uptime `144367→174367`, client B stayed OPEN at `182063`; browser rejects non-newer targets and resets smoothing timing after gaps over 1 s. |
| Current architecture | Local CSV timestamps carried a misleading timezone suffix and time behavior was unclear across reboot. | Timestamp formatting conflated local text with offset metadata; dashboard open was mistaken for synchronization. | `timestamp_local` uses `%Y-%m-%dT%H:%M:%S` without suffix; `utc_offset_minutes` stays separate. Sync Time POST supplies `Date.now()` and configured offset; firmware calls `settimeofday`, saves epoch + monotonic checkpoint/config to NVS, and restores/advances by monotonic delta when applicable. | Hardware CSV: 1,800 rows, `badTimestampCount 0`, offset `-300`; no RTC/NTP resync in this path, so only the Sync Time control synchronizes. |
| Current architecture | A fourth dashboard appeared to fail completely or churned `Disconnected` while HTTP fallback was healthy. | The three-client pool rejects only the newcomer, but browser fallback lost its 250 ms constant and reconnect attempts overwrote healthy fallback status. | Restore `POLL_FRAME_MS = 250`; keep `Live` after successful state polls; do not downgrade status merely because a retry starts. | Hold clients 1–3 open, open client 4, verify clients 1–3 remain open while client 4 polls at 4 Hz and retries every 1 s; only total transport failure may show `Disconnected`. |
| Current architecture | GIF playback ownership was unclear and could be described as a project-written decoder. | The pipeline mixes custom storage/UI/display integration with locally patched third-party LVGL and AnimatedGIF decode layers. | Document the boundary: custom dual-slot mmap and widget lifecycle; LVGL timer/wrapper; AnimatedGIF parsing/LZW/composition; zeroed full canvas and standard non-turbo path for delta/disposal correctness. | Verify `GIF_openRAM`/`GIF_playFrame`, keep mapping alive through widget destruction, test delta/disposal GIFs, and keep the internal-DMA CO5300 flush separate from decode changes. |
| Current architecture | Reflashing while a committed GIF existed caused a repeatable `main` task stack overflow before networking started. | Boot-time payload validation put a 4,096-byte CRC scratch buffer on the 3,584-byte ESP-IDF main-task stack. | Validate the committed payload through a temporary read-only `esp_partition_mmap` and CRC it directly, then unmap; no full sector lives on the stack. | Reboot with committed media present and require `HTTP API ready` with no stack overflow, panic, or reset loop. |