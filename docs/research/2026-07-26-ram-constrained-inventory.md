# What was blocked by internal RAM — a complete inventory

Date: 2026-07-26
Scope: desk research only. No hardware was touched, nothing was built or
flashed, and no configuration was changed while producing this document.
Trigger: [`docs/research/2026-07-26-ble-wifi-coexistence.md`](2026-07-26-ble-wifi-coexistence.md)
measured `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` at **82,812 bytes of DIRAM**
against only **13,611 bytes** of internal free at peak on `main`.

The question this answers: *"What was marked not-an-option because of RAM? Just
the GIF?"*

**Short answer: no.** The GIF decoder is the most conspicuous case because it
boot-looped the board, but at least four other decisions were shaped by internal
RAM, and several more that *look* like RAM decisions are not — they are DMA
capability, PSRAM capacity, CPU rasterisation cost, or correctness. Keeping those
apart is the whole point of the exercise; this project has already lost time to
conflating them twice.

---

## 0. Definitions, because several past mistakes came from mixing these up

| Term | What it is on this board | Who competes for it |
|---|---|---|
| **Internal SRAM / DIRAM** | 341,760 B of on-chip RAM (`dram0_0_seg` in `build/boost_gauge.map`), addressable as either data or instructions. Every byte given to IRAM is a byte taken from DRAM. | everything below |
| **DRAM (internal data RAM)** | The data-side view of DIRAM. `.bss`, `.data`, and the internal heap. | Wi-Fi driver, LWIP, NimBLE, FreeRTOS stacks that touch flash, LVGL draw buffers, static arrays |
| **IRAM** | The instruction-side view of DIRAM. Code that must not take a flash-cache miss (ISRs), plus anything deliberately parked there for speed. | `LV_ATTRIBUTE_FAST_MEM_USE_IRAM`, `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT`, `BT_CTRL_RUN_IN_FLASH_ONLY=n` |
| **DMA-capable** | A *property* of internal DRAM, not a separate pool. ESP32-S3 SPI GDMA cannot stream from PSRAM at all. | LVGL draw strips, Wi-Fi static RX buffers |
| **PSRAM (external, SPIRAM)** | 8 MB octal. ~6.4 MB free at runtime (spike, `PSRAM free 6,390,652`). Cannot feed SPI GDMA. Higher latency; pointer-chasing inner loops suffer. | cached face canvases, GIF framebuffer, task stacks that never touch flash, `malloc` by default (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0`) |

Three consequences that recur throughout this document:

1. **Freeing IRAM returns DRAM.** Turning off an IRAM option does not "speed
   nothing up and free nothing" — it converts 82.8 kB of instruction space back
   into data space. The spike measured DMA-capable free going **5,823 → 106,235 B**
   in the control build, so the recovered memory really is usable by the display
   and the radio, not just by generic allocations.
2. **Total free is not the binding constraint; the largest contiguous block
   often is.** `main` at peak: 13,611 free but only **7,680 B largest block**.
   NimBLE needs ~22.5 kB contiguous. Wi-Fi does not merely consume internal
   DRAM, it fragments it (59,392 → 7,680 across Wi-Fi + httpd bring-up).
3. **PSRAM being nearly empty is irrelevant to any of this.** 6.4 MB free PSRAM
   never helped the GIF decoder, the radio, or a draw strip.

### How much is actually being unblocked

The "~100 KB" figure needs decomposing, because the spike's 114,023 B control
build had **two** changes, not one:

| Change | DIRAM recovered | Source |
|---|---:|---|
| `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n` | **82,812** | spike, `idf.py size` 78,953 → 161,765 |
| `CONFIG_ESP_WIFI_IRAM_OPT=n` + `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` | **17,700** | spike, `idf.py size` 53,945 → 71,645 |

So turning off **only** the LVGL option leaves an *estimated* **~96.4 kB** free
internal at peak (13,611 + 82,812 — arithmetic, **not measured in isolation**;
the spike never ran that configuration on its own). The measured 114,023 B
figure requires giving up the Wi-Fi IRAM optimisations as well, which on `main`
today are both still `=y` (`sdkconfig:1393`, `sdkconfig:1395`).

Throughout the table below, "would ~100 KB unblock it?" means the ~82.8 kB
LVGL-only lever unless stated otherwise.

---

## 1. The inventory

Every row cites its source. Where the historical record states no number, the
cell says **not recorded** rather than a guess.

### 1a. Genuinely constrained by internal RAM

| # | What was wanted | Internal RAM it needed | What happened | Source | Would ~100 KB unblock it? |
|---|---|---:|---|---|---|
| 1 | **GIF decoder's `GIFIMAGE` (LZW tables + palettes + file buffer) placed in internal RAM** for decode speed — PSRAM latency dominates a dependent pointer chase | **24,580 B** measured as the internal-free delta | **Reverted.** Boot-looped: `Guru Meditation LoadProhibited` through `wifi_softap_start` / `ieee80211_hostap_attach`. GIF loads at ~3 s, before SoftAP attach; taking 24.5 kB left ~49 kB internal and the Wi-Fi driver faulted on a failed alloc. Crash precedes the HTTP server, so OTA could not recover it — fix went over serial. Pointer indirection kept so a future attempt can place only the hottest few kB | `AGENTS.md:121`; commit `b002e45` body; `main/gif/boost_gif.c:272-279`; `main/gif/boost_gif.c:100-107` | **Yes**, with room to spare — 24.5 kB against ~96 kB. But see §4: the *benefit* was never measured, only the crash |
| 2 | **NimBLE + Wi-Fi coexistence (BLE telemetry to an Android app)** | **29,668 B** at `nimble_port_init()` + advertising start; **36,320 B** total runtime at peak; needs a **~22.5 kB contiguous** block; **6,512 B** static | **Was blocked on `main`, measured unblocked in the spike.** Every intermediate config failed traceably to internal DRAM. With the LVGL IRAM option off: single clean boot, 77,703 B free internal, 63,488 B largest block, cadence unaffected | `docs/research/2026-07-26-ble-wifi-coexistence.md` §"Measured numbers", §"What it took to make it fit"; `docs/plans/2026-07-26-bluetooth-android-app.md:20-57` | **Yes — already demonstrated on hardware.** The single strongest item on this list |
| 3 | **BLE controller code in IRAM** (`CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=n`), so BLE interrupts don't take a flash-cache miss | "**~20 kB** that this configuration does not have spare" — the spike's own estimate, not a measurement | **Deferred.** The working spike build needed `RUN_IN_FLASH_ONLY=y` to fit. Its Kconfig help warns of a performance cost; it is one of two unseparated candidate causes for HTTP p50 going 25 ms → 88–181 ms | `docs/research/2026-07-26-ble-wifi-coexistence.md:143-146`, §"What I could NOT test" | **Probably** — but note this consumes IRAM, so it partly re-spends what the LVGL change frees. Needs measuring, not assuming |
| 4 | **Keeping `CONFIG_ESP_WIFI_IRAM_OPT=y` / `CONFIG_ESP_WIFI_RX_IRAM_OPT=y` while also running BLE** | **17,700 B** of DIRAM (spike: 53,945 → 71,645) | **Constrained.** The spike had to switch both off to recover HTTP responsiveness under NimBLE. On `main` today both are still `=y` | `docs/research/2026-07-26-ble-wifi-coexistence.md:161`; `sdkconfig:1393`, `sdkconfig:1395` | **Yes** — this is the "don't have to pay it" case. ~82.8 kB is nearly 5x this cost |
| 5 | **Larger LVGL draw strips: 40 lines** (37,280 B/buffer, 74,560 B double-buffered — **+37,280 B internal DMA** over the 20-line production setting) | 74,560 B DMA-capable internal, vs 37,280 B today | **Rejected.** "Wi-Fi route disappeared after flash/load" — the textbook internal-DRAM-exhaustion signature | `README.md:124`; `AGENTS.md:63` | **RAM-wise yes.** But see §3: there is no measured benefit to chase, so unblocking it is not a reason to do it |
| 6 | **Larger LVGL draw strips: 28 lines** (26,096 B/buffer, 52,192 B double — **+14,912 B** over production) | 52,192 B DMA-capable internal | **Rejected.** "HTTP became unresponsive under load." Cause not diagnosed in the record; consistent with httpd/LWIP losing internal allocations, but **not recorded as a RAM measurement** | `README.md:123` | **Probably**, if the mechanism was internal-DRAM pressure. The record does not say it was |
| 7 | **Untrimmed Wi-Fi buffer pools.** Production runs `STATIC_RX_BUFFER_NUM=6`, `STATIC_TX_BUFFER_NUM=6`, `CACHE_TX_BUFFER_NUM=8`, `RX_BA_WIN=6`, `MGMT_SBUF_NUM=8`, all below IDF defaults | **not recorded** — no byte figure was ever written down for this trim | **Constrained**, in the same commit that moved Wi-Fi/LWIP out of the internal DMA heap. The stated rationale is workload ("a four-client control page does not need throughput-oriented Wi-Fi pools"), not scarcity, but static RX buffers must be DMA-capable internal, so the effect is real | `sdkconfig.defaults:29-34`; commit `d2316eb` diff | **Yes, if anyone wants it.** No evidence anyone does — no throughput complaint is recorded |
| 8 | **`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536`** — a standing 64 kB internal reservation so display DMA and Wi-Fi can coexist | 65,536 B held back from the general allocator, permanently | **Constraint by design**, not a deferral. Explicitly called out as load-bearing in both the README and the AGENTS invariants | `sdkconfig.defaults:21-24`; `README.md:78-80`; `AGENTS.md:63` | **Do not touch it.** ~100 KB makes it cheaper to keep, not safe to remove |
| 9 | **Boot-time media CRC validation with a 4,096 B scratch buffer on the stack** | 4,096 B on the 3,584 B ESP-IDF main-task stack | **Reworked, not deferred.** Repeatable `main` task stack overflow before networking. Fixed by CRC-ing through a temporary read-only `esp_partition_mmap` instead | `AGENTS.md:108` | **No, and it doesn't matter** — this was stack sizing, not heap scarcity, and the mmap fix is strictly better than a bigger stack |
| 10 | **NimBLE host mempools in internal RAM** (spike) | **not recorded** separately | **Constrained** — moved to PSRAM as one of the cumulative steps needed to fit | `docs/research/2026-07-26-ble-wifi-coexistence.md:160` | **Possibly**, but no reason to: host mempools are not latency-critical the way the controller is |
| 11 | **`CONFIG_BT_CTRL_BLE_MASTER=n`** — set specifically *to save memory* | **not recorded** | **Rejected on correctness, not size.** Its help text ("Enable BLE connection feature") does not describe the central role; with it off the controller reports `CONNECT:0` — an unconnectable peripheral. Must stay `y` | `docs/research/2026-07-26-ble-wifi-coexistence.md:175-179`; `AGENTS.md:148` | **N/A** — a memory-saving attempt that was wrong for a non-memory reason |
| 12 | **`CONFIG_BT_LE_MAX_CONNECTIONS` / `BT_LE_50_FEATURE_SUPPORT`** memory tuning | zero — the symbols do not exist on ESP32-S3 | **No-op.** Accepted silently into `sdkconfig.defaults` as unknown symbols. Same failure class as the phantom 60 MHz QSPI trial | `AGENTS.md:148`; `docs/research/2026-07-26-ble-wifi-coexistence.md:180-183` | **N/A** |

### 1b. Looks like a RAM decision, isn't

| # | Item | Why it is *not* an internal-RAM constraint | Source |
|---|---|---|---|
| 13 | **LVGL draw buffers in PSRAM** (the stock BSP's `use_psram = true`) | A **DMA capability** limit, not a size limit. ESP32-S3 SPI GDMA cannot stream from PSRAM; the bounce-buffer path returns `ESP_ERR_NO_MEM` and the panel never completes a clean full-frame update (half-white/half-green). More free internal RAM does not make PSRAM DMA-capable | `README.md:51-56`, `README.md:71-76`; `AGENTS.md:100`; `main/boost_display.c:19-24`, `:529`; `main/boost_display.h:15-18` |
| 14 | **24-line draw strips** (+7,456 B internal over production) | **Reverted for a subjective reason** — "the live mirror was reported subjectively laggier" — and it produced **no measured physical-FPS gain**. Not a RAM rejection at any point | `README.md:122` |
| 15 | **`heap_caps_malloc_extmem_enable(64 KB)` around GIF widget creation** | An **allocator-targeting** failure, not a capacity failure. Internal free moved by 112 B: the threshold only affects `malloc`, and the ledger records LVGL as allocating from its own builtin pool. Calls removed as no-ops. **⚠ See §4.1 — the recorded root cause is contradicted by the current tree** | `AGENTS.md:120`; commit `b002e45` body; `main/boost_gauge.c:646-648` |
| 16 | **TE / tearing fix by region double-buffering** | Recorded as "a larger change", and the RAM it would need is *far* beyond this lever. The needle's dirty band is 205 of 466 rows → 205 × 466 × 2 = **191,060 B** of DMA-capable internal for that region alone (*arithmetic from the recorded row count, not a measurement*); a full frame is 434 KB. ~100 KB does not reach it. The actual blocker is architectural: TE-at-vblank only fixes the phase of the *first* strip on a CPU-rasterised partial pipeline | `AGENTS.md:123`, `AGENTS.md:129`, `AGENTS.md:125` |
| 17 | **24 cached full-screen Big Digit grounds** | Wants **10 MB** — that is **PSRAM**, and it exceeds the ~6.4 MB free. Also pointless: a flat fill is already LVGL's cheapest primitive, so there is nothing to pre-compute | `README.md:509-511`; `AGENTS.md:114` |
| 18 | **Cached static face art** (Vault, Night City) | **Enabled by PSRAM, never internal-limited.** 434 KB PSRAM `lv_canvas` per face, painted once at scene build. Biggest measured win in the project (Vault median 37 → 61 FPS, min 4 → 54, while *adding* vignette and scanlines) | `README.md:496-504`; `AGENTS.md:115`; `main/boost_gauge.c:31`, `:1490`, `:1922` |
| 19 | **AnimatedGIF turbo LZW path** (`pTurboBuffer = NULL`) | Disabled for **correctness** — turbo failed delta/disposal composition. A turbo buffer would want *more* memory (`LZW_BUF_SIZE_TURBO`), but that is not why it is off | `AGENTS.md:50-58`; `main/gif/boost_gif_dec.c:137-141`; `main/gif/boost_gif_dec.h:59`, `:105` |
| 20 | **8-bit palette-index plane removal** in the vendored decoder | Freed **212 KiB of PSRAM** and cut 434 KB of per-frame PSRAM traffic. Nothing to do with internal DRAM | commit `7633c41` body; `AGENTS.md:50-58` |
| 21 | **Browser GIF preview disabled; device-pixel-ratio capped at 2** | **Browser-side** memory and render cost. No relationship to firmware DRAM | `AGENTS.md:38`, `AGENTS.md:103` |
| 22 | **Needle judder / frames-over-budget / per-face FPS** | **CPU rasterisation cost.** Falsified twice already — not the 0.35° gate, not `LV_DRAW_THREAD_PRIO`. The ESP32-S3 has no 2D accelerator; the panel link is ~6% utilised | `AGENTS.md:122`, `AGENTS.md:126`, `AGENTS.md:115` |
| 23 | **QSPI clock as a general lever** | **Link-bound operations only.** 40 → 80 MHz moves the Big Digit full-screen recolour −17% and the partial-update faces 0–3% | `README.md:530-534`; `AGENTS.md:133` |
| 24 | **Debug snapshot buffer** (`GET /api/v1/debug/snapshot`) | 466×466×2 = 434 KB explicitly allocated `MALLOC_CAP_SPIRAM`. Never contended for internal | `main/boost_web.c:932`; `README.md:631-636` |
| 25 | **Raw dual-slot media store replacing SPIFFS** | A **flash** architecture problem (GC, staging, atomicity), not RAM | `AGENTS.md:101`; `README.md:323-336` |

---

## 2. Genuinely unblocked — ranked by value to effort

**1. BLE + the Android app (plan phases 0–2).** *High value, effort already
largely spent.* This is the only item on the list where the unblocked state has
been **observed on hardware**: single clean boot, BLE advertising alongside
Wi-Fi STA + SoftAP + httpd + 3 WebSocket clients + the display, 77,703 B free
internal, 63,488 B largest block, cadence min 57 / median 60. Phase 0 was
written as a go/no-go gate and it passed. Caveats that are *not* about RAM and
must not be glossed: no BLE central ever saw the advertisement, throughput is
entirely unmeasured, cadence under an active connection is untested, and the
HTTP p50 penalty (25 → 88–181 ms) is real and unattributed. Also gated on the
non-RAM safety item in the plan — one full USB flash to actually activate the
OTA rollback net, which has never been written to this board.
Source: `docs/research/2026-07-26-ble-wifi-coexistence.md`;
`docs/plans/2026-07-26-bluetooth-android-app.md:73-89`.

**2. Not paying for the Wi-Fi IRAM optimisations.** *Moderate value, near-zero
effort.* The spike had to set `ESP_WIFI_IRAM_OPT=n` and `ESP_WIFI_RX_IRAM_OPT=n`
to recover HTTP responsiveness with NimBLE present — a 17,700 B DIRAM recovery
bought by slowing the Wi-Fi path. With 82,812 B available from the LVGL option
alone, that trade is no longer forced: `main` can keep both `=y` and still fit
BLE. This is purely "stop giving something up", so it costs nothing to take.
Source: `docs/research/2026-07-26-ble-wifi-coexistence.md:161`; `sdkconfig:1393,1395`.

**3. BLE controller in IRAM (`BT_CTRL_RUN_IN_FLASH_ONLY=n`).** *Moderate value,
moderate effort, genuinely diagnostic.* It is one of exactly two candidate
causes for the 3.5–7× HTTP latency regression under BLE, and the spike could
not separate them because the IRAM build did not fit. It does now (spike
estimate: ~20 kB). Worth doing **as an experiment first** — if latency recovers,
the answer is "flash-cache misses", and if it does not, the answer is
"coexistence arbitration" and the product decision changes. Note this spends
IRAM back, so it must be measured against the same peak-load heap probe.
Source: `docs/research/2026-07-26-ble-wifi-coexistence.md:143-146`, §"What I could NOT test".

**4. GIF decoder state (or just its hottest arrays) in internal RAM.** *Low
value, low effort, unmeasured upside.* 24,580 B against ~96 kB is comfortable,
and the `GIFIMAGE` pointer indirection was deliberately preserved for exactly
this. If a partial placement is preferred, the two large hot arrays are
`usGIFTable` (4,096 × `uint16` = **8,192 B**) and `ucGIFPixels`
(2 × 4,096 = **8,192 B**), plus `ucFileBuf` (**4,096 B**) — *sizes computed from
the header constants, not measured*. The reason this ranks fourth despite being
easy: **nobody has ever measured what it buys.** The attempt boot-looped before
producing an FPS number, and the surrounding GIF optimisation work delivered
+3% (14.55 → 15 FPS) against a predicted +17%. GIF playback is also an exclusive
path that the cadence guard explicitly does not defend. Do not repeat the
mistake of taking internal RAM for an unpriced speed benefit — that is the exact
pattern this whole document exists to document.
Source: `main/gif/boost_gif.c:272-279`; `main/gif/boost_gif_dec.h:222-231`;
`AGENTS.md:121`; commit `b002e45` body.

**5. Larger draw strips (28 or 40 lines).** *Low value, low effort, evidence
says don't.* RAM-wise these become affordable (+14,912 B and +37,280 B). But the
24-line test — the only one that ran cleanly enough to measure — showed **no
physical-FPS gain**, and the README is explicit that perceived mirror lag is a
network/canvas problem rather than a strip-throughput one. `AGENTS.md:64` forbids
enlarging strips "for an unmeasured speed claim". Unblocking is not the same as
justifying.
Source: `README.md:114-132`; `AGENTS.md:63-64`.

**6. Restoring Wi-Fi buffer pools to IDF defaults.** *Speculative value, low
effort.* Available if throughput ever becomes a complaint. Nothing in the record
says it is one. Leave alone until something asks for it.
Source: `sdkconfig.defaults:29-34`.

**7. General headroom.** Larger httpd stacks, more open sockets, wider JSON
buffers, additional internal-DRAM tasks. Nothing specific is blocked today, but
the current margin — 13,611 B free with a 7,680 B largest block — is why any of
those would have been risky. Worth knowing the constraint is lifted before the
next feature runs into it.

### The price of the lever, which is not yet paid

Every item above is contingent on turning off `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM`,
and **that has been validated on exactly one face.** The spike measured
`dyno-cell` (arc) in demo mode: min 57 / median 60 against a min 58 / median 60
baseline — unchanged. The three richer faces (`vault`, `night-city`, `bigdigit`)
are more rasterisation-bound and were **not** measured, and the ledger requires
any style-affecting change to be re-checked **per face** on `worstRenderUs` and
`pixelsPerSecond`, never on `renderFps`. The option's *benefit* has never been
measured on any face; only its cost is known.
Source: `AGENTS.md:147`, `AGENTS.md:111`, `AGENTS.md:116`;
`docs/research/2026-07-26-ble-wifi-coexistence.md:321-325`.

---

## 3. Still blocked, or never RAM-limited

| Item | Status after ~100 KB | Why |
|---|---|---|
| LVGL draw buffers in PSRAM | **Still impossible** | ESP32-S3 SPI GDMA cannot stream from PSRAM. A capability, not a quantity. `README.md:51-56` |
| TE region double-buffering (the real tearing fix) | **Still blocked** | Needs the whole dirty region DMA-capable and internal: ~191 kB for the needle band by arithmetic, 434 KB for a frame. ~100 KB is not close. `AGENTS.md:129` |
| 24 cached Big Digit grounds | **Still blocked, and still pointless** | 10 MB of PSRAM against ~6.4 MB free, to cache a flat fill. `README.md:509-511` |
| Turbo LZW decode | **Unaffected** | Off for delta/disposal correctness. `AGENTS.md:50-58` |
| 24-line strips | **Unaffected** | Reverted for subjective mirror lag with no FPS gain. `README.md:122` |
| `heap_caps_malloc_extmem_enable` | **Unaffected** | Wrong allocator targeted; a capacity change does not fix aim. `AGENTS.md:120` |
| Boot CRC scratch buffer | **Unaffected** | Stack sizing; already fixed better via mmap. `AGENTS.md:108` |
| Needle judder / per-face frame drops | **Unaffected** | CPU rasterisation cost. Falsified twice as a scheduling or gating problem. `AGENTS.md:122`, `:126` |
| QSPI clock for partial-update faces | **Unaffected** | Only full-frame pushes are link-bound. `AGENTS.md:133` |
| Browser GIF preview, DPR cap | **Unaffected** | Browser-side. `AGENTS.md:38` |
| SPIFFS → raw media store | **Unaffected** | Flash architecture. `AGENTS.md:101` |
| `SPIRAM_MALLOC_RESERVE_INTERNAL=65536` | **Keep** | Load-bearing invariant so display DMA and Wi-Fi coexist. `README.md:78-80` |

---

## 4. Explicit uncertainty

Everything in this section is a gap in the record, not a conclusion.

### 4.1 The "wrong allocator" root cause is contradicted by the current tree

`AGENTS.md:120` records the mechanism as: `CONFIG_LV_USE_CLIB_MALLOC=y` is set,
but `lv_conf_internal.h` reads `CONFIG_LV_USE_STDLIB_MALLOC`, which is never
defined, so LVGL falls back to `LV_STDLIB_BUILTIN` and allocates from its own
pool, never `malloc` — therefore `heap_caps_malloc_extmem_enable` could not
reach it.

**In the tree as it stands today, that chain does not hold:**

- `managed_components/lvgl__lvgl/src/lv_conf_kconfig.h:44-45` explicitly bridges
  the symbol: `#elif defined(CONFIG_LV_USE_CLIB_MALLOC)` → `#define CONFIG_LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB`.
- `lv_conf_internal.h:38` includes `lv_conf_kconfig.h` **before** the
  `#ifndef LV_USE_STDLIB_MALLOC` fallback at lines 106-112, so the fallback is
  not taken.
- The link map is decisive: `build/boost_gauge.map:114608` resolves
  `lv_malloc_core` to `liblvgl__lvgl.a(lv_mem_core_clib.c.obj)`, and
  `lv_mem_core_builtin.c.obj` **does not appear in the map at all**. LVGL in
  this build calls `malloc()`.
- `CONFIG_SPIRAM_USE_MALLOC=y` (`sdkconfig:1218`), so
  `heap_caps_malloc_extmem_enable()` is functional in principle.

**What I am *not* claiming.** The empirical result stands unchallenged: internal
free moved by 112 B, so the call did nothing. I cannot say why. Two candidate
explanations, and the record cannot distinguish them:

1. The LVGL version present when the experiment ran (commit `b002e45`, ~2026-07-22)
   lacked the Kconfig bridge, and the component has since been refreshed.
   **This is unverifiable from the repository**: `dependencies.lock`,
   `sdkconfig`, and `managed_components/` are all in `.gitignore` (`.gitignore:1-8`),
   so `git log -- dependencies.lock` returns nothing and there is no record of
   which LVGL version was in the tree at any past commit. The tree today pins
   **lvgl/lvgl 9.4.0**.
2. The stated mechanism was wrong at the time too, and the 112 B result has some
   other cause that was never chased.

**Consequence for this inventory:** row 15 stays classified as "not a capacity
constraint" either way — nothing about 100 KB of extra internal RAM changes an
allocator-aim problem. But the *lesson* recorded in `AGENTS.md:120` ("a Kconfig
symbol existing is not evidence it is the one being read") may be resting on an
incorrect diagnosis, and anyone revisiting GIF decoder placement should verify
empirically rather than trusting either the ledger row or this paragraph. The
ledger's own guard applies to itself: confirm placement with
`esp_ptr_external_ram()` plus free-heap deltas.

**The build tree caveat:** `build/boost_gauge.map` (timestamped 2026-07-26 13:50)
was produced by another agent's in-progress work on branch
`spike/lvgl-iram-benefit`, whose `sdkconfig` I did not read as authoritative for
`main`. The specific facts I take from it — which stdlib malloc core the linker
resolved, and the size of a static array in `libmain.a` — are not affected by
the IRAM option under test, but they are one build's evidence, not `main`'s.

### 4.2 Numbers the record does not contain

| Question | Status |
|---|---|
| What does `LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` actually *buy*, on any face? | **Never measured.** Only its cost (82,812 B) is known. It was introduced in the initial commit `6812b20` with no rationale and no measurement, and went unpriced for the project's whole history |
| Cadence with the option off, on `vault` / `night-city` / `bigdigit` | **Not measured.** Only `dyno-cell` |
| Free internal at peak with *only* the LVGL option off | **Not measured.** ~96,423 B is my arithmetic (13,611 + 82,812). The measured 114,023 B control also had both Wi-Fi IRAM opts off |
| What the 24.5 kB internal GIF placement would have bought in FPS | **Never measured** — it boot-looped first |
| Which of `usGIFTable` / `ucGIFPixels` / `ucFileBuf` are the "hottest few kB" | **Not measured.** My byte figures are arithmetic from `main/gif/boost_gif_dec.h:222-231`; no profiling exists |
| Free-RAM figures at the 28-line and 40-line strip failures | **Not recorded.** Only the symptoms ("HTTP became unresponsive", "Wi-Fi route disappeared") |
| Internal DRAM saved by the Wi-Fi buffer pool trim | **Not recorded.** No byte figure anywhere |
| Whether BLE's HTTP latency penalty is coexistence or `RUN_IN_FLASH_ONLY` | **Explicitly unseparated** by the spike |
| BLE throughput, MTU, connection-interval behaviour, cadence under an active connection | **Entirely unmeasured** — no central was ever available |

### 4.3 Two documentation inconsistencies found while doing this

Recorded because they touch memory claims, not because they are in scope to fix.
**Neither was changed.**

- **`sdkconfig.defaults:36-37`** says *"The LVGL worker and HTTP server do not
  perform flash writes; keep their stacks in PSRAM"* and sets
  `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y`. But `main/boost_web.c:1528`
  sets `cfg.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` with
  `cfg.stack_size = 10240`, so the httpd task stack is **internal**, not PSRAM.
  Forcing it internal is almost certainly correct — the httpd task performs OTA
  flash writes, and a PSRAM stack cannot survive a disabled cache — but the
  comment describes the opposite of what the code does. 10,240 B of internal
  DRAM is not nothing at a 13,611 B budget. The LVGL worker half of the claim
  *is* honoured: `main/boost_display.c:607` sets `adapter_cfg.stack_in_psram = true`.
- **`README.md:340`** still refers to *"the active 60 MHz CO5300 QSPI trial"*.
  That trial is the phantom setting corrected in `AGENTS.md:131` and
  `README.md:134-145`; the clock is 80 MHz and owned by `BOOST_LCD_PCLK_HZ`.
  One stale sentence survived the correction.

### 4.4 An internal-DRAM consumer nobody has priced

Not a deferred feature and not something ~100 KB unblocks, but it belongs here
because it is the same failure pattern as the LVGL IRAM option — a static
internal allocation that no document mentions:

`main/boost_model.c:34` declares `static boost_log_sample_t s_logs[BOOST_LOG_CAPACITY]`
with `BOOST_LOG_CAPACITY = 1800` (`main/boost_model.h:18`). The link map places
it at `.bss.s_logs 0x3fcbc8f0 0xa8c0` — **43,200 bytes of internal DRAM**, in
`.bss`, permanently. That is larger than NimBLE's entire 36,320 B runtime cost,
spent on the 2 min 24 s telemetry ring described at `AGENTS.md:35`. It was by
some margin the largest single static internal allocation from `libmain.a` in
the map I inspected.

`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY` is not set (`sdkconfig:1223`), so
`.bss` is internal by default; the array carries no `EXT_RAM_BSS_ATTR`. Nothing
in the ring is DMA-touched or accessed from an ISR, and it is written at 12.5 Hz
and read only on CSV export, so PSRAM latency would be irrelevant to it. I have
**not** verified that moving it is safe or that it does not need to survive a
cache-disabled window — that requires reading the write path and the export
path properly, and this was a documentation task. Flagging it, not proposing it.

---

## 5. So, was it just the GIF?

No. Ranked by how firmly the record supports "internal RAM was the blocker":

1. **BLE** — measured, unambiguous, and the largest thing that was off the table.
2. **The GIF decoder's LZW state** — the one that actually crashed a board, and
   the one everyone remembers.
3. **The BLE controller in IRAM** — deferred with a stated (estimated) shortfall.
4. **The Wi-Fi IRAM optimisations** — a live, ongoing trade `main` is still making.
5. **40-line draw strips** — rejected with a textbook DRAM-exhaustion symptom.
6. **28-line draw strips** — rejected with a symptom consistent with it, though
   never diagnosed as such.
7. **Wi-Fi buffer pools** — trimmed alongside the internal-DMA reservation, with
   a workload rationale and no recorded byte cost.

And the framing correction that matters more than the list: the scarcity was
never a Wi-Fi appetite problem or a hardware verdict. It was **one unpriced
display tuning option** holding 82,812 bytes, introduced in the first commit and
carried for the project's entire history without anyone measuring what it bought.
The GIF boot loop, the BLE `ESP_ERR_NO_MEM`, the 7,680 B largest free block and
the 40-line strip failure are all the same fact seen from four different angles.
