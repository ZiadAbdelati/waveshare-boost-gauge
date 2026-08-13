# BLE (NimBLE) + Wi-Fi coexistence on the boost gauge — hardware spike

Date: 2026-07-26
Branch: `spike/ble-wifi-coexistence` (commit `126ae46`). **Not merged to main.**
Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75, COM3, LAN `192.168.50.102`.
Firmware baseline: `v0.5.0-1-g37339b4` (repo `main` at HEAD), ESP-IDF v5.5.1.

## Verdict

**Yes — BLE and Wi-Fi coexist on this hardware, and they were observed running
together. No, they do not fit in `main` as it is configured today.**

The radios were never the problem. The ESP32-S3 coexistence arbiter came up
without complaint in every build (`coexist: coex firmware version: b0bcc39`),
Wi-Fi STA stayed associated at `-49 dBm`, SoftAP stayed up, the HTTP API stayed
reachable, and the physical gauge held its cadence. The constraint is **internal
DRAM**, and the thing consuming it is not Wi-Fi — it is
`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`, which parks LVGL's rasteriser in IRAM
at a cost of **82.8 kB** of DIRAM.

On stock `main`, free internal RAM at peak is **13.6 kB** with a **7.7 kB**
largest free block. The NimBLE controller needs a ~22.5 kB contiguous block and
**29.7 kB** total. It cannot fit, and every intermediate configuration failed in
a way traceable to that single number.

Once LVGL's IRAM copy was released, everything came up on the first boot with
**77.7 kB** free internal and a **63.5 kB** largest block — comfortably more
headroom than `main` has today, with BLE running.

## Measured numbers

### Internal RAM at genuine peak load

Peak means: `dyno-cell` (arc) face, demo mode, display rendering, Wi-Fi STA
associated, SoftAP up, HTTP server live, and the full pool of **3 WebSocket
clients** connected. Sampled over 30 s via a temporary `GET /api/v1/debug/heap`.
`heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` and friends.

| Metric (bytes) | Baseline (`main` + probe) | BLE-off control¹ | **BLE enabled + advertising** |
|---|---:|---:|---:|
| internal free (median) | **13,611** | 114,023 | **77,703** |
| internal largest free block | **7,680** | 63,488 | **63,488** |
| internal minimum-ever free | **9,535** | 109,567 | **73,199** |
| DMA-capable free | 5,823 | 106,235 | 69,915 |
| PSRAM free | 6,390,652 | — | 6,537,876 |

¹ Control = the identical memory configuration (Wi-Fi IRAM opt off, LVGL IRAM
opt off) with `CONFIG_BT_ENABLED=n`. It exists so the BLE delta is not confused
with the config changes that made room for BLE.

**NimBLE's actual cost:**

| Measurement | Bytes |
|---|---:|
| Runtime internal RAM, control vs BLE-enabled, both at peak | **36,320** |
| `nimble_port_init()` + advertising start, measured alone against an unfragmented heap (78,991 → 49,323) | **29,668** |
| Static `.bss`/`.data`/IRAM footprint (`idf.py size` DIRAM remaining, 168,277 → 161,765) | **6,512** |

The 36,320 − 29,668 ≈ 6.7 kB difference is the NimBLE host task stack (3,072),
the spike's notify producer task stack (3,072) and change.

### Boot-time internal DRAM staging

From `DRAM[stage]` log lines added to `app_main` (tuned NimBLE, LVGL IRAM still
on — the build that fails):

| Stage | free | largest block |
|---|---:|---:|
| app_main entry | 138,975 | 63,488 |
| after `boost_display_start()` | 80,919 | 63,488 |
| after gauge + sample task | 71,699 | 59,392 |
| after Wi-Fi + httpd | **13,155** | **7,680** |
| BLE controller init | `ESP_ERR_NO_MEM` | — |

Wi-Fi (STA + SoftAP + LWIP) plus the HTTP server consume **58,544 bytes** of
internal DRAM. That, plus NimBLE's 29,668, is 88,212 against the 71,699
available — a **shortfall of ~16.5 kB before any safety margin**.

Note the largest free block collapsing from 59,392 to 7,680: Wi-Fi does not just
consume internal DRAM, it fragments it. **Bring-up order is decisive.** With
NimBLE started after Wi-Fi the controller cannot get a contiguous block at any
total-free figure; with NimBLE started first it takes 22.5 kB contiguous without
difficulty.

### Display cadence — the gate that matters

`python tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30`,
run in **demo mode on the `dyno-cell` (arc) face**, per AGENTS.md. Reference:
min 57 / median 60.

| Build | min FPS | median FPS | samples |
|---|---:|---:|---:|
| Reference (AGENTS.md, 2026-07-25) | 57 | 60 | — |
| Baseline (`main` + heap probe) | 58 | **60** | 103 |
| BLE-off control (same memory config) | 57 | **60** | 101 |
| **BLE advertising, 30–60 ms interval, run 1** | 57 | **60** | 79 |
| **BLE advertising, 30–60 ms interval, run 2** | 58 | **60** | 73 |
| **BLE advertising, 1000 ms interval** | 56 | **60** | 75 |
| `main` firmware after restore | 58 | **60** | 102 |

**BLE does not cost display cadence.** Every configuration passes the 60 FPS
median guard, and the min stays inside the reference band.

Because FPS alone hides pacing, the deeper metrics over the same 30 s soak with
3 WebSocket clients attached:

| Metric | Baseline | BLE-off control | BLE advertising (run 1 / run 2) |
|---|---:|---:|---:|
| `renderFps` min / median | 55 / 60 | 53 / 60 | 55 / 60  ·  56 / 60 |
| `worstRenderUs` median | 21,865 | 23,980 | 22,762  ·  22,070 |
| `worstRenderUs` max | 93,612 | 80,141 | 102,980  ·  71,983 |
| `framesOverBudget` median | 2 | 3 | 3  ·  3 |
| `pixelsPerSecond` median | 514,792 | 508,136 | 499,392  ·  502,372 |
| `renderGapP50Us` median | 16,100 | 16,100 | 16,000  ·  16,100 |
| `renderGapMaxUs` median | 22,777 | 25,008 | 23,738  ·  23,185 |
| WebSocket frames/s, 3 clients | 133.1 | 200.1 | 185.0  ·  174.2 |

`worstRenderUs` and `framesOverBudget` sit inside their run-to-run spread; the
103 ms `worstRenderUs` outlier in BLE run 1 is not reproduced in run 2 (72 ms,
*better* than baseline's 94 ms). `renderGapP50Us` is a clean 16.0–16.1 ms
throughout. Nothing here indicates BLE perturbs the display pipeline.

### What BLE *does* cost: HTTP responsiveness

Sequential `GET /api/v1/state` round-trip, n = 60–80.

| Build | min | **p50** | p90 | max |
|---|---:|---:|---:|---:|
| `main` firmware | 7.8 ms | **25.3 ms** | 38.8 ms | 58.9 ms |
| BLE-off control (same memory config) | 8.1 ms | **27.8 ms** | 43.0 ms | 70.1 ms |
| BLE advertising @ 30–60 ms, run 1 | 75.6 ms | **181.2 ms** | 283.7 ms | 1207.8 ms |
| BLE advertising @ 30–60 ms, run 2 | 9.0 ms | **88.1 ms** | 188.1 ms | 483.4 ms |
| BLE advertising @ 30–60 ms, run 3 | 10.8 ms | **166.0 ms** | 284.8 ms | 346.5 ms |
| BLE advertising @ 100 ms | 22.2 ms | **156.3 ms** | 243.6 ms | 280.8 ms |
| BLE advertising @ 1000 ms | 14.9 ms | **102.9 ms** | 194.5 ms | 358.5 ms |

The control lands within 2.5 ms of `main`, so the memory-config changes are not
responsible. HTTP p50 degrades **3.5–7x** when BLE is enabled, and slowing
advertising to 1 Hz recovers only part of it. Two plausible causes, not
separated by this spike:

1. Wi-Fi/BLE coexistence arbitration genuinely time-slicing the radio.
2. `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y`, which this build needs in order to fit.
   Its own Kconfig help warns performance will decrease; every BLE interrupt now
   takes a flash-cache miss, stalling both cores. A build with the controller in
   IRAM would not pay this — but that build does not fit.

The 62.5 Hz WebSocket telemetry path is **not** affected (185 and 174 frames/s
with BLE, vs 133 baseline), so this is specifically request/response latency on
the httpd task, not telemetry throughput.

## What it took to make it fit

Applied cumulatively; DIRAM remaining is the `idf.py size` figure out of 341,760.

| Configuration | DIRAM remaining | Result on hardware |
|---|---:|---|
| `main` + heap probe (no BLE) | 67,869 | Works. 13.6 kB free at peak. |
| \+ NimBLE, near-stock | 42,369 | **Boot loop**, 10 boots in 30 s. |
| \+ NimBLE sized down, host mempools in PSRAM | 53,945 | Boots; **httpd and BLE both `ESP_ERR_NO_MEM`**. |
| \+ `ESP_WIFI_IRAM_OPT=n`, `ESP_WIFI_RX_IRAM_OPT=n` | 71,645 | HTTP recovers; **BLE still `ESP_ERR_NO_MEM`**. |
| \+ `BT_CTRL_RUN_IN_FLASH_ONLY=y`, BLE started before Wi-Fi | 78,953 | **BLE advertises**; Wi-Fi then panics in `wifi_softap_start`. |
| \+ `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` | 161,765 | **Both up, single clean boot.** |
| (control) previous row with `BT_ENABLED=n` | 168,277 | Both-off control for attribution. |

The decisive lever is the last one, and it is worth stating plainly:
`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` costs **82,812 bytes** of DIRAM
(161,765 − 78,953). That is six times NimBLE's static footprint and nearly three
times its total runtime cost. **The gauge's internal-RAM scarcity is a display
tuning decision, not a Wi-Fi one.** Turning it off cost nothing measurable in
this spike: the control build held min 57 / median 60 FPS and 27.8 ms HTTP p50.

Two configuration traps found along the way, both worth recording:

- **`CONFIG_BT_CTRL_BLE_MASTER` is not the central role.** Its help text reads
  "Enable BLE connection feature". Setting it to `n` to save memory produced a
  controller reporting `Feature Config, ... CONNECT:0` — connectable advertising
  unsupported, i.e. a GATT peripheral nothing can connect to. It must stay `y`.
- **`CONFIG_BT_LE_*` symbols do not exist on this target.** ESP32-S3 uses the
  `BT_CTRL_*` namespace; `BT_LE_MAX_CONNECTIONS` and `BT_LE_50_FEATURE_SUPPORT`
  were silently accepted into `sdkconfig.defaults` as *unknown symbols* and had
  no effect. Same failure class as the phantom 60 MHz QSPI trial: check the
  generated `sdkconfig`, not the defaults file.

## Failure signatures observed

**1. Near-stock NimBLE — boot loop (10 boots in 30 s):**

```
W (2610) wifi:malloc buffer fail
E (2621) wifi:Expected to init 6 rx buffer, actual is 2
E (2656) boost_net: boost_network_start(220): wifi init
E (2664) boost_main: web control plane failed: ESP_ERR_NO_MEM
E (2699) BLE_INIT: Malloc failed
BLE assert emi.c 164, param 00000000 00001000
Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).
```

**2. Tuned NimBLE, LVGL IRAM still on — device up, control plane and BLE both dead:**

```
E (6672) boost_web: boost_web_start(1608): httpd
W (6675) boost_main: DRAM[after-web-start] free=9611 largest=7680 minEver=9239
E (6683) boost_main: web control plane failed: ESP_ERR_HTTPD_TASK
E (6712) BLE_INIT: Malloc failed
E (6717) BLE_INIT: esp_bt_controller_init -2
E (6725) boost_ble: nimble_port_init: ESP_ERR_NO_MEM
```

**3. BLE first, LVGL IRAM still on — BLE wins, Wi-Fi dies (8 boots in 32 s):**

```
I (2699) boost_ble: advertising as BoostGauge
W (2703) boost_main: DRAM[after-ble] free=49323 largest=40960
...
W (4325) wifi:alloc eb len=752 type=4 fail
Guru Meditation Error: Core  0 panic'ed (LoadProhibited). Exception was unhandled.
Backtrace: ieee80211_hostap_attach <- wifi_softap_start <- _do_wifi_start
           <- wifi_start_process <- ieee80211_ioctl_process <- ppTask
```

**4. The configuration that works — single boot, both up:**

```
W (2561) boost_main: DRAM[before-ble] free=161679 largest=86016
I (2574) BLE_INIT: Put all controller code in flash
I (2583) BLE_INIT: Bluetooth MAC: 28:84:85:55:5e:d6
I (2676) boost_ble: host synced, addr 28:84:85:55:5e:d6
I (2699) boost_ble: advertising as BoostGauge
W (2703) boost_main: DRAM[after-ble] free=132023 largest=63488
I (4313) wifi:mode : sta (28:84:85:55:5e:d4) + softAP (28:84:85:55:5e:d5)
I (5423) boost_net: STA got IP 192.168.50.102
I (5437) boost_web: HTTP API ready
W (5437) boost_main: DRAM[after-web-start] free=73807 largest=63488
W (20446) boost_main: DRAM[t+15s] free=73695 largest=63488 minEver=73199
```

## On the prior ledger-based claim

The prior analysis cited the regression-ledger row *"Internal RAM taken for speed
starved the radio"* — a 24.5 kB GIF-decoder allocation boot-looping the device
with a backtrace through `wifi_softap_start` / `ieee80211_hostap_attach` — and
concluded BLE+Wi-Fi was a high-risk go/no-go.

**That row generalises, and the objection that it was "about a GIF decoder" does
not hold.** Failure signature 3 above reproduces it *exactly*: the same
`wifi_softap_start` → `ieee80211_hostap_attach` frames, the same class of
allocation failure (`wifi:alloc eb len=752 type=4 fail`), the same unrecoverable
boot loop that precedes the HTTP server. The row is not a fact about GIF
decoding; it is the generic signature of internal-DRAM exhaustion on this board,
and BLE triggers it on the nose. The ledger's stated lesson — *measure free
internal RAM at peak Wi-Fi usage, not at the moment of allocation, and keep a
hard reserve* — is exactly right, and following it is what produced this report.

**But the risk framing was wrong in two respects, and the correction matters
more than the confirmation:**

1. **"Can BLE and Wi-Fi coexist on ESP32-S3" was never the question.** They can,
   they did, and the coexistence arbiter is not the constraint. Framing it as a
   radio-level go/no-go pointed at the wrong subsystem entirely.
2. **The blocking resource is the display's IRAM tuning, not Wi-Fi's appetite.**
   `LV_ATTRIBUTE_FAST_MEM_USE_IRAM` holds 82.8 kB of DIRAM — more than the
   entire BLE stack costs. "There is no room for BLE" is really "there is no
   room for BLE *while LVGL keeps 82.8 kB of IRAM*", which is a tunable product
   trade-off, not a hardware verdict.

So: not a go/no-go, and not high risk in the sense claimed. It is a **budget
decision with a measured price tag**, and the price is HTTP request latency
(25 ms → 88–181 ms p50), not display cadence.

## Remaining margin

In the working configuration, with BLE advertising and the full 3-client
WebSocket pool attached:

- internal free **77,703 B**, minimum-ever **73,199 B**, largest block **63,488 B**
- DMA-capable free **69,915 B**

That is **5.7x** the headroom `main` has today (13,611 B) and **7.7x** its
minimum-ever (9,535 B). It is comfortable. The largest free block being 63.5 kB
rather than 7.7 kB also means the heap is no longer fragmented into scraps,
which is what killed every other ordering.

## Follow-up (2026-08-13): the active-connection case, tested

The OBD2 BLE central went live and the "active connection + notifications in
flight" gap above was finally exercised, with a vLinker FD+ as the peripheral.
Findings (full detail in the AGENTS.md ledger):

- The FD+ negotiates a **7.5–15 ms** connection interval via the **L2CAP**
  connection-parameter-update path (`BLE_GAP_EVENT_L2CAP_UPDATE_REQ`), which
  NimBLE auto-accepts when unhandled. This is the aggressive case this spike
  could not reach with advertising alone.
- The web-mirror "lag" is real WiFi/BLE coexistence, but the physical gauge is
  unaffected (renderFps 56–62, display path, not the radio).
- **Both candidate fixes failed and were reverted:** (1) forcing a slower
  30–50 ms interval made the FD+ bunch its ELM traffic into longer bursts that
  blocked the radio longer (WS frames 939→~700/15 s, HTTP p50 27→~95–115 ms);
  (2) `esp_coex_preference_set(ESP_COEX_PREFER_WIFI)` is deprecated and also
  degraded WiFi. The baseline (20–40 ms request, FD+ L2CAP-updates to 7.5–15 ms,
  BALANCE coexistence) is the best measured state.
- Net conclusion: the lag is the inherent cost of a chatty BLE central on the
  S3's single 2.4 GHz radio. It does not apply in the car (no LAN), and the
  bench `SEARCHING`/`NO DATA` spam is a worst case a real car does not produce.

## What I could NOT test

Stated plainly, because these are real gaps, not caveats:

- **No BLE central was available, so nothing off-device ever saw the
  advertisement.** This PC's Intel Wireless Bluetooth adapter is in a Code 10
  fault state ("This device cannot start"); `bleak` reports
  `BleakBluetoothNotAvailableError: No Bluetooth adapter found`. No phone was
  reachable and no `adb` is installed. Fixing the adapter would mean changing
  system device settings, which I did not do. Evidence of advertising is
  therefore ESP-side only: `ble_gap_adv_start()` returned 0, the host synced with
  address `28:84:85:55:5e:d6`, and `advertising: true` was reported over HTTP.
  **That the controller accepted the advertisement is not proof a scanner can
  see it.**
- **Section 4 (throughput) is entirely unmeasured.** No central connected, so no
  connection interval was negotiated, no MTU was exchanged, and not one
  notification was sent (`notifySent: 0`). The firmware *requests* 15–30 ms
  (`itvl_min=12`, `itvl_max=24`), latency 0, 4 s supervision timeout, which would
  support 33–66 Hz against the product's 20 Hz target — but that is a request
  the central is free to refuse, and it was never exercised. Treat the 20 Hz
  target as **unvalidated**.
- **Display cadence with an active BLE connection and notifications in flight.**
  All cadence figures above are with BLE *advertising only*. A live connection
  adds periodic connection events that advertising does not, and the HTTP
  latency result shows BLE radio activity is not free. This is the single most
  important untested case.
- **Whether the HTTP latency penalty is coexistence or `RUN_IN_FLASH_ONLY`.**
  Not separated. Separating it needs a build with the controller in IRAM, which
  needs ~20 kB that this configuration does not have spare — recoverable by
  trimming Wi-Fi buffers, untested.
- **Long-run stability.** Longest continuous observation of the working
  configuration was roughly 90 s. No soak, no thermal, no reconnect cycling, no
  power-cycle repetition.
- **Everything outside the measured path**: GIF playback, media upload, OTA,
  pixel shift, the other three faces, and the real sensor path were not
  exercised with BLE enabled. All cadence work was demo mode on `dyno-cell`, per
  the documented guard.
- **`LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` was only validated against the `dyno-cell`
  face.** It moves LVGL's rasteriser out of IRAM; the richer faces (`vault`,
  `night-city`, `bigdigit`) are more rasterisation-bound and were not measured.
  Per the ledger, any style change must be re-checked per face on
  `pixelsPerSecond` and `worstRenderUs`, not `renderFps`.

## Reproducing

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
& 'C:\esp\v5.5.1\esp-idf\export.ps1'
Set-Location 'C:\Users\aliab\boost-gauge'
git checkout spike/ble-wifi-coexistence
Remove-Item sdkconfig -Force        # sdkconfig.defaults changed
idf.py -DSPIKE_BLE_FIRST=1 build
idf.py -p COM3 flash                # COM3 only
```

Then `GET /api/v1/debug/heap` for RAM and BLE link state,
`?bleadvms=N` to retune the advertising interval, `?blehz=N` for the notify
producer, and `python tools/check_display_cadence.py --url http://192.168.50.102
--seconds 30` in demo mode on `dyno-cell`.

## Board state at end of spike

Reflashed to `main` (`v0.5.0-1-g37339b4`, no `-dirty`) over COM3 and verified:

- `GET /api/v1/state` responds; `mode: apsta`, `staConnected: true`,
  `staIp: 192.168.50.102`, `apSsid: BoostGauge-5ED5`
- cadence guard after restore: **min 58 / median 60** over 102 samples
- HTTP `/api/v1/state` p50 **25.3 ms**
- theme returned to `night-city`, demo mode returned to `false` (real sensor)
