# Plan — Lean rows=1 ring ISR: shed per-firing overhead, test if 48 strands fit hpwit-style

## Context

48 strands × 256 does not stream clean on the shipped ring. Today's investigation eliminated the false
leads (encode unroll — compiler already optimal; slower clock — slows encode equally; large pad —
latches the strand). Two facts then reframed it:

1. **hpwit's driver IS a small streaming ring** (`DMABuffersTampon`, `__NB_DMA_BUFFER=10`), one LED per
   buffer, refilled in the GDMA ISR — the same architecture as ours. So "the ring is wrong for 48" was
   FALSE. His ring works; the question is per-firing efficiency, not architecture.
2. **Our rows=1 test** (the hpwit-equivalent granularity — 1 LED/buffer) collapsed `enc` from 468 µs
   (rows=7) to **63 µs**, but `lt` exploded to ~8000/s. At rows=1 the interrupt fires ~25,600×/s, and our
   ISR does per-firing work hpwit's doesn't: `spi_flash_cache_enabled()`, two `esp_timer_get_time()`
   calls, the oracle division `(eofNow−armUs)·1000/sliceNs`, and batch-loop bookkeeping. That overhead,
   × 25,600/s, is the suspect for the `lt` blowup.

**hpwit runs 48 strands on this library (PO-confirmed)** — a demonstrated result, so his lean structure
IS proven at 48. The exact per-firing diff is now mapped (explorer): at rows=1 our ISR pays, EVERY firing,
what his pays NONE of — `spi_flash_cache_enabled()`, an `esp_timer_get_time()`, a 64-bit divide
`(eofNow−armUs)·1000/sliceNs`, plus `dbg*` instrumentation and (per slice) two MORE timer reads. His ISR
is: `ledToDisplay++` → `loadAndTranspose` (1 LED) → counter-compare terminator splice → `dmaBufferActive =
(…+1) % NB` → done. `encodeRingSlice` at rows=1 is ALREADY ~his `loadAndTranspose` (one LED-row), so the
entire gap is the per-firing prologue. That is the sheddable overhead.

**Drop the oracle ENTIRELY in the lean path (PO decision) — no reconcile, pure hpwit counter.** The
oracle's only value is coalescing-tolerance at SHALLOW pool depth: at rows=7 a coalesced firing loses 7
rows of position and a shallow pool can't absorb it. At rows=1 that value evaporates — a coalesced firing
loses 1 LED, and a deep-in-LEDs pool (e.g. 24 buffers = 24 LEDs of lead ≈ 14 KB, trivial) swallows it,
the next firing catching up exactly as hpwit's depth-10 pool does. So the lean path is his literal model:
counter-advance, `slot = counter % ringBufs`, refill-until-caught-up, NO timer, NO division, NO periodic
reconcile. The coalescing safety is POOL DEPTH, not the clock. This is not a risk we're accepting — at
1 LED/buffer the granularity IS the safety.

**The honest limit (why Phase 0 measures, and why we build anyway):** even oracle-free, shedding the
~20 µs of per-firing overhead takes rows=1 from 63 → ~40 µs/firing — STILL above the 21.6 µs per-LED wire
budget for 48 strands (the pure 48-strand encode is the floor). So the lean path is GUARANTEED to fix
`lt` for the ~16-24-strand configs (pure encode < budget) and makes rows=1 generally usable, but may NOT
alone reach `lt=0` at the full 48 — the 2.7× producer/consumer wall, met one level down. That residual is
the multicore pipeline's job. Build the lean path regardless (real progress, PO); Phase 0 quantifies the
residual honestly.

**Gate is measure-but-build (PO):** Phase 0 measures the pure 1-LED 48-strand encode honestly. If it's
under ~21.6 µs the lean path fits 48; if over, the lean path still SHIPS (it helps every rows=1 config and
is one step toward the goal) and the residual points at the multicore pipeline. Either way the lean path
is built — the measurement sets expectations, it does not gate the work.

## Design — a lean rows==1 ISR branch, gated on measurement

### Phase 0 — decompose the 63 µs (measure, set expectations; do NOT gate)
`dbgMaxEncodeUs` already brackets `encodeRingSlice` alone (explorer confirmed: the two per-slice
`esp_timer_get_time` reads at :421/:423 wrap the encode only). So the bench already reports pure 1-LED
encode µs — read it at 48 strands/rows=1 (the rows=7 run showed enc≈468/7≈67/LED incl. overhead; rows=1
showed 63 TOTAL, so the pure encode is well under that). Interpretation only:
- **Pure encode < ~21.6 µs**: the lean path should fit 48 strands — high value.
- **Pure encode > ~21.6 µs**: the lean path still SHIPS (helps all rows=1 configs, one step closer), and
  the residual `lt` quantifies exactly how far the multicore pipeline must still carry. No stop.

### Phase 1 — the lean rows==1 fast branch (only if Phase 0 passes)
At `rowsPerBuf == 1` the clock oracle is unnecessary: with 1 LED/buffer the pool is deep in LED-units and
a naive "one refill per firing, advance a counter" — hpwit's exact model — is correct, because a coalesced
interrupt at this granularity just means the next firing refills two, which a tiny counter handles without
the division. So add a branch in `moonI80EofCb` (there is precedent — the `primeOnly` branch already
regime-splits this ISR):

- **Skip the oracle**: no `esp_timer_get_time`, no division. Advance `lastWrittenSlice`/refill-cursor by a
  plain counter, refill the just-drained buffer (index from a running `% ringBufs`), like hpwit's
  `dmaBufferActive`.
- **Skip the cache-check per firing IF safe**: `spi_flash_cache_enabled()` guards a real panic
  (config-save during render). Investigate whether it can move to once-per-frame or be replaced by a
  cheaper flag — do NOT drop it blindly (it fixed a shipped crash). If it must stay, keep it; it's one
  branch, cheaper than the timer+division.
- **Terminator splice a pool-depth ahead, counter-keyed** (hpwit line ~2253): when the last real LED is
  written, splice the NULL terminator `__NB_DMA_BUFFER` ahead, self-terminate — no `gdma_stop`, no clock.
  We already have `bufLastNode[]`/the mount machinery for this.
- **Coalescing safety IS pool depth, no clock** (hpwit's model, PO-confirmed): a missed firing leaves the
  counter 1 LED behind; the refill-until-caught-up loop (counter-keyed, capped) catches up next firing,
  and the pool's LED-depth lead is the margin — exactly his depth-10 scheme. NO periodic reconcile, NO
  timer. Sized: ringBufs deep enough that pool-lead > worst coalescing burst (the host mock's coalesced-EOF
  case pins byte-identity, proving the counter scheme is safe at the tested depth).

### Phase 2 — keep the oracle path for rows>1 (unchanged)
rows>1 (the shipped 16-strand clean config, and lapping generally) keeps the clock-oracle path exactly as
committed — it is correct and wall-verified there. The lean branch is purely additive, gated on
`rowsPerBuf==1`. No regression risk to the shipped config.

## Code grounding (explorer-confirmed line numbers)
- `src/platform/esp32/platform_esp32_moon_i80.cpp` — `moonI80EofCb` ring branch (350-484). The ISR ALREADY
  regime-splits: `if (!primeOnly && st->busy)` = the heavy oracle path (383+), `else if (primeOnly &&
  st->busy)` = a lean division-free semaphore-only path (459-468) — the in-repo TEMPLATE for the new branch.
  Add a lean path taken when `rowsPerBuf == 1` (a new regime; a rows=1 many-LED frame has nSlices > ringBufs
  so today it wrongly falls into the heavy path). The lean body: shed items 1-8 of the per-firing prologue
  (cache-check → move to periodic or keep as ONE branch; drop the eofNow read + the divide + all dbg reads
  from the per-firing path), advance a plain counter + `slot = counter % ringBufs`, call `encodeRingSlice`
  (unchanged — already 1 LED at rows=1), splice the terminator a pool-depth ahead counter-keyed (reuse
  `bufLastNode[]` + the self-terminating NULL from `createRingState`), periodic clock reconcile for
  coalescing safety. `spi_flash_cache_enabled()` (:373) stays SOMEWHERE (it fixed a shipped panic) but at
  once-per-frame or as the single cheapest branch — investigate, don't drop blindly.
- Host mock `test/unit/light/unit_ParallelLedDriver_ring.cpp` — `driveRingFrameCoalesced` already pins the
  coalesced-EOF contract; extend it to the counter-keyed lean branch (byte-identical whatever the grouping),
  proving the periodic-reconcile safety holds without the per-firing oracle.

## Verification
1. **Phase 0 gate**: pure 1-LED 48-strand encode µs on the bench. Decides go/no-go HONESTLY.
2. Host ctest: lean branch byte-identical to the oracle branch at rows=1 (same coalesced-EOF pin).
3. S3 build + flash; bench: 48 strands, rows=1, lean path — does `lt` drop to 0? Does the wall render
   clean on the 2 wired pins (and ideally wire more pins to judge all 48)?
4. Confirm the shipped rows=7 16-strand path is UNCHANGED (still `lt=0`, same fps) — the lean branch must
   not touch it.
5. If Phase 0 fails or the lean path still shows `lt>0`: the honest conclusion is the multicore
   whole-frame pipeline (its own plan) — report the residual, don't force it.

## Out of scope
- Multicore whole-frame pipeline (the fallback if the lean ring can't fit 48 — separate, bigger plan).
- The ~5 s white-flash residual (its own hunt).
- Encode-speed work (measured dead — [[encode-unroll-does-not-help]]).
