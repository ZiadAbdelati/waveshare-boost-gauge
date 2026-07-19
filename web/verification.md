# Web Control Plane Verification

This checklist is for firmware **0.2.0-web** and the current raw-media/WebSocket
control plane. Run the live checks against a board on a trusted LAN or its
SoftAP; the mock server only covers static assets and simple HTTP wiring.

## Environments

| Environment | URL | Real firmware? |
|-------------|-----|----------------|
| STA/LAN | `http://<BOOST_WEB_IP>/` | Yes |
| SoftAP | `http://192.168.4.1/` | Yes |
| Mock | `http://127.0.0.1:18080/` | No |

## Baseline and API smoke

Against `BOOST_WEB_IP` (or `192.168.4.1` in SoftAP mode), confirm:

1. `/`, `/app.js`, and `/styles.css` load from local assets.
2. `GET /api/v1/state`, `/config`, `/network`, `/themes`, `/logs`, and
   `/media/status` return valid JSON.
3. `POST /api/v1/time` returns an `epochMs` close to the host clock.
4. `PUT /api/v1/config` changes brightness and the subsequent state reflects
   it; apply the change asynchronously without making the dashboard unusable.
5. `PUT /api/v1/themes/active` succeeds for every returned theme id.
6. Enable a whole-day dim schedule and then disable it; the physical
   brightness follows each change.
7. `GET /api/v1/logs?limit=120`, `DELETE /api/v1/logs`, and
   `GET /api/v1/logs.csv` all work.
8. `PUT /api/v1/network` with `keepPassword:true` returns promptly and an
   unchanged STA remains connected.
9. An invalid OTA body is rejected with 4xx.

Example read-only/API probes:

```sh
BOARD_URL="http://${BOOST_WEB_IP:?set BOOST_WEB_IP}"
curl -fsS "$BOARD_URL/api/v1/state"
curl -fsS "$BOARD_URL/api/v1/config"
curl -fsS "$BOARD_URL/api/v1/network"
curl -fsS "$BOARD_URL/api/v1/themes"
curl -fsS "$BOARD_URL/api/v1/media/status"
curl -fsS "$BOARD_URL/api/v1/logs?limit=120"
curl -fsS "$BOARD_URL/api/v1/logs.csv" -o /tmp/boost-gauge-log.csv
```

## Raw-media upload and migration checks

Use a valid 466 × 466 GIF fixture whose size is exactly **1,379,129 bytes**.
The browser preview is intentionally disabled. Record the `time_total` from
the upload and verify the returned status reports the committed media:

```sh
GIF='/path/to/IMG_5325-ezgif.com-optimize (2).gif'
test "$(stat -c%s "$GIF")" -eq 1379129
curl --fail --silent --show-error \
  --output /tmp/media-upload.json \
  --write-out 'upload_time_total=%{time_total}\nhttp_code=%{http_code}\n' \
  -X POST "$BOARD_URL/api/v1/media" \
  -H 'Content-Type: image/gif' \
  -H 'X-Filename: full-fixture.gif' \
  --data-binary @"$GIF"
curl -fsS "$BOARD_URL/api/v1/media/status"
```

Latest hardware result: the full **1,379,129-byte** upload completed in
**7.504 s** through the raw dual-slot partition. This is the current speed
baseline; do not substitute the earlier fragile tuned-SPIFFS result (7.37 s)
or the prior SPIFFS regression (about 172 s).

Defend atomicity and cancellation with an existing committed GIF:

1. Upload a small valid GIF and confirm `media/status` says `present:true`.
2. Start the full upload, then click **Delete** before it completes. The
   browser must abort the XHR, wait for that request to settle, and only then
   send `DELETE /api/v1/media`.
3. Confirm the aborted upload does not replace the prior GIF. The previous
   committed media is preserved and restored after the abort; the subsequent
   delete then removes it.
4. Issue Delete twice more after the upload has settled. Both repeated deletes
   must succeed and leave `present:false`; no stale playback or UI error may
   remain.
5. For a direct overlap test, send DELETE while POST is still active. The
   server must reject the overlap with **409** (`media_upload_in_progress`);
   the browser's ordered cancel-and-delete path should avoid that race.

The raw-media migration guard is architectural, not just a speed check:

- Partition label is `media`, offset `0x820000`, size `0x7E0000`, split into
  two slots. Confirm the table before flashing:

  ```sh
  grep -E '^media[[:space:]]' partitions.csv
  ```

- The inactive slot erases only the required aligned range, streams payload and
  CRC, and writes the committed header last. On boot, the store scans
  CRC-valid headers and selects the newest generation. Reboot after a commit
  and confirm `GET /api/v1/media/status` still reports the same GIF.
- Playback maps the committed payload through the LVGL variable descriptor;
  it is not a `/spiffs/active.gif` staging-file check. An aborted or invalid
  upload must leave the prior valid slot selected.

## Live telemetry and browser responsiveness

The live path is WebSocket, not SSE: connect to `/api/v1/state/ws`. The server
uses a fixed pool of **3 clients**, each with at most one in-flight heap-owned
frame, and broadcasts at a bounded **20 Hz**. A new dashboard must not evict an
existing socket. The browser sends an application heartbeat every **750 ms**;
the server consumes it. Verify two concurrent dashboards remain WebSocket
`OPEN` for 30 seconds with heartbeat active and no HTTP polling. The verified
histories were client A uptime `144367→174367` and client B remaining `OPEN` at
uptime `182063`.

The connection badge reports **Live · WebSocket 20 Hz**, **Live · HTTP 4 Hz**,
or **Disconnected**. The browser applies only strictly newer `uptimeMs` targets,
renders a 35 ms EMA on every `requestAnimationFrame`, and resets timing after a
gap greater than **1 s**. The sparkline remains **4 Hz** and canvas DPR is capped
at **2**. On the demo waveform, collect ten seconds of browser samples and require
approximately 50 ms WebSocket target spacing; compare target-to-render error with
the prior 10 Hz / 90 ms EMA baseline rather than using physical-display FPS.

Close or block the WebSocket and verify the browser starts HTTP
`GET /api/v1/state` fallback at **4 Hz** (every 250 ms). A successful fallback
sample must change the badge to **Live · HTTP 4 Hz**; it becomes **Disconnected**
only when state polling also fails. Restore the socket and verify the badge
returns to **Live · WebSocket 20 Hz** without duplicate polling timers.

### Fourth-client capacity check

Use four separate browser tabs or four WebSocket clients against
`ws://<board>/api/v1/state/ws` while the board is publishing telemetry:

1. Open clients 1–3 and confirm all three remain `OPEN` for at least 30 s with
   the 750 ms application heartbeat and no HTTP polling.
2. Open client 4. Its handshake is the only one that may be closed/rejected;
   clients 1–3 MUST remain open and continue receiving newer `uptimeMs` values.
   A fourth tab must not evict an existing dashboard or cause its badge to
   churn.
3. In the fourth tab, block/close WebSocket traffic. Confirm
   `GET /api/v1/state` runs every 250 ms (4 Hz), and that a successful sample
   keeps the badge **Live** while WebSocket retries occur every 1 s. The badge
   becomes **Disconnected** only after both the socket and state poll fail.
4. Restore the socket and confirm polling stops with one timer, the badge stays
   **Live**, and clients 1–3 were never disconnected.

This check distinguishes capacity rejection of the newcomer from the forbidden
behavior of failing the whole fourth dashboard or evicting an existing client.

### GIF ownership and lifecycle check

Trace one upload through the board logs/source before changing playback. Verify
the custom layers are web upload, raw dual-slot CRC/committed-header publication,
`esp_partition_mmap`, and the locked `lv_image_dsc_t`/`lv_gif` gauge integration.
Verify `lv_gif.c` delegates `GIF_openRAM`/`GIF_playFrame` to bundled
AnimatedGIF for parsing, LZW, frame timing, and composition; this is not a
project-written decoder. Confirm the intentional standard-decoder choice: the
full framebuffer is zeroed and `pTurboBuffer` is null because turbo did not
compose delta/disposal frames correctly. During delete or replacement, verify
the order display lock → destroy widget → unmap; mapped bytes must not be freed
or unmapped while the widget exists. The custom display flush remains the
internal-DMA partial CO5300 path and is not GIF decompression.

During a 30-second dashboard soak, exercise a control (for example, theme or
brightness) while telemetry is active. Latest verified result: maximum
main-thread probe was **9 ms**, with **no freezes longer than 500 ms**.
Telemetry publication is outside the LVGL worker, so GIF playback must not
stall the WebSocket stream.
## Clock and CSV verification

The **Sync Time** control is the only synchronization action. Its `POST
/api/v1/time` supplies browser `Date.now()` and the configured offset; firmware
calls `settimeofday` and saves epoch plus monotonic checkpoint/config to NVS.
On reboot it restores that checkpoint and advances only by monotonic delta when
applicable. There is no RTC/NTP resynchronization in this path, so opening the
dashboard alone does not sync the device.

CSV `timestamp_local` must be `%Y-%m-%dT%H:%M:%S` with no suffix;
`utc_offset_minutes` remains separate. Hardware verification produced **1,800
rows**, `badTimestampCount 0`, and offset `-300` minutes.
***
***

## Physical cadence and crash-log guards

With GIF media deleted and the live gauge animating, run the cadence guard:

```sh
python3 tools/check_display_cadence.py \
  --url "${BOARD_URL}" --seconds 30
```

Latest hardware result was **min 61, median 63 over 112 samples**. The gauge
and sampling contract remains **16 ms / approximately 60 Hz**. Do not run
this live-gauge threshold while GIF playback is active; GIF decoder rendering
is a different workload.

Capture the serial output during the live-gauge and media/reboot checks, then
fail the review if any display-memory, panic, reset, or crash marker appears:

```sh
! grep -E \
  'ESP_ERR_NO_MEM|send color data failed|Guru Meditation|panic|abort|rst:0x|Brownout' \
  serial.log
```

The expected result is no matching line. In particular, preserve the DMA
display path's no-`ESP_ERR_NO_MEM` behavior while Wi-Fi is active.

## Mock and SoftAP smoke

Mock-server wiring (no WebSocket or hardware claims):

```sh
python3 -m py_compile tools/mock_server.py
node --check web/app.js
python3 tools/mock_server.py --host 127.0.0.1 --port 18080
curl -I -s http://127.0.0.1:18080/
curl -fsS http://127.0.0.1:18080/api/v1/state
curl -fsS http://127.0.0.1:18080/api/v1/config
curl -fsS http://127.0.0.1:18080/api/v1/themes
curl -fsS http://127.0.0.1:18080/api/v1/logs.csv
```

On hardware, join `BoostGauge-XXXX` with `boost1234`, open
`http://192.168.4.1/`, and repeat the live WebSocket, media, cadence, and
serial crash-log checks above. The physical gauge must resume after media is
deleted and its PSI must change during the live sweep.
