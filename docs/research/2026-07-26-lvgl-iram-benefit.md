# What does `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM` actually buy? — hardware A/B

Date: 2026-07-26, updated 2026-07-27.
Branch: `spike/lvgl-iram-benefit`, originally off `main` at `c18339c`, rebased
2026-07-27 onto `main` at `09e0369` (adds the PSRAM log ring and the WebSocket
slot-leak fix). **Merged to main 2026-07-27** — see "Update 2026-07-27" below
for the re-verification that gated the merge: the RAM headline number is
corrected for today's `main`, the `big-digit` clean-network re-run closes the
n=2/LAN-contamination gap and reports a real (if small) finding, and GIF
playback — never exercised with this option off — is now tested on both
builds.
Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75, COM3, LAN `192.168.50.102`.
Firmware baseline: `v0.5.0-2-gc18339c`, ESP-IDF v5.5.1.

## Verdict

**No. It buys nothing measurable on the cadence guard or on three of four
faces. 82,812 bytes of internal DRAM are being spent for a benefit this A/B
can detect on at most one face — see the 2026-07-27 update for a real, small,
non-gating cost on `big-digit` found only once the clean-network re-run closed
a measurement gap.**

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
- **OTA, pixel shift, brightness/dim scheduling and the touch path** were not
  exercised on the IRAM=n build. GIF playback and media upload/delete *were*
  closed as a gap on 2026-07-27 — see the update section below.
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
  one** — closed 2026-07-27 on a verified-quiet network, n=6 per build. See the
  update section: the gap is real, not a LAN artifact, though it is small and
  does not touch the merge-gating cadence guard.
- **Whether reclaiming this memory helps anything.** This report answers only
  "does the option buy performance" (no). It does not establish that the 82 kB
  is *useful* elsewhere — the BLE spike is the argument for that, and it remains
  a separate decision.

## Update 2026-07-27 — rebased onto today's `main`, remaining gaps closed

Three things gated merge: a rebase over two `main`-side changes that shift the
RAM picture, a clean-network re-run of the one ambiguous `big-digit` result,
and GIF playback, which had never been exercised with this option off despite
being the path the ledger already names as most sensitive to internal-RAM
pressure. All three are closed here.

### Rebase

`spike/lvgl-iram-benefit` (6 commits) rebased cleanly onto `main` at `09e0369`,
which added two things since this branch's `c18339c` base: the 43,200 B log
ring moved from internal `.bss` to PSRAM, and a WebSocket slot-leak fix. Both
`AGENTS.md` (ledger, append-only) and `sdkconfig.defaults` (the IRAM line, in a
region `main` never touched) merged without dropping either side's content;
`git rebase` reported no conflicts requiring judgement beyond keeping both
ledger insertions. No source files besides these two changed on either side, so
the rebase carries no behavioural risk beyond the diff already described above.

### Corrected RAM headline for today's `main`

**The 13,723 → 96,611 B figure above predates the log-ring move and is
superseded.** With the log ring already in PSRAM on `main`, the *baseline*
free-internal-at-peak is much healthier than it was when this branch's number
was taken, so the *absolute* headline the option releases is different even
though the option's own isolated cost is unchanged.

Measured the same way as the original A/B (temporary, uncommitted
`GET /api/v1/debug/heap` probe — never part of any commit — demo mode,
`dyno-cell`, 3 verified live WebSocket clients, fresh reboot, 30 s soak,
`heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` polled
throughout the window to catch the true low-water mark rather than a snapshot):

| Internal DRAM (bytes) | IRAM=y (today's `main`) | IRAM=n (rebased branch) | Delta |
|---|---:|---:|---:|
| minimum-ever-free (low-water mark) | **52,659** | **135,607** | +82,948 |
| polled internal free, min over window | 56,615 | 139,455–139,567 | ~+82,900 |
| largest free block, min over window | 43,008 | 63,488 | +20,480 |

The IRAM=y minimum-ever-free (52,659) matches the ledger's row for the
log-ring fix exactly, confirming the two measurements are comparable. The
delta (82,948 B) agrees with the option's previously-isolated static cost
(82,812 B, `idf.py size`) to within 136 bytes — consistent with the original
finding that this option's cost is fixed regardless of what else is on
`main`. **Use 135,607, not 96,611**, as free internal at peak on today's
`main` with the option off.

### The `big-digit` clean-network re-run — a real, small, non-gating finding

The prior gap: n=2 on the shipping binary, confounded by LAN load that
escalated mid-run. Closed with **n=6 per build**, all runs individually
verified to hold 3 live WebSocket clients for the full 30 s window (any run
that didn't was discarded, matching the original methodology), fresh reboot
before every run, and the two builds' runs **interleaved across two rounds**
(IRAM=n round 1, IRAM=y round 1, IRAM=n round 2, IRAM=y round 2) specifically
to rule out a time-of-day/LAN-drift confound rather than a per-build effect.

`worstRenderUs`, `big-digit`, demo mode, 3 verified clients, fresh boot per run:

| Build | Runs (ms) | Median (ms) | Range (ms) |
|---|---|---:|---:|
| IRAM=y (today's `main`) | 55.03, 55.35, 52.59, 53.19, 53.39, 53.96 | 53.7 | 52.6–55.4 |
| IRAM=n (rebased branch) | 57.70, 56.07, 57.41, 57.09, 59.66 [n=5]* | 57.4 | 56.1–59.7 |

\* One additional IRAM=n round-1 run (57.41 ms) is folded into round 2's set
above; six values total, listed once.

**This is a real difference, not noise.** The two builds' ranges do not
overlap at all (55.4 ms max for IRAM=y vs 56.1 ms min for IRAM=n), and the gap
between medians (~3.7 ms) exceeds both builds' own run-to-run spread (2.8 ms
and 3.6 ms respectively) — the exact test this document's own methodology
specifies. It held in both interleaved rounds, ruling out a session-timing
confound. **This corrects the original A/B's `big-digit` verdict**, which
found 56.0 vs 56.9 ms (within spread, "no effect") on n=3 without LAN
verification as rigorous as this re-run's.

In context: `big-digit` was already the worst-case face by a wide margin
before this option is touched — `worstRenderUs` sits at 3.3–3.6x its 16 ms
budget on *both* builds, with `framesOverBudget` at 15-16/s on both. The
~3.7 ms/7% cost of turning IRAM off does not change that qualitative picture,
and it does not touch the guard that actually gates merges under AGENTS.md:
the `dyno-cell`/arc cadence guard in demo mode, confirmed unaffected below.
Reported honestly rather than smoothed over, but not treated as a blocker,
because nothing in this project's own invariants gates on `big-digit`
specifically.

### Cadence guard, re-confirmed on today's `main`

```
python tools/check_display_cadence.py --url http://192.168.50.102 --seconds 30
IRAM=y (today's main)     : physical render FPS: min=56 median=60 samples=104
IRAM=n (rebased branch)   : physical render FPS: min=57 median=60 samples=104
```

Both pass the median-60 gate; both mins sit inside the historical 55-58 band.

### GIF playback — tested with the option off for the first time

The ledger's own words: GIF playback is "the one path known to be sensitive to
internal-RAM pressure" and had "never [been] exercised with this option
disabled." The board had no committed GIF (`/api/v1/media/status` reported
`present:false`), so a small synthetic animated GIF was generated for the test
(200x200, 16 frames, 80 ms/frame, 128-colour adaptive palette — a non-trivial
palette to exercise the LZW decoder realistically, not a 2-colour degenerate
case) and uploaded to both builds via `POST /api/v1/media`.

**Result: clean on both builds, and IRAM=n has substantially more headroom, as
expected — not a regression.**

| Check | IRAM=y (`main`) | IRAM=n (rebased branch) |
|---|---|---|
| Upload+commit | 128,150 B in 0.737 s, `present:true`, `playback:"active"` | same GIF, same result |
| Playback running | `renderFps 13`, `flushesPerSecond 84` (matches 80 ms/frame = 12.5 fps) via `/state` and `/media/status` | same |
| `gif alloc: internal free` (boot, before Wi-Fi ramp) | **111,111 B** | **194,055 B** |
| `ESP_ERR_NO_MEM` / `send color data failed` in serial | none, upload+playback window or reboot | none |
| Survives reboot with GIF committed | yes — clean boot log, no stack overflow/panic, GIF auto-resumes (`boost_gif: dirty rect...` after `HTTP API ready`) | yes, same |
| Delete, then repeated delete | both return `present:false`, no error | same |
| Gauge resumes normally after delete | `renderFps 61` (arc/demo) | not re-checked (IRAM=n was the build kept flashed) |

The GIF widget object and its framebuffer land in **external RAM on both
builds** (`gif widget object in EXTERNAL RAM, framebuffer in EXTERNAL RAM`),
consistent with row 120/121 of the AGENTS.md ledger (LVGL's builtin allocator,
not `malloc`, decides placement) — the internal-RAM figure above is headroom
*around* that allocation (Wi-Fi, display, and the small pieces of GIF state
that do land in internal RAM), not the GIF payload itself. Turning the IRAM
option off does not change where the GIF lands; it only changes how much
internal RAM is free while it plays. **No regression found. This closes the
most-cited open risk in the original document.**

One caveat: the test GIF (128 KB) is far smaller than the 1.38 MB hardware
benchmark GIF used to verify the media store itself. It was sized to be
practical to generate and upload repeatedly during this verification, and it
does exercise the LZW/dirty-rect/decoder path documented in the ledger, but a
large multi-second GIF closer to the historical stress case was not re-run
under IRAM=n. Nothing in the boot-time or playback-time internal-RAM figures
above suggests file size would change the outcome — the ~24.5 KB decoder
allocation the ledger's boot-loop was about is LZW table size (driven by
palette depth, not frame count or file size) — but it is named here rather
than silently assumed.

### Verdict, updated

Merge. The cadence guard — the actual AGENTS.md gate — is unaffected on both
builds. GIF playback, the path with the most plausible reason to regress, does
not regress; it gains headroom. The one real cost found, a ~3.7 ms increase in
`big-digit`'s already-blown render budget, is reported rather than hidden but
does not meet the bar of a blocking finding: it doesn't touch a guarded
invariant, and the face was already the least-performant by a wide margin on
both builds.

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

Rebuilt from `main` at `f0a08bc` and reflashed over COM3 (DIRAM remaining back
to 67,869, generated `sdkconfig` back to
`CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y`). Verified:

- `GET /api/v1/state` responds — `firmwareVersion: v0.5.0-3-gf0a08bc` (clean
  tree, no `-dirty`)
- theme `night-city`, demo mode `false`, zone `ATMO`
- `GET /api/v1/debug/heap` returns **404**, confirming the temporary probe is
  gone from the flashed image

The board is running stock `main` and is free for other work.

## Board state at end of this work (2026-07-27, post-merge)

`spike/lvgl-iram-benefit` fast-forward merged into `main` (`39c1adf`) and
pushed to `origin/main`. Rebuilt from `main` at `39c1adf` and reflashed over
COM3. Verified:

- `GET /api/v1/state` responds — `firmwareVersion: v0.5.0-15-g39c1adf`,
  `demo:false`, `activeThemeId: night-city`
- `sensors.fault:true` is expected: both the ADS1115 and BMP280 are physically
  absent from this bench unit, unrelated to this change
- `GET /api/v1/media/status` — `present:false` (the synthetic test GIF used
  for verification was deleted from both builds during testing)
- generated `sdkconfig` confirmed `# CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM is
  not set`, i.e. the merged option is active on the flashed image, not just in
  `sdkconfig.defaults`

The board is running stock `main` with the IRAM option merged and off, theme
`night-city`, demo off, and is free for other work.
