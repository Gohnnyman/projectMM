# Plan — MoonI80 lapping-v2: clock-oracle ring (48×256 endgame)

## Context

Prime-only streaming is DONE and wall-verified: ≤224 lights/strand (`ceil(lights/rows) ≤ bufs`, rows
capped at 7 by the one-node rule, bufs at 32) renders pixel-perfect at ~80 fps. 256+/strand REQUIRES
lapping (37+ slices over a ≤32 pool; priming a whole 256-frame needs ~150 KB internal that doesn't
exist), and lapping on the current mechanics is "almost good": the image holds, but a shifted/delayed
region with wrong colors appears intermittently (purple → orange = a one-byte GRB shift).

Diagnosis, measured:

1. **Missed refills cause the shifted region.** The GDMA EOF interrupt is a latch, not a queue: two
   buffer-EOFs coalesce into one interrupt under load, the ISR refills once, and the self-advancing
   `refillSlot`/`refilledRow` cursor (moon_i80.cpp:361-399) permanently trails — every later slice lands
   one position shifted until frame end. Proven on the bench (EOF undercount under API polling).
2. **The worst-case encode overshoots the per-slice deadline; the average roughly fits.** True deadline =
   `rowsPerBuf × 21.6 µs` (8-bit bus; 108 µs at rows=5, 151 µs at the rows=7 cap). Measured worst refill
   at the target shape (48 strands, all 12288 lights): `enc=350 µs` at rows=5 — a ~3× worst-case tail
   over a deadline the *average* refill roughly meets (the wall renders mostly correct). Data-side levers
   are exhausted and measured (240 MHz, IRAM chain, snapshot/correction hoists, empty-lane uniformity).
3. **The current lapping frame-end races its own instrument**: `gdma_stop` fires when `drainCount ≥
   nSlices+1` (moon_i80.cpp:414-420), and drainCount undercounts under coalescing — the stop lands late,
   which also inflated the measured frame time (13.6 ms at the target shape vs the true ~5.7 ms wire).

**The wire physics, now pinned from code:** 48 strands on 6 data pins is an **8-bit bus**
(`busWidthPins()` = 6 data + 1 latch = 7 ≤ 8, ParallelLedDriver.h:1136-1140), so a 256-light frame costs
256 × 21.6 µs ≈ 5.53 ms + 350 µs reset ≈ **~170 fps wire ceiling — the 100 fps goal is wire-feasible**,
gated only on the streaming mechanics + encode keeping up.

**Model:** hpwit's I2SClocklessVirtualLedDriver (reviewed with him): small fixed pool, refill trailing
the read head, zero-pad deadline extension, self-terminating chain, no mid-frame stop, IRAM ISR — studied,
then written fresh against our architecture.

## Design — four mechanisms

### 1. Clock-oracle batch refill (the correctness fix)
The looping DMA free-runs at crystal-exact wire speed, so **elapsed time IS the drain position**:
`drainPos = (now − armUs) / sliceUs`, integer µs math, with `sliceUs = rowsPerBuf·rowBytes / 26.67 MHz
(+ padUs when enabled)`. The EOF ISR (moonI80EofCb ring branch) stops trusting its interrupt count:
each firing computes `drainPos` and refills **every** unwritten slice up to
`drainPos + ringBufs − kLead` (`kLead = 2`), **capped at 4 slices per firing** (bounds ISR duration;
EOFs keep arriving every slice, so capped batches still converge). Effects:
- A coalesced interrupt changes *when* the batch runs, never *what* gets written — the shifted-region
  artifact is structurally dead.
- **The pool becomes a jitter buffer**: the writer may fall behind by up to `(ringBufs − kLead) ×
  sliceUs` (e.g. 16 bufs × 108 µs ≈ 1.5 ms) during a worst-case spike and catch up in the next batches.
  The requirement drops from "worst-case enc < deadline" (unmeetable, 3×) to "**average** enc <
  sliceUs" — which the wall's mostly-correct rendering says is already near-true; the `late` counter
  (below) measures it exactly.
- `drainCount` stays only as a diagnostic; `refillSlot` is derived as `sliceIndex % ringBufs` (the
  mount order fixes buffer↔slice congruence, unchanged).

### 2. Frame end: clock-keyed stop over the zeroed tail (subtraction over splice)
Keep the looping chain and the existing past-frame zero-fill (moon_i80.cpp:384-397): once the batch
writes past slice `nSlices`, recycled buffers are already memset-zero. The ISR then stops the engine
(`lcd_ll_stop` + `gdma_stop`) when **the oracle** says `drainPos ≥ nSlices + kTailBufs` — not when an
interrupt count does. A late stop is now *harmless by construction*: the DMA is circling zeroed
buffers, and extra zeros on the wire ARE the WS2812 reset; lateness only nudges the next arm (bounded
by ISR latency, µs with mechanism 4). `lastTransmitUs` is stamped from the oracle (`nSlices × sliceUs`)
so frameTime reports the true wire time, un-inflated.
*Rejected alternative, documented in-code:* hpwit's terminator splice at last-slice-written
(`gdma_link_concat` + restore-at-arm). It ends the frame exactly but reintroduces the runtime-concat
machinery this file already rejected once (moon_i80.cpp:1062) — the zeroed-tail stop achieves the same
wire behavior with code that already exists. If the bench shows stop artifacts, the splice is the
fallback, keyed by `bufLastNode[]` (the fragility that burned the first attempt is fixed).

### 3. `ringPadUs` — interleaved SHARED zero-pad (deadline trim, control-gated)
Chain becomes `buf → pad → buf → pad → …`: after each buffer's node, one extra node mounts the SAME
shared zero block (`padUs` of bus bytes at 26.67 MHz; 120 µs ≈ 3.2 KB, allocated once).
`gdma_link_mount_buffers` already supports arbitrary node offsets and the mount's own `endIdx` is
ground truth (moon_i80.cpp:250-255, 1101-1105) — the pad nodes mount in the same loop, `mark_eof`
stays on the DATA nodes. A <150 µs LOW gap reads as a pause, not a latch (hpwit's `_DMA_EXTENSTION`;
~300 µs measured to latch), so the per-slice deadline grows by `padUs` at a linear fps cost
(frame += nSlices·padUs; 120 µs × 52 ≈ +6.2 ms — halves fps, which is why it's a **control**, not a
constant: `ringPadUs` 0-120, default 0, next to ringRows/ringBufs in `addRingControls()`
(MoonLedDriver.h:190-209)). The oracle's `sliceUs` includes it. Descriptor pool grows to
`ringBufs × (itemsPerBuf + 1)` when padded.

### 4. IRAM interrupt + instruments
- `gdma_channel_alloc_config_t` currently sets no interrupt priority and no IRAM flag
  (moon_i80.cpp:936). Set `intr_priority = 3` (hpwit's level) and register the ISR IRAM-safe — the
  encode chain is already IRAM (MM_RAMFUNC, shipped), so the cache-safe registration is now legal.
  Removes ISR-dispatch latency and flash-write stalls from the deadline race.
- **`late` counter** in RingStats + ringDbg: slices the oracle refilled *after* their drain position had
  passed (stale on the wire) — the machine's scatter meter; the wall's "almost good" becomes a number,
  and soak acceptance is `late == 0`.
- **Regime visibility**: the driver's status line (DriverBase.h:422 "driving X of Y lights") gains the
  regime word — `(primed)` / `(lapping)` — from `nSlices ≤ ringBufs`; ringDbg's `tn` field already
  discriminates but the PO shouldn't need ringDbg to know which side of the boundary a config is on.

## Code grounding (what changes where)

- `src/platform/esp32/platform_esp32_moon_i80.cpp` — the whole feature lives here:
  - `MoonI80State`: + `armUs`, `sliceUs`, `padUs`, `zeroPad*` (shared block ptr/len), `lastWrittenSlice`,
    `dbgLate`; `refilledRow/refillSlot` become derived-from-slice-index.
  - `moonI80EofCb` ring branch (331-429): the oracle batch replaces the single-refill body; clock-keyed
    stop replaces the drainCount test; prime-only branch unchanged (terminator EOF, no stop).
  - `encodeRingSlice` (846-861): unchanged seam; called per batched slice.
  - `createRingState`/`initRingDma` (935-1116): pad-node mounting in the mount loop (1080-1106), pool
    sizing + shared zero block alloc, `sliceUs` derivation, `intr_priority`/IRAM channel config.
  - `startRingTransfer` (868-931): stamp `armUs`; prime loop and reset busy-wait unchanged.
  - `moonI80Ws2812InitRing` (1152-1190): `padUs` parameter threaded; heap pre-check includes the pad
    block.
- `src/platform/platform.h`: `moonI80Ws2812InitRing` signature + `MoonI80RingStats.late`; kRingPad
  bounds constant next to kRingRowsDefault/kRingBufsDefault (803-804).
- `src/light/drivers/MoonLedDriver.h`: `ringPadUs` control in `addRingControls()` (190-209), threaded
  through `busInitRing` (292-297); ringDbg gains `lt%u` (refreshBusKpi, 221-227).
- `src/light/drivers/ParallelLedDriver.h`: regime word where the status is set / `tick1s` frameTime
  block (552-557); `busInitRing` call site (1336) passes the pad control.
- `src/light/drivers/DriverBase.h`: status format gains the regime suffix (422-425).
- `test/unit/light/unit_ParallelLedDriver_ring.cpp`: the mock (driveRingFrame/WithTermination) gains
  **coalesced-EOF delivery** (2 drains, 1 callback) with byte-identity through the oracle batch — the
  regression test the old design couldn't pass; padded-chain tiling byte-identity (pad bytes stay 0);
  clock-keyed stop over the zeroed tail across 2 frames; constant-RAM assert (pool size independent of
  nSlices).

## Phases + acceptance (bench: shiffy, /dev/cu.usbmodem2021401, 192.168.1.150)

- **A. Oracle + batch + clock-keyed stop, pad=0** — 2-pin bench, 256/strand (rows=7/bufs=16):
  shifted-region artifact gone (PO's eyes), `late` counter quantifies the residual tail; frameTime
  deflates to the true ~5.9 ms (≈170 fps max) proving the stop no longer lags.
- **B. Pad sweep** — `ringPadUs` 0→60→120 on the bench; accept the smallest pad with `late = 0` over a
  multi-minute soak under API polling. If `late > 0` even at 120: the compile-time lane-count unroll is
  the named next lever (backlogged, not this plan).
- **C. Target shape** — 6 pins × 8 × 256, all 12288 lights (Panels 16×3): clean wall (PO), `late = 0`,
  measured fps vs the 170 ceiling — **the 100 fps answer lands here**.
- **D. Instruments + docs** — intrusive loopback bit-verify riding the ring at 256; KPI + performance.md
  at merge; regime word visible; backlog ring entry updated to the shipped state.
- Gates: ctest + scenarios green throughout; ESP32 3-variant build; the commit rides as one combined
  commit when the PO says so.

## Out of scope (named, backlogged)
- Compile-time lane-count unroll (reserve encode lever; only if B fails at max pad).
- PLL240M / 19.2 MHz clock (fps-costing fallback, superseded unless C misses badly).
- Multi-strand loopback; spacer layouts; the ringDbg diagnostic removal (stays until 256 soaks clean).

## Outcome (same day, bench-verified)

**Phases A+B: ACHIEVED, wall-verified by the PO.** The clock-oracle batch refill + clock-keyed stop
stream 256 lights/strand pixel-perfect on the 2-pin bench (16 strands; `lt` frozen at 0 over thousands
of frames, de0, 66 fps actual against the 149 fps wire ceiling at pad=30) — the first clean 256/strand
in the project's history. The shifted-region artifact is structurally dead; frameTime deflated to the
true wire time (6.7 ms vs the old 13.6 ms stop-lag inflation).

Two findings the plan didn't predict, both resolved:
- **The cache-safe ISR paniced (Cache error) during persistence saves**: the handler code is all IRAM,
  but the DATA it reads (the driver module object, PSRAM-mapped) sits behind the same cache a flash
  write disables. Fix: the standard defer guard (`spi_flash_cache_enabled()` → return; the oracle batch
  catches up next EOF). Proven by a 12-consecutive-save stress with zero crashes.
- **The wall's panels latch at ≤60 µs LOW, not hpwit's 150 µs** — pad=60 made every slice repaint LEDs
  0..6 (latch resets the strand's address). pad=30 is clean. The latch threshold is per-strip silicon;
  kRingPadMaxUs stays 120 for tolerant strips, the control is the hardware knob.

**Phase C: the 48-strand encode does not fit — measured, not guessed.** At 6 pins × 8 × 256 (all 12288
lights): worst refill 466 µs vs the 181 µs padded deadline, `late` climbing ~120/s (~17% of slices) —
a SUSTAINED capacity deficit the pool cannot absorb. Per this plan's own branch: the compile-time
lane-count unroll is the named next lever (its own plan), with the 19.2 MHz clock (+78 µs/slice budget,
~110 fps ceiling) as the second stage. The 100 fps goal remains feasible on measured numbers.

Residual: a ~1-frame white flash every ~5 s at 256/strand (random LEDs, dense effects show it as a
hickup) — not a late slice (`lt`=0 throughout); the intrusive-loopback soak is the named instrument,
queued post-baseline.
