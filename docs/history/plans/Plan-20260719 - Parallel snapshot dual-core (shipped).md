# Plan — Parallel snapshot: split the 18 ms correction loop across both cores (smallest fps step)

## Context

48×256 (12,288 LEDs) streams CLEAN and self-tunes (committed 5ab8d819), but fps is ~15-30 against a
~146 fps wire ceiling. The multicore split already moves the output stage to core 1 — verified — yet the
frame is `max(effect, output)` bound because the split is LOPSIDED. Measured on the wall this session
(tw/ts/tp diagnostics in ringDbg):

- **effect (core 0): ~8 ms**, then **core 0 IDLES ~26 ms** (renderWait) while
- **core 1: snapshot 18 ms (`ts`) + prime 14 ms (`tp`) + wire-wait 0.04 ms (`tw`) ≈ 32 ms.**

The **snapshot (`ts`) is the single biggest segment — 18 ms**, bigger than the 14 ms prime. It is
`snapshotSourceForRing()`'s per-light color-correction loop over 12,288 lights
(ParallelLedDriver.h:1058, `for i < winLights: correction_.apply(...)`) — **embarrassingly parallel**, no
cross-light state, writing into an ALREADY-allocated internal buffer (~36 KB, no new RAM). This plan does
the smallest, lowest-risk fps step: **run half the snapshot on core 0 (idle after its effect) and half on
core 1, in parallel.** No ISR change, no DMA-arm change, no encode change — just the copy loop.

**Predicted:** snapshot 18 → ~9-10 ms; core-1 output ~32 → ~23 ms; frame `max(8, 23)` → fps ~30 → ~40 on
a cheap effect (less on heavier effects where core 0 has less spare; unchanged on render-bound effects —
correct). The fork-join of the 14 ms PRIME is a separate follow-up (bigger risk surface); this step banks
the largest single segment first and proves the parallel-for machinery on the safest loop.

## Design — a 2-worker parallel-for over the snapshot's light range

The correction loop splits into two halves by light index. The existing core-1 worker
(`Drivers::encodeTask_`, a `platform::WorkerTask` woken via `notifyTask`/`waitNotify`, already spawned and
parked) does the TOP half; core 0 does the BOTTOM half inline, right after its effect render, then both
join before the frame proceeds. Because the snapshot is the FIRST thing the output stage does — and it
gates core 0's own next render (render N+1 can't overwrite the live buffer until the snapshot copies it
out) — putting core 0 on it is the researcher's key insight: core 0 helps with the exact work it would
otherwise idle-wait for.

**CORRECTION from the primitive map (load-bearing):** the snapshot runs inside `tickRing()` (ParallelLedDriver.h:544), which UNDER THE SPLIT already executes on **core 1** (the `mmEncode` worker runs every Driver's `tick()`). So core 0 is NOT available at the `Drivers::tick()` composite point to take a half — by snapshot time, control is on core 1. The clean structure the map points to:
- The parallel-for lives INSIDE the snapshot. The core-1 caller (already running) spawns/wakes a **second helper worker pinned to core 0** (`platform::spawnPinnedTask(..., core=0)` — the same seam Drivers uses for its core-1 worker, which confirms specific-core pinning works), hands it lights [0, winLights/2), and itself does [winLights/2, winLights).
- **Join** with a `std::atomic<bool> helperDone_`, acquire/release, polled with `platform::yield()` — byte-for-byte the `quiesceEncode()` spin idiom (Drivers.h:522-528). The core-1 caller waits the helper out, then runs the whole-buffer pattern-hold + `encodeSrc_` bias tail ONCE, and returns. Encode/prime/arm proceed on core 1 unchanged.
- The helper worker is spawned once (engage-time, beside the split's own task) and PARKED in `waitNotify`; each frame the snapshot `notifyTask`s it. No per-frame task churn.
- When the split is OFF (single-core), or the helper can't spawn: the snapshot runs the FULL [0, winLights) range inline — the shipped serial path, byte-identical.
- **Line-aligned split point**: round winLights/2 so each half's byte range starts on a 64-byte cache line (no false sharing on the boundary write); outCh-stride, so align on `lcm(64, outCh)` lights.

**Static 50/50 is the right FIRST cut** (simplest that works): the snapshot cost is uniform per light, so
halving it is optimal *for the snapshot itself*. The self-balancing atomic-ticket refinement (core 0 takes
MORE than half when its effect is cheap) is the follow-up that also covers the prime — deliberately not in
this step. A 50/50 snapshot split already halves the biggest segment.

## Signature change
`snapshotSourceForRing()` → `snapshotSourceForRing(nrOfLightsType lo, nrOfLightsType hi)` (a light
sub-range; default [0, winLights) preserves every non-split caller and the serial path). The per-light
loop bound changes from `i < winLights` to `lo ≤ i < hi`; the pattern-hold + bias-pointer tail stays
whole-buffer and runs once (on core 1, after the join) — it is cheap and not worth splitting.

## Code grounding
- `src/light/drivers/ParallelLedDriver.h` — `snapshotSourceForRing` (996-1039): add the [lo,hi) range
  params; the correction loop (1058) honors them; the pattern-hold/bias tail runs post-join, unchanged.
  `tickRing` (the caller) — core 1 runs its half here.
- `src/light/drivers/Drivers.h` — the split: core-1 worker fn runs the top-half snapshot on wake; core 0
  runs the bottom-half in `tick()` after composite; the join flag added beside `encodeDone_`/
  `renderSplitActive_` (478-495, 432). Reuse `notifyTask`/`waitNotify`/`WorkerTask` — NO new task.
- `src/platform/platform.h` — only if a join helper is cleaner than an inline `std::atomic` (the split
  already uses `std::atomic<bool> encodeStop_`, so the house style exists — likely no new platform API).

## Verification
1. **Host ctest**: the range-split snapshot must be BYTE-IDENTICAL to the whole-range one — run
   `snapshotSourceForRing(0, N)` vs `(0,N/2)+(N/2,N)` and memcmp the buffer. The existing ring
   byte-compare tests (unit_ParallelLedDriver_ring) are the oracle; add this split-equivalence case.
2. **S3 build + flash; bench 48×256**: read `ts` — it must ~halve (say 18→~10 ms); frame time down; fps up
   on a cheap effect; **`lt=0` MUST hold** (a snapshot race would show as scatter). renderWait KPI drops.
3. **PO eyes on the wall** — clean, same image, higher fps.
4. **Effect sweep**: cheap (Solid) vs heavy (Fire/GEQ3D) — fps rises where core 0 has spare, unchanged
   (never WORSE) when render-bound.
5. Confirm split OFF (multicore off) is byte-for-byte the serial path — the whole-range default.

## Out of scope (the follow-ups this step de-risks)
- Fork-join of the 14 ms PRIME (per-buffer chunks + core-1-owned arm) — the next step once the snapshot
  parallel-for is proven.
- Self-balancing atomic-ticket pool (core 0 takes >half on cheap effects; covers snapshot+prime together).
- ISR top/bottom-half split (highest risk to shipped correctness).
- IRAM-ing the snapshot kernel / SWAR the correction (throughput lever, if the floor still limits).
- 100 fps is not reachable; ~50 is the honest ceiling. This step targets ~40.
