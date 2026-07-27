# What does `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` actually buy? — hardware A/B

Date: 2026-07-26
Branch: `spike/lvgl-iram-benefit` (off `main` at `c18339c`). **Not merged to main.**
Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75, COM3, LAN `192.168.50.102`.
Firmware baseline: `v0.5.0-2-gc18339c`, ESP-IDF v5.5.1.

## Verdict

**No. It buys nothing measurable. 82,812 bytes of internal DRAM are being spent
for no benefit this A/B can detect on any of the four faces.**

Across 30 measured 30-second soaks — four faces, two builds, three or six runs
each, every run from a fresh boot with the full 3-client WebSocket pool — not
one metric moved outside its own run-to-run spread in favour of the option. The
guard metric is unchanged (`dyno-cell` median 60 FPS both ways), `worstRenderUs`
is unchanged, `pixelsPerSecond` is unchanged, and WebSocket throughput is
unchanged at the 187.5 frames/s ceiling on both builds.

The single arguable exception is `night-city`, where `framesOverBudget` moves
from a median of 15 to 16 per second and `renderGapP50Us` from 17.1 ms to
17.8 ms. That is **one extra over-budget frame per second out of ~37 renders**,
on the face that already drops 15, with `renderFps`, `worstRenderUs` and
`pixelsPerSecond` all flat. It is reported below rather than buried, but it is
not worth 82 kB — it is roughly 1/80th of the internal DRAM budget per dropped
frame per second, on one face.

The cost is confirmed independently of the prior spike: `idf.py size` DIRAM
remaining goes **67,869 → 150,681 bytes**, a delta of **exactly 82,812 bytes**,
matching the BLE spike's figure derived from a completely different pair of
builds.

## Correction to a previously recorded number

`docs/research/2026-07-26-ble-wifi-coexistence.md` records internal free at peak
going **13,611 → 114,023** with the option off. **The 114,023 figure is not
attributable to this option alone.** That build was the BLE-off control, which
also had `CONFIG_ESP_WIFI_IRAM_OPT=n` and `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` — see
that document's own footnote 1.

Changing **only** `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`, measured here:

| Internal DRAM at genuine peak (bytes) | IRAM=y (`main`) | IRAM=n | Delta |
|---|---:|---:|---:|
| free (median of per-run medians) | **13,723** | **96,611** | +82,888 |
| largest free block | 7,680 | 63,488 | +55,808 |
| minimum-ever free | 9,527 | 92,179 | +82,652 |
| DMA-capable free | 5,935 | 88,823 | +82,888 |

The runtime delta (82,888 B) agrees with the static `idf.py size` delta
(82,812 B) to within 76 bytes. **Use 96,611, not 114,023,** when budgeting what
this change alone releases. The extra ~17 kB in the spike's number came from the
Wi-Fi IRAM options, which are a separate decision with a separate cost.

`docs/research/2026-07-26-ram-constrained-inventory.md` (written concurrently
and independently) reached the same conclusion from the arithmetic alone and
predicted **~96,423 B**, explicitly labelling it as not measured. The measured
value is **96,611 B** — 188 bytes higher. That prediction is now confirmed on
hardware and can be relabelled as measured.

## Methodology

Stated explicitly, because several of this project's recorded "facts" turned out
to omit the conditions they were measured under.

**Conditions held identical for every single sample:**

- **Demo mode ON.** Per AGENTS.md, a real MAP sensor at constant atmosphere
  invalidates nothing and legitimately renders in the single digits. (The
  board's sensors are in fact absent — `adsPresent:false`, `bmpPresent:false` —
  so real mode was not an option here either.) Asserted programmatically before
  every run; a run whose `/state` did not report `demo:true` aborted.
- **Face asserted, not assumed.** `PUT /themes/active` followed by a read-back
  assertion that `activeThemeId` matched, before any sample was taken.
- **6-second settle after the theme PUT**, discarding the scene *build*. The
  ledger notes `worstRenderUs` right after a PUT reports the ~50 ms build, not
  steady state.
- **Fresh reboot before every run** (`POST /api/v1/restart`, wait for the API,
  then 5 s). See the slot leak below for why this is mandatory.
- **Exactly 3 WebSocket clients**, verified live for the whole window. Any run
  in which a client was refused or dropped was **discarded and re-run**, because
  a 2-client run has a materially different telemetry load. 11 partial runs were
  discarded this way and are excluded from every figure here.
- **30 s sample window at 4 Hz** (~120 `/state` polls), first 4 samples dropped
  as warmup — the same warmup rule `tools/check_display_cadence.py` uses, since
  the metrics are one-second buckets and the first can straddle a reset.
- Sampling began **~25-30 s after boot** in every run (recorded per run as
  `bootAgeSecAtSampleStart`).
- Firmware version recorded per run; both builds report
  `v0.5.0-2-gc18339c-dirty`. The `-dirty` is an unrelated concurrent edit to
  `README.md` in the shared worktree, not a source difference between the two
  builds. The only source difference between them is one line of
  `sdkconfig.defaults`.

**Run counts:** 3 valid runs per face per build, except `night-city` which got
**6 per build** because it was the only face showing a signal near the spread
boundary and n=3 could not settle it.

**Reading the tables:** each cell is `median [min-max]` across runs. A
difference is treated as real only if it exceeds the larger of the two builds'
own min-max spreads. Everything reported as "no change" failed that test.

**Both builds carried a temporary `GET /api/v1/debug/heap` endpoint** for the
internal-RAM figures (same approach as the prior spike). The three extra
`night-city` IRAM=y runs were taken on stock `main` firmware, which has no such
endpoint, so those three contribute cadence data only — they are excluded from
the RAM table (12 of 15 IRAM=y runs carry heap data; 15 of 15 for IRAM=n).

### The shipping binary was re-verified separately

The A/B above compares two *instrumented* builds. The binary this branch
actually ships has the heap probe reverted, so it is not the binary that was
measured. It was therefore rebuilt, reflashed and re-run on all four faces —
see "Verification of the shipping binary" below. Measuring one binary and
shipping another is the failure mode this whole document exists to unpick.

### The WebSocket slot leak, and why back-to-back runs are not comparable

**Found while building the harness; it is a genuine bug on `main` and it
invalidates the prior spike's WebSocket throughput comparison.**

Symptom: the first 3-client connection after a boot works and all three clients
receive ~62.5 frames/s. On later attempts within the same boot, the third client
completes the HTTP 101 handshake and is then immediately closed. Serial shows
`W httpd_uri: httpd_uri: uri handler execution failed`. Aggregate throughput
sits at ~125 frames/s instead of ~187.5 and stays there.

Mechanism, in `main/boost_web.c`:

- On a CLOSE frame or recv error, `state_ws_get()` sets
  `s_state_ws_clients[slot].fd = -1` but does **not** clear `inflight` or
  `payload`.
- `state_ws_send_done()` only clears `inflight` when `client->fd == ctx->fd`.
  Once `fd` is `-1` that can never match, so `inflight` stays `true` forever.
- Slot allocation requires `fd < 0 && !inflight`, so the slot is permanently
  unallocatable until reboot.

Detection: connect 3 clients, close them, reconnect, and count clients actually
receiving frames; combined with a passive COM3 serial capture for the
`httpd_uri` warning. (Reading the port resets the board unless DTR/RTS are
driven low *before* opening — worth knowing, it silently reboots the device
under measurement otherwise.)

**Consequence for measurement:** any two WebSocket measurements taken without a
reboot between them are measuring different numbers of live clients. This is
almost certainly what produced the prior spike's headline
**133.1 vs 200.1 frames/s** — 133 is close to 2 clients (125) and 200 to 3
(187.5). Every run here therefore reboots first and verifies 3 live clients.

A second, independent confound was found in the same place: **the pool is shared
with any dashboard open on the LAN.** A browser somewhere on the network
reconnects within ~1 s of a reboot and races our third client for the last slot,
so even a first-attempt-after-boot run sometimes only gets 2 slots. This is why
runs are verified and retried rather than trusted.

This bug has been filed as separate work; it is **not** fixed on this branch,
because fixing it mid-A/B would have changed the firmware under measurement.

**The general lesson is the reusable part.** A measurement harness that does not
verify its own preconditions will silently report a different experiment than
the one intended — here, "3 clients" that was often 2. The same failure class
was found independently the same day in `sim/`: its screenshot path used
`lv_snapshot_take()`, which re-renders the whole widget tree, so a screenshot
could never show a partial-refresh trail and every past "no trails" claim from
one was unfalsifiable. Assert the precondition in the harness, and record what
was asserted next to the number.

## Per-face results

Demo mode, 3 WebSocket clients, 30 s per run, fresh boot per run.
Cells are `median [min-max]` across runs.

**`dyno-cell`** (arc — the face the 60 FPS guard is defined against) — n = 3 / 3

| Metric | IRAM=y (current) | IRAM=n (reclaimed) |
|---|---:|---:|
| renderFps median | 60 [60-60] | 60 [60-60] |
| renderFps min | 57 [56-57] | 56 [55-57] |
| worstRenderUs median (ms) | 21.3 [20.6-22.2] | 21.1 [20.1-21.5] |
| worstRenderUs max (ms) | 62.0 [59.6-64.6] | 62.0 [61.3-62.5] |
| pixelsPerSecond median (k) | 502 [502-505] | 501 [501-502] |
| renderGapP50Us median (ms) | 16.1 [16.1-16.1] | 16.1 [16.1-16.1] |
| renderGapMaxUs median (ms) | 23.3 [22.0-23.6] | 22.3 [21.3-22.8] |
| framesOverBudget median | 2.0 [2.0-3.0] | 3.0 [2.0-3.0] |
| WS frames/s (3 clients) | 187.3 [187.2-187.5] | 187.4 [186.8-187.5] |

**`vault-tec`** — n = 3 / 3

| Metric | IRAM=y (current) | IRAM=n (reclaimed) |
|---|---:|---:|
| renderFps median | 56 [56-57] | 56 [56-56] |
| renderFps min | 50 [49-51] | 50 [49-51] |
| worstRenderUs median (ms) | 23.4 [23.2-26.0] | 24.7 [23.5-25.3] |
| worstRenderUs max (ms) | 34.7 [34.1-36.4] | 37.1 [34.0-38.0] |
| pixelsPerSecond median (k) | 414 [413-446] | 401 [397-406] |
| renderGapP50Us median (ms) | 16.1 [16.1-16.1] | 16.2 [16.2-16.2] |
| renderGapMaxUs median (ms) | 35.6 [34.9-36.0] | 34.1 [34.0-34.9] |
| framesOverBudget median | 6.0 [6.0-7.0] | 6.5 [6.0-7.0] |
| WS frames/s (3 clients) | 186.7 [181.0-187.7] | 187.5 [187.5-187.6] |

**`night-city`** — n = 6 / 6

| Metric | IRAM=y (current) | IRAM=n (reclaimed) |
|---|---:|---:|
| renderFps median | 37 [36-38] | 37 [36-38] |
| renderFps min | 28 [27-29] | 28 [26-29] |
| worstRenderUs median (ms) | 31.9 [31.5-32.7] | 32.3 [29.4-34.8] |
| worstRenderUs max (ms) | 56.9 [51.6-74.2] | 59.0 [56.1-64.9] |
| pixelsPerSecond median (k) | 304 [292-307] | 303 [297-309] |
| renderGapP50Us median (ms) | 17.1 [16.9-17.6] | 17.8 [17.5-18.8] |
| renderGapMaxUs median (ms) | 85.3 [81.7-96.0] | 89.5 [84.0-98.9] |
| framesOverBudget median | **15.0 [14.0-15.0]** | **16.0 [16.0-17.0]** |
| WS frames/s (3 clients) | 187.4 [187.1-187.5] | 187.4 [187.2-187.5] |

**`big-digit`** (the full-screen recolour — the most likely place for IRAM to
matter) — n = 3 / 3

| Metric | IRAM=y (current) | IRAM=n (reclaimed) |
|---|---:|---:|
| renderFps median | 27 [27-28] | 27 [27-28] |
| renderFps min | 20 [19-20] | 19 [18-20] |
| worstRenderUs median (ms) | 56.0 [55.1-56.7] | 56.9 [56.6-58.7] |
| worstRenderUs max (ms) | 64.3 [63.5-70.4] | 68.6 [65.4-69.9] |
| pixelsPerSecond median (k) | 1312 [1305-1313] | 1295 [1295-1313] |
| renderGapP50Us median (ms) | 31.9 [26.6-32.0] | 31.9 [31.8-32.0] |
| renderGapMaxUs median (ms) | 96.2 [96.0-100.0] | 96.6 [96.0-97.0] |
| framesOverBudget median | 16.0 [16.0-16.0] | 16.0 [15.0-16.0] |
| WS frames/s (3 clients) | 187.3 [186.6-187.5] | 187.4 [187.2-187.4] |

### The `big-digit` full-screen repaint specifically

This was the hypothesis with the best prior: it is the one face doing a genuine
full-screen recolour, priced in the ledger at ~45 ms unbanded and ~33 ms at the
selected 2 bands, so IRAM placement of the rasteriser should help most here.

It does not. `worstRenderUs` median is **56.0 ms [55.1-56.7]** with IRAM versus
**56.9 ms [56.6-58.7]** without — a 0.85 ms difference against a 2.06 ms spread.
`renderGapP50Us` is 31.9 ms both ways, `framesOverBudget` 16 both ways,
`renderFps` 27 both ways.

(Note these `worstRenderUs` figures are higher than the ledger's 25.8-45.1 ms
band because these runs carry the full 3-client WebSocket load, which the
banding measurements did not. That does not affect the A/B, since both builds
carry it identically.)

The likely reason there is no effect: per the ledger, this operation is
substantially link-bound — 14.5 ms of the 45 ms was the QSPI transfer at 60 MHz,
and raising the clock 40 → 80 MHz bought 17% here while buying nothing on the
partial-update faces. Instruction fetch is not the bottleneck for a flat fill,
and the ESP32-S3 caches the flash-resident rasteriser well enough that moving it
to IRAM does not show up.

**No SIMD alternative exists here, and none is implied.** The bundled LVGL tree
offers only `NONE`, `NEON` and `HELIUM` blend backends — both accelerated ones
are ARM — plus an empty `ASM_CUSTOM` hook. There is no Xtensa/ESP32-S3 assembly
backend to switch on, so "turn the IRAM option off and enable SIMD instead" is
not an available trade. The verdict rests on the measurements above, not on a
substitute being available.

## WebSocket throughput hypothesis — refuted

The spike's 133.1 (main) vs 200.1 (reclaimed) frames/s suggested memory pressure
was throttling telemetry by ~50%. Measured directly here, with 3 verified live
clients and a fresh boot on both builds:

| | IRAM=y | IRAM=n |
|---|---:|---:|
| WS frames/s, 3 clients, median of 15 runs | **187.4** | **187.4** |
| range across all runs | 181.0 - 187.7 | 186.8 - 187.6 |

**Identical, and both are at the architectural ceiling.** The cadence contract
specifies one frame per 16 ms sample per client — 62.5 Hz × 3 = 187.5 frames/s.
Both builds deliver 62.4-62.5 Hz per client. There is no headroom for the
reclaimed memory to buy, because `main` was already saturating the contract.

The prior 133.1 figure is consistent with two live clients (125) rather than
three, i.e. the slot leak described above, not memory pressure. **The
telemetry-throughput argument for reclaiming the 82 kB should be withdrawn.**

## Verification of the shipping binary

Rebuilt from the branch tip with the probe reverted (`a58cb48`, reported as
`v0.5.0-6-ga58cb48`), reflashed over COM3, `idf.py size` DIRAM remaining
150,681 — identical to the measured IRAM=n build. Generated `sdkconfig` confirmed
to contain `# CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM is not set`, per the ledger's
rule that a line in `sdkconfig.defaults` is not evidence the symbol took effect.

Official guard, demo mode on `dyno-cell`:

```
python tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30
physical render FPS: min=56 median=60 samples=104
```

**Median 60 — passes.** (`min` 56 sits inside the band observed on both
instrumented builds, 55-57.)

Per-face, one verified 3-client run each unless noted:

| Face | renderFps median | worstRenderUs median | pixelsPerSecond median | framesOverBudget |
|---|---:|---:|---:|---:|
| `dyno-cell` | 60 | 21.9 ms | 501 k | 3 |
| `vault-tec` | 56 | 25.5 ms | 394 k | 7 |
| `night-city` | 38 | 32.4 ms | 293 k | 16 |
| `big-digit` (n=2) | 27 | 59.2 ms [58.0-60.4] | 1295-1322 k | 16 |

Three of the four faces land inside the instrumented IRAM=n bands. **`big-digit`
`worstRenderUs` reads 2-3 ms higher** (59.2 ms vs 56.9 ms instrumented, vs
56.0 ms on IRAM=y). This is reported rather than smoothed over, but it is **not
isolated**, and it is unlikely to be caused by the probe revert, which only
removes code. During these runs the external dashboard on the LAN escalated from
holding one pool slot to holding two, and one attempt returned degraded
per-client rates (32.9/51.5/51.5 f/s) plus a WebSocket protocol error — i.e.
measurable extra load that was absent during the A/B. Only two clean 3/3 runs
were obtainable before that contamination, so n=2. **Treat the A/B pair as the
controlled comparison and this table as a smoke test of the shipping binary.**

## What I could NOT test

Stated plainly rather than estimated.

- **Anything outside demo mode.** The board's ADS1115 and BMP280 are both
  absent (`adsPresent:false`, `bmpPresent:false`, `fault:true`), so the real
  sensor path could not be exercised at all, on either build. Every number here
  is demo mode.
- **GIF playback**, media upload, OTA, pixel shift, brightness/dim scheduling
  and the touch path were not exercised on the IRAM=n build. GIF playback is the
  most notable gap: it is the one path known to be sensitive to internal-RAM
  pressure (the 24.5 kB decoder boot loop in the ledger) and it is plausibly the
  path that would *benefit* most from the reclaim, but it was not measured
  either way.
- **Long-run stability.** Longest continuous observation of the IRAM=n build was
  a single 30 s soak plus reboot overhead; total time on that firmware was
  roughly 35 minutes across ~25 boots. No thermal soak, no multi-hour run.
- **Whether the `night-city` +1 `framesOverBudget` is causal.** n=6 per build
  with barely non-overlapping ranges (14-15 vs 16-17) is suggestive, not
  conclusive. It could be a real effect of moving the rasteriser out of IRAM on
  the most rasterisation-bound face; it could also be a second-order effect of
  the very different heap layout. Separating those would need instruction-level
  profiling, which this A/B did not do.
- **Perceived smoothness.** No visual assessment was made of either build; all
  conclusions are from the exposed metrics. The ledger is explicit that metrics
  and perceived judder have diverged before.
- **The `-dirty` firmware suffix** could not be cleared, because another agent
  holds uncommitted `README.md` changes in the shared worktree. Both builds
  carry it identically, so it does not affect the comparison, but neither build
  is a clean-tree build.
- **`big-digit` `worstRenderUs` on the shipping binary versus the instrumented
  one** — a 2-3 ms gap, n=2, confounded by external LAN load that appeared
  partway through. Not isolated. Re-running it on a quiet network would settle
  it, and is the one loose end worth closing before merge.
- **Whether reclaiming this memory helps anything.** This report answers only
  "does the option buy performance" (no). It does not establish that the 82 kB
  is *useful* elsewhere — the BLE spike is the argument for that, and it remains
  a separate decision.

## Reproducing

```powershell
$env:IDF_TOOLS_PATH = 'C:\Espressif'
& 'C:\esp\v5.5.1\esp-idf\export.ps1'
Set-Location 'C:\Users\aliab\boost-gauge'
git checkout spike/lvgl-iram-benefit
Remove-Item sdkconfig -Force        # sdkconfig.defaults changed
idf.py build
idf.py -p COM3 flash                # COM3 only - COM6 is a different device
```

Per run: `POST /api/v1/restart`, wait for `/api/v1/state`, `PUT /themes/config
{"demoMode":true}`, `PUT /themes/active {"id":<face>}`, settle 6 s, open 3
WebSocket clients to `/api/v1/state/ws` and verify all three receive frames,
then poll `/api/v1/state` at 4 Hz for 30 s. Discard and re-run if fewer than 3
clients stay live.

Guard, demo mode on `dyno-cell` per AGENTS.md. Run on both IRAM=n binaries:

```
python tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30
instrumented IRAM=n build : physical render FPS: min=57 median=60 samples=104
shipping binary (a58cb48) : physical render FPS: min=56 median=60 samples=104
```

Reference is min 57 / median 60. **Both pass** on the median, which is the gate.

## Board state at end of this work

Reflashed to `main` (`c18339c`) over COM3 and verified: `/api/v1/state`
responds, theme `night-city`, demo mode `false`.
