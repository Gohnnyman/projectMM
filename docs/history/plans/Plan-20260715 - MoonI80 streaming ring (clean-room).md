# Plan — MoonI80 streaming ring, clean-room rebuild (256 lights/strand in shift mode)

## Context

**The goal:** drive **256 lights/strand** (target 48×256) through a 74HCT595 expander on the ESP32-S3, up from today's **~96/strand** cap.

**The measured blocker (ADR-0014, board B):** in shift mode a >96-light frame exceeds the largest internal DMA block (~42 KB) and falls to PSRAM — and the S3's GDMA **cannot sustain a PSRAM read at the expander's 26.67 MHz clock** (controlled experiment: same board/PSRAM/chain, direct mode at 2.67 MHz streams a PSRAM frame fine, shift mode never completes at any size). So the fix is not "make PSRAM faster" — it is **never let the DMA read PSRAM at the shift clock.**

**The mechanism:** never materialise the full frame. Loop the DMA over a small ring of **internal** buffers; as each drains, the CPU encodes the next slice straight into it, reading the tiny (internal, ~24× smaller) Layer buffer. PSRAM leaves the path entirely. Espressif calls this "bounce buffers"; hpwit arrived at it independently. Only *we* can build it because we own the descriptor chain, the EOF hook, and the single never-re-armed `lcd_ll_start` — `esp_lcd` can express none of it.

**Why clean-room, not un-stash:** a prior ring attempt (in `stash@{0}`) *streamed* at 256 but **rendered wrong**, root cause never found, after three live-patch mitigations. Per PO: build fresh against the current codebase, be critical of the existing scaffolding, and use the **now-working loopback bit-verifier** (2304/2304 on a real strand, landed in `2873ec9`) as the instrument the first attempt lacked. The stash is NOT read.

**What already exists in the current tree (the foundation):**
- `ParallelLedDriver::encodeRows<Slot>(outCh, dst, firstRow, rowCount, closeFrame)` — **already slice-ready**: writes to `dst+0` for any `firstRow`, `closeFrame` gates the trailing latch pad, loop is per-row. Its own comment names "the streaming ring (MoonI80's phase-2 path)" as the caller. Proven byte-identical to a whole-frame encode by an existing host test.
- `prefillShiftFrame<Slot>` — writes shift-mode constants per *run of equal-mask rows*; the exhausted-strand handling (a short strand must stop being clocked or it flashes white) is already correct.
- `platform.h` **already declares** the ring seam: `moonI80Ws2812InitRing`, `moonI80Ws2812TransmitRing`, `moonI80Ws2812IsRing`, and `MoonI80EncodeFn`. **No definitions exist anywhere** (declared-only) — so the contract is a draft to interrogate, and the implementation is genuinely fresh.

## Critical findings against the existing scaffolding (what I will CHANGE, not inherit)

The committed platform.h ring block and `MoonI80EncodeFn` are a *draft*. Interrogated against the current code and the PO decision, three things are wrong or unsettled and get fixed in this build:

1. **The contract says refill runs "in the EOF ISR, in IRAM" — WRONG per the settled decision.** PO chose **task refill**. Nothing in `src/light/` is `IRAM_ATTR`; there is no ISR→domain callback anywhere in this repo (`platform_esp32_ir.cpp` states the opposite convention explicitly). The 3.6× drain-vs-encode margin makes ISR determinism unnecessary. **Fix:** the EOF ISR does one `xSemaphoreGiveFromISR`; a pinned high-priority task calls `MoonI80EncodeFn`. Rewrite the platform.h comment to describe the task, and drop "must be IRAM-safe" from the callback contract. The seam signature stays identical, so a future ISR escalation is "who calls it," not a rewrite.

2. **`MoonI80EncodeFn`'s `firstRow/rowCount/closeFrame` shape is right — keep it**, because it maps 1:1 onto `encodeRows`. This is the one part of the draft that is correct and load-bearing; do not redesign it.

3. **Frame termination is the genuinely delicate part, and the prefetcher is the named adversary.** The current whole-frame `startTransfer` uses `GDMA_FINAL_LINK_TO_NULL` (stops cleanly, one shot). A ring uses `GDMA_FINAL_LINK_TO_HEAD` (never terminates) — so *something* must stop it at frame end. **Decision for this build: terminate by stopping the peripheral in the EOF ISR on the last slice** (`gdma_stop` + `lcd_ll_stop`), NOT by writing a NULL terminator into a live node (the GDMA prefetches descriptors ahead of the data, so a NULL we write can be read too late and the DMA wraps into a stale buffer). This is a deliberate, documented choice with the prefetcher named as the reason at the introduction site — the reflex-standard `gdma_link_concat(NULL)` is rejected there with that reason.

## Design (settled)

**Ring geometry.** `rowBytes` = one row (one light across all strands) = `outCh × 24 × slotBytes × outputsPerPin()`. Ring buffer = `kRingRows` rows; **N = kRingBufs** buffers, all INTERNAL. Start N=**4** (not the theoretical minimum of 2 — a 2-deep ring makes the refill task win a one-buffer race every wrap, and the GDMA prefetcher can re-read a buffer before the task refills it; N=4 gives the task 3 buffers of runway). `kRingRows=16` → 4×(16×576 B) ≈ 37 KB internal at the 8-bit-bus 16-strand size — under the ~42 KB block, and independent of strand length (the whole point).

**Timing budget (why a task suffices):** DMA drains one 16-row buffer in ~345 µs; CPU encodes 16 rows in ~96 µs → 3.6× margin. A WiFi task preempting the refill task is absorbed by that margin; an actual underrun shows as visible glitching, which is measurable → only *then* escalate to ISR.

**Third tick path.** Add `tickRing(outCh)` beside `tickSync`/`tickAsync`. `tick()` selects it when `moonI80Ws2812IsRing(bus_)`. `tickSync`/`tickAsync` stay **byte-for-byte unchanged** (proven paths). `tickRing` calls `moonI80Ws2812TransmitRing` then waits on slot 0 (the ring reports completion there).

**Encode trampoline.** `MoonI80EncodeFn` is a plain function pointer; there's no CRTP hook for it. `MoonI80LedDriver` adds a `static` trampoline that casts `user`→`this` and calls `encodeRows<Slot>(outCh, dst, firstRow, rowCount, closeFrame)`, branching on `slotBytes()`.

**Ring vs whole-frame selection.** Ring **only when** shift mode AND the frame doesn't fit a contiguous internal block. Otherwise keep the proven whole-frame path (direct mode streams PSRAM fine). Fall back to whole-frame if `InitRing` fails.

**Prefill for recycled buffers.** Ring buffers are RECYCLED, not zeroed per frame — so the pulse-start/tail constants and the latch pad must be written explicitly per buffer. `prefillShiftFrame` already lays out per-run masks; the refill must honor `firstRow` so a slice spanning a strand-end boundary re-lays the constants correctly. This is the invariant most likely to break → a host test pins "a recycled buffer produces the same bytes as a fresh one."

## Implementation steps

1. **Save this plan** to `docs/history/plans/` (process rule: first implementation step).

2. **Platform ring backend** (`platform_esp32_moon_i80.cpp`) — extend `MoonI80State` (ring buffers `ring[kRingBufs]`, `isRing`, `encode`/`user`, `totalRows`/`rowsPerBuf`/`nextRow`, `refillReady` counting semaphore, `refillSlot`, `frameDone`, the refill task handle). `destroyState` frees the new resources + semaphore + task. Add `moonI80Ws2812InitRing` (N internal buffers via `allocFrame(psram=false)`; `gdma_link` sized for all N; mount all with `mark_eof=true`, last with `GDMA_FINAL_LINK_TO_HEAD`), `moonI80Ws2812TransmitRing` (prime all buffers via `encode`, one `gdma_start`+`lcd_ll_start`), `moonI80Ws2812IsRing`. Extend `moonI80EofCb` with a ring branch: on a non-final slice `xSemaphoreGiveFromISR(refillReady)`; on the last slice `gdma_stop`+`lcd_ll_stop`+give `done[0]`. Add the pinned refill task. Rewrite the platform.h contract comment (ISR→task).

3. **Desktop stub** (`platform_desktop.cpp`) — inert `InitRing`→false, `TransmitRing`→false, `IsRing`→false (host has no GDMA; ring is bench-verified, exactly like the whole-frame path).

4. **Domain** (`ParallelLedDriver.h` + `MoonI80LedDriver.h`) — add `tickRing`; `tick()` selects it via `IsRing`. Add the static encode trampoline + `busInitRing()` forward in `MoonI80LedDriver`. `reinit()` chooses ring vs whole-frame by the selection rule.

5. **Loopback through the ring** (the instrument-first step, PO priority) — route `moonI80Ws2812Loopback`'s transmit through the ring when shift mode + oversize, so the bit-verifier validates ring output at 256. This also closes the backlogged "shift-mode loopback stalls on the PSRAM whole-frame path" root cause.

## Verification

**Host (`ctest`):** (a) `tickRing` drives a mock frame end-to-end; (b) the last slice closes the frame, others don't; (c) **a recycled buffer == a fresh one** (the prefill invariant). The slice-invariant (sliced == whole-frame) is already pinned by an existing test.

**Hardware — board B (192.168.1.150, shiffy) — THIS is the acceptance test:**
1. 96/strand still renders (no regression).
2. **Loopback through the ring PASSES at 128 then 256** — per-bit truth, the instrument the first attempt lacked. Watch for the known-benign '595 first-latch bit-0 artifact and exclude it.
3. 128 then 256/strand render on real panels; `wireUs` sane; **zero GDMA errors**; no glitching (glitch ⇒ task underran ⇒ escalate to ISR refill).
4. fps vs whole-frame at 96 (UI module swap, no reflash) — measure toward the 100 fps driver-fps goal.

**PO's eyes are the measurement — stop and hand over at "it's running on board B"; do not self-certify.** Build only 3 ESP32 variants (one classic, one S3, one P4).

## Scope guards

NOT touching `I80LedDriver` or Parlio. NOT changing `tickSync`/`tickAsync`. NOT the ISR refill (task first; escalate only on measured underrun). The ring/render-split core-1 contention (the stash's fps regression) is settled by design: **a MoonI80 ring bus does not allocate buffer 1**, so `tickRing` is the only async path when the ring is active — there is no second async mechanism to fight over core 1.
