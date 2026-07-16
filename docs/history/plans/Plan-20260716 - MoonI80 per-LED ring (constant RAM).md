# Plan — MoonI80 ring: measure honestly, then go per-LED (constant RAM at any strand length)

## Context

**Where we got to today.** Two real bugs were found and fixed, and the ring rendered correctly for the first time in the project's history:

1. **The phantom pad** — ring buffers were sized `rows + padBytes` (16,128 B) while the DMA nodes are mounted **rows-only**, so the pad was written, cache-synced, and *never clocked*: 43% of the ring's RAM. Worse, it made `kRingBufs=16` need **252 KB against ~160 KB free**, so `moonI80Ws2812InitRing` returned false and the driver **silently fell back to whole-frame — even with `forceRing=1`**. Every "clean at 128/192/196" result in the entire investigation was that fallback; **the ring had never actually run**. (CodeRabbit flagged this pad; it was declined. It was right.)
2. **The missing closing latch** — introduced while removing the pad (`last` was hard-coded to `false`). The '595 is a one-slot delay line, so the frame's final value was never latched and **persisted into the next frame**: everything shifted by one, with a pulse-start **constant** stuck in position 0 — which is exactly why it survived `brightness=0` (constants bypass the correction LUT). **192 now renders perfectly on the ring** (PO-confirmed, 214 fps).

**The wall that remains.** 256 lights/strand needs `nSlices + 1 = 17` buffers × 9,216 B = **153 KB**, against ~158 KB free — leaving ~5 KB for a WiFi/HTTP reserve that must be ~32–40 KB. The allocation is refused. **Bigger `kRingRows` does not help** (identical total bytes). So 256 cannot be reached by avoiding buffer reuse, and reuse has never worked.

**The way out (the PO's insight).** hpwit drives 48×256 at ~100 fps on this same silicon with **one LED per DMA buffer**: `WS2812_DMA_DESCRIPTOR_BUFFER_MAX_SIZE (576*2)`, `__NB_DMA_BUFFER = 10`, allocating 12 descriptors — **13.5 KB total, constant at any strip length**. His ISR fires per LED and does a full transpose inside it. At 1 LED/buffer our RAM stops scaling with strand length entirely: 256, 512, 1024 all cost the same ~19 KB. It also dissolves today's structural bugs — the `nSlices+1` buffer problem vanishes and the tail/latch always has somewhere to live.

**The blocker is not RAM, and it is not our algorithm.** Two corrections that reshape this plan:

- **Our encoder is not behind hpwit's.** It already does one `transposeLanes8x8/16x8` **per channel per row** (`ParallelSlots.h`), structurally identical to his three transposes per LED. Same shape, same count.
- **`CONFIG_COMPILER_OPTIMIZATION_DEBUG=y` (`-Og`) + `ASSERTIONS_ENABLE=y`, and we never set it** — it is nowhere in `esp32/sdkconfig.defaults*`; we inherited IDF's debug default. hpwit builds `-O2` with an all-IRAM handler. **Our measured ~136 µs/LED encode is an `-Og` number**, and so is every ESP32 KPI in `docs/performance.md`.

**One correction to the research, verified here:** the advice to "drop to hpwit's 19.2 MHz for a 39 % margin gain" **does not transfer**. He runs a **PLL240M** tree; we select `LCD_CLK_SRC_DEFAULT` → PLL160M → **80 MHz resolution with an integer prescale** (`initPeripheral`). On our tree: prescale 3 → 26.67 MHz → **300 ns slot (in spec)**; prescale 4 → 20 MHz → **400 ns slot — past the ~380 ns T0H max**, where a "0" reads as a "1". **There is no slower legal option for us**, so 26.67 MHz is not an accidental overclock and the **21.6 µs per-LED deadline is fixed**. (Moving to PLL240M to unlock 19.2 MHz is a separate, deliberate decision — not part of this plan.)

**The goal:** 256 *and* 512 lights/strand, at constant RAM, with the ISR meeting a real deadline.

## The sequence (each step is measured before the next is designed)

**Step 1 — `-O2` for the ESP32 build, then RE-MEASURE.** Set `CONFIG_COMPILER_OPTIMIZATION_PERF=y` in `esp32/sdkconfig.defaults`. This is the single biggest lever and it is currently an accident, not a decision. **Do not design the ring geometry until the encode is measured under `-O2`** — the current 136 µs/LED and the 22 µs projection both bracket an unknown. Ship as its own commit with a before/after so the win is attributable; expect every KPI in `docs/performance.md` to move (that is a doc update, not a regression).

**Step 2 — IRAM the ring's ISR encode chain.** `moonI80EofCb` tail-calls a **flash-resident** encode (documented at the handler). At an 11–47 kHz interrupt rate a flash cache miss inside the ISR is a wedge, and it is *already* a backlogged correctness bug (the OTA/flash-cache fault: an OTA runs on its own task while rendering, so the window genuinely overlaps). `IRAM_ATTR` the chain the ISR reaches — `encodeRingSlice` → the trampoline → `encodeRows`/`prefillShiftRows` → the `ParallelSlots` templates → `Correction::apply` — and set `isr_cache_safe`. This fixes a real fault *and* buys ISR headroom. The S3/P4 have unified DIRAM with room; the ring is `SOC_LCDCAM_I80_LCD_SUPPORTED`-gated so no IRAM cost lands on the IRAM-tight classic.

**Step 3 — re-measure, then choose the geometry.** With a trustworthy µs/LED number, set `(bufferLEDs, kRingBufs, EOF-every-N)`. Target: **1 LED/buffer, 32 buffers, EOF every 4** → **(32+2) × 576 B = 19.1 KB**, flat at any strand length, **11.6 kHz** interrupts (4× calmer than hpwit's shipping 46 kHz), required encode **≤ 21.6 µs/LED**. Batched EOF is supported — `gdma_link.h` exposes `flags.mark_eof` per *mount* and `gdma_link.c:240` sets `suc_eof` only on a mount's last node — and it decouples RAM granularity from interrupt rate. Note it only amortises the ~2 µs ISR entry (~9 % gain), so it is an interrupt-rate tool, **not** encode headroom.

**Step 4 — only if the encode still misses 21.6 µs:** move the encode out of the ISR to a high-priority task pinned to the non-render core (the ISR then does a counter + notify). This is the biggest structural lever and the honest plan-B; hold it in reserve rather than doing it speculatively.

**Files:** `esp32/sdkconfig.defaults` (step 1); `src/platform/esp32/platform_esp32_moon_i80.cpp` (`kRingRows:132`, `kRingBufs:165`, `moonI80EofCb`, `createRingState`, `startRingTransfer`), `src/light/drivers/MoonLedDriver.h` (`ringEncodeTrampoline`), `src/light/drivers/ParallelSlots.h` + `Correction.h` (IRAM attrs). **The encode seam already generalises to per-LED for free** — `MoonI80EncodeFn(dst, firstRow, rowCount, last)` just takes `rowCount = 1`; this is a parameter change, not a rewrite.

## Verification

- **Instruments are fixed and trustworthy (use them):** `ringDbg` publishes ONE completed frame — `enc<mean>/<max>us(n<count>) gap<mean>/<min>us(n<count>)`, with `enc:none` when the ISR encode never ran; the `build` control carries a **git build id** so a flash can be proven to have landed. Both were fixed today after each produced a false conclusion. **Print the counts — comparing means with mismatched denominators is what manufactured a phantom "2.3× too slow".**
- **Host:** the 26 ring/slots tests stay green (they pin the encode bytes: sliced == whole-frame, recycled == fresh, ragged strands). Add a test for the per-LED geometry (`rowCount=1` slices tile identically to the whole frame).
- **Bench, in order:** re-measure `enc` after step 1, then after step 2 — each on its own, so each win is attributable. Then 128 → 192 → 196 → **256** → **512** on the ring, confirming `ringDbg` reports a real `sl*/bf*` (not `not ring` — that is the silent-fallback tell).
- **The gate is the PO's eyes on the strip.** Report what the instrument says and hand it over; do not self-certify.

## Risks

- **The `-O2` win is unmeasured.** If it does not close the 6.3× gap, step 4 (encode out of the ISR) becomes the real fix, not a reserve.
- **`-O2` is global** — it changes every module's codegen and every KPI. Its own commit, its own gate run.
- **47 kHz interrupts on the render core** could starve WiFi/HTTP — we have already measured a ~19 ms core-0 encode starving the W5500 ethernet on the LC16. Batched EOF (every 4) is the mitigation; watch HTTP responsiveness on the bench, not just fps.
- **`kRingBufs=8` + reuse remains unproven.** Today's fixes make 192 work *without* reuse; per-LED makes reuse routine rather than exceptional, which is the real test of the ISR refill.
