# GIF playback

This page covers GIF upload/storage and the playback performance contract.

## Layered GIF pipeline

This is a hybrid pipeline, **not a project-written GIF decompressor**:

- **Custom upload/storage:** `web/app.js` uploads to `main/boost_web.c`; the request is streamed into the raw dual-slot CRC/committed-header store in `main/boost_media_store.c/.h`. The selected payload is mapped with `esp_partition_mmap`.
- **Custom gauge integration:** `main/boost_gauge.c/.h` runs under the display lock, hides the gauge, creates and centers a black-backed `lv_gif` widget, selects RGB565, and gives it an `lv_image_dsc_t` whose data points at the mapped GIF bytes. The load path maps before creating/feeding the descriptor.
- **Third-party decode/playback:** LVGL's `lv_gif` wrapper in `managed_components/lvgl__lvgl/src/libs/gif/lv_gif.c` calls bundled AnimatedGIF (`AnimatedGIF/src/gif.c`) via `GIF_openRAM` and `GIF_playFrame`. LVGL's timer supplies frame timing; parsing, LZW decode, and delta/disposal composition remain in those components.
- **Intentional local LVGL changes:** `lv_gif.c` zero-initializes the full framebuffer to prevent stale AMOLED strips and forces `pTurboBuffer = NULL` so the standard decoder composes delta/disposal frames correctly; this does not make decompression custom. The framebuffer is the full canvas.

The mapped partition data must remain valid until the widget is destroyed. The shutdown order is display lock → destroy `lv_gif` widget → unmap partition data. The flush path remains the custom internal-DMA partial CO5300 path in `main/boost_display.c`; it is separate from GIF parsing and decoding.

## Upload and storage

The `media` partition is a raw dual-slot store at offset `0x820000`, size `0x7E0000`. Each upload targets the inactive slot: firmware erases only the required aligned range, streams the GIF payload while computing its CRC, and writes the committed header last. On boot, the store scans CRC-valid headers and selects the newest generation. Playback uses an LVGL variable file descriptor over the selected slot; there is no file-copy activation step.

The full **1,379,129-byte** GIF upload completed in **7.504 s** on hardware. Because the previous slot is not replaced until the new header is committed, an aborted upload preserves the previous GIF. Two repeated deletes succeeded; after deletion the physical gauge resumed and PSI changed.

GIF playback is exclusive. A native **466×466** clip fills the AMOLED; smaller clips remain at their native dimensions, centered on a pure-black AMOLED background. Upload accepts sources up to **466 × 466 px**.

The upload and delete operations are cancellation-safe. Clicking **Delete** during an upload aborts the browser XHR, waits for that request to settle, and then sends `DELETE`. The server rejects overlapping delete/upload operations with **409 Conflict**, preventing the delete from racing an in-progress slot commit.

## Animation performance contract

GIF playback is an exclusive LVGL path. A 466×466 RGB565 frame is about **434 KB**. The transfer cost of a strip is **linear in its size**, measured by sweeping transfer sizes from 932 B to 37,280 B at three clocks (40 reps each, completion-to-completion deltas, no chunking boundary anywhere in that range):

| clock | slope | link rate | vs theory | fixed intercept |
|-------|-------|-----------|-----------|-----------------|
| 80 MHz | 0.02539 µs/B | **39.4 MB/s** | 98.5% | 106.3 µs |
| 40 MHz | 0.05041 µs/B | 19.8 MB/s | 99.2% | 108.2 µs |
| 20 MHz | 0.10045 µs/B | 10.0 MB/s | 99.6% | 115.1 µs |

The slope halves exactly with the clock while the intercept does not move, which separates the two costs: the bus itself runs at essentially the full arithmetic rate, and there is a **clock-independent ~106 µs of software overhead per transfer** — three separate blocking calls (CASET, RASET, RAMWR), each with its own bus acquire/release. The raw command bits account for under 1 µs of it.

A full frame is 24 transfers (23 × 20 lines + one 6-line remainder), so the transfer-only floor is **13.6 ms (≈74 FPS)** before GIF decoding and LVGL rendering. Of that, 11.0 ms is pixel data and 2.5 ms is per-transfer overhead.

The clock is confirmed at the requested rate two independent ways: `spi_device_get_actual_freq()` returns exactly 80000/40000/20000 kHz for the three requests, and the measured throughput tracks theory to within 1.5%. The pins are entirely GPIO-matrix routed (PCLK=GPIO38 is not an SPI2 IOMUX pin), but that costs nothing here — the IOMUX-vs-matrix frequency penalty is **ESP32-only** and does not apply to the S3 at or below 80 MHz.

> The 999 µs/strip figure from an earlier revision was never committed as inspectable code, so its cause could not be found. The lesson is not "measure instead of calculating" — it is that a measurement nobody can re-run is not evidence. The table above comes from a harness that was committed before its numbers were quoted.

The uploaded 466×466 fixture (`IMG_5325-ezgif.com-optimize (2).gif`) is 1,379,129 bytes, 101 frames, 3.37 seconds, and nominally 30 FPS. On the board it measured **14–20 physical renders/s** (median 20) with no serial display, memory, panic, or reset errors — decoder/renderer-bound rather than a panel-transfer failure.

### Direct panel push (2026-08-16)

The decode/render split is measured on hardware with a per-frame `perf:` serial line (120-frame windows): on the 98%-full-frame fixture above, decode costs **36.4–37.7 ms/frame** while the panel transfer costs **15.7–17.6 ms** against the 13.6 ms pure-transfer floor. Playback is therefore decode-bound, not transfer-bound.

`main/gif/boost_gif.c` pushes each decoded frame's rect straight to the panel through `boost_display_push_bitmap()` (`main/boost_display.c`), which reuses the region-dbuf internal DMA scratch strips and the same 20-line chunking + single TE wait as `region_dbuf_writeback()`. The redundant LVGL re-blit of the (already panel-ready RGB565) framebuffer is skipped on success. The push is refused (and playback falls back to the ordinary bounded LVGL invalidation) whenever panel rotation ≠ 0, the strips are not allocated, or the placement is not a plain 1:1 blit — no new internal RAM is allocated. Measured: 119/120 direct pushes, 0 fallbacks, ~18.7 FPS on this worst-case fixture (vs 14–20 baseline); the gauge cadence guard still passes after teardown (dyno-cell demo, min 57 / median 61). Remaining levers for 30/60 FPS are pipelining decode against push and decode acceleration, both gated on the `perf:` line's measurements.

The push byte-swaps each strip to big-endian RGB565 in the internal scratch copy — the CO5300 wire format. The LVGL path gets this swap from the adapter bridge (`lv_draw_sw_rgb565_swap` for this panel profile, applied before the custom draw-bitmap hook); the direct push bypasses the bridge, so it must apply the same transform itself (the first flash skipped it and showed fully scrambled colors). The swap costs ~2.6 ms per full frame, keeping the effective push at ~18.3 ms.

Do not use `check_display_cadence.py` while GIF media is active: its ≥60 FPS threshold defends the live gauge path, not full-frame animation playback.

For smoother animation, reduce frame rate, palette complexity, changed-pixel area, or use a purpose-built low-area LVGL animation. Native 466×466 GIFs avoid additional scaling work, but still need to fit the decode/render budget.
