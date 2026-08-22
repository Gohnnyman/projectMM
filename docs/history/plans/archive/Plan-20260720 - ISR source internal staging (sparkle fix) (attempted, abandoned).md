# Plan — Fix the last-row sparkle: stage the ring's ISR-read source in internal RAM (hpwit's memory model)

> **OUTCOME: attempted, abandoned (2026-07-20).** Built, tested (host byte-identity green), and flashed. A
> `stageBlackTest` diagnostic (zero the staging buffer; the ISR-encoded region must go black) PROVED the fix
> was live and correct — the bottom ROWS of every panel blacked out. But that revealed the mis-diagnosis: the
> sparkle is not the tail *rows* of each panel — it is the last 8 *panels* (a whole panel-row = strands/lanes
> 40–47 = the 6th and final 74HC595 in the shift chain, mostly panels 44–46, wandering within the last 8).
> That is a PER-STRAND-GROUP fault (the shift-chain tail's timing margin — the `595 shift clock window` class),
> on the wrong axis from this row-based fix. The `snP`/`snI` correlation that motivated the whole plan was
> correlation, not causation. **Lesson: verify the spatial extent of a visual fault (which strands vs which
> rows) with a blackout/ruler diagnostic BEFORE building a fix around a residency correlation.** All code
> reverted; the real investigation moves to the 6th-'595 shift-chain-tail margin.

## Context

At 48×256 (12,288 lights) the giant wall sparkles on ~3 bottom-row panels; the small wall (shiffy, 3,840
lights) is perfect on the identical firmware. Proven tonight by a cross-board A/B on the new `sn`/`lv`
ringDbg residency probe:

- **shiffy: `snI`** — snapshot in Internal RAM → clean.
- **giant wall: `snP`** — snapshot in PSRAM → sparkles.

Root cause (pinned, not theorized): the lapping ring's **EOF ISR encodes the tail slices reading from the
snapshot buffer, and at 12,288 lights that snapshot is PSRAM-resident**. The snapshot (36 KB) cannot sit in
internal RAM beside the DMA pool (37 KB) in the ~39 KB internal slack, so the internal-first alloc
(`ensureSnapshotCap`) silently falls back to PSRAM. A cache-safe IRAM ISR reading PSRAM under WiFi
flash-cache-off windows returns wrong bytes → the ISR-encoded slices (the frame tail = the bottom panels)
sparkle. The prime, which reads the same PSRAM snapshot in **task** context, is clean — so only the ISR
path is affected. Every ring counter is blind to this (they watch refill lateness, not read corruption);
only the eye and the `snI/snP` probe catch it.

This is a divergence from **hpwit's virtual driver**, our reference design. His model (recorded in
`docs/history/shift-register-driver-analysis.md:175`): a **PSRAM framebuffer → CPU refills small INTERNAL
DMA buffers → the DMA/encode only ever reads INTERNAL**. Our ring already has the small-internal DMA pool;
we just skipped the "source is read into internal" half and let the ISR read PSRAM directly. This plan
closes exactly that gap, which is both **the closest thing to hpwit's architecture** and **the only option
that scales to 10K+ lights** (it holds the internal cost to a fixed fraction of the window regardless of
light count; shrinking the pool to fit the whole snapshot internal caps out below ~1,500 lights).

## Design — an internal "ISR staging" buffer for the tail slices, filled in task context

The ISR encodes only the **tail** slices `[ringBufs, nSlices)` (the prime encodes `[0, ringBufs)` in task
context and is already safe). That tail is a contiguous source range and is a *fraction* of the window
(~7 KB source at 48×256, and it shrinks toward zero as the near-prime pool grows `ringBufs → nSlices`).

- **Allocate a fixed internal staging buffer** sized to the tail range's source bytes
  (`(nSlices − ringBufs)` worth of window lights × srcCh), via `platform::allocInternal` with **no PSRAM
  fallback** — if internal can't supply it (won't happen at realistic geometries; ~7 KB vs ~39 KB slack),
  the driver degrades to today's behavior (PSRAM snapshot) rather than failing. Grow-only, freed in
  `release()` with the other ring buffers.
- **In `tickRing`, after `snapshotSourceForRing()` and BEFORE `busTransmitRing()`** (both task context),
  copy the tail range PSRAM→internal: `memcpy(isrStage_, snapshotBuf_ + tailStartByte, tailBytes)`. This
  is the one PSRAM read of the tail, done in the safe context where PSRAM reads never corrupt. Costs one
  ~7 KB memcpy per frame on the render/encode thread (negligible beside the existing ~36 KB snapshot copy).
- **Point the ISR encode at internal for the tail.** `encodeSrc_` currently biases into `snapshotBuf_` so
  the encode index `winStart_ + laneStart_ + row` lands in the window. Add a second bias pointer
  `encodeSrcIsr_` into `isrStage_` (biased so the same index formula lands in the staging copy). The
  encode trampoline (`MoonLedDriver::encodeRowsTramp` → `encodeRows`) uses `encodeSrcIsr_` when it runs in
  ISR context (the tail refill) and `encodeSrc_` (the PSRAM snapshot) when it runs in the prime (task).
  The context is already known at the call boundary: the prime calls through `primeRingRange`, the ISR
  refill through `fillSlice`/`encodeRingSlice`. Thread a "read from staging" flag (or a resolved source
  pointer) down that one boundary so `encodeRows` reads the right buffer.
- **When the snapshot already lands internal** (small configs, `snI` — shiffy): the staging buffer is
  unnecessary. Guard the whole mechanism on `platform::ptrIsPsram(snapshotBuf_)` — only stage when the
  snapshot is actually in PSRAM. `snI` boards keep their exact current, proven-clean path (no new copy, no
  new buffer). This is the minimal-blast-radius default the robustness principle wants.

**Scope guard:** this does NOT change the prime, the completion, the frontier terminator, the geometry, or
the correction path. It adds one internal buffer + one task-context memcpy + one source-pointer selection
at the encode boundary. Correction stays fused in `encodeRows`' gather (unchanged), so the staging buffer
holds RAW source bytes at `srcCh` stride, exactly like `snapshotBuf_`.

## Code grounding

- `src/light/drivers/ParallelLedDriver.h`
  - `ensureSnapshotCap` (~1040): add a sibling `ensureIsrStageCap` sizing the tail buffer; alloc internal-only.
  - `snapshotSourceForRing` (~1073) / `tickRing` (~546): after the snapshot, if `ptrIsPsram(snapshotBuf_)`,
    memcpy the tail range into `isrStage_` and set `encodeSrcIsr_`; else leave it null (use the snapshot).
  - `encodeRows` (the gather, reads `encodeSrc_`): select `encodeSrcIsr_` vs `encodeSrc_` by call context.
  - `release()` (~390): free `isrStage_` alongside `snapshotBuf_`.
- `src/platform/esp32/platform_esp32_moon_i80.cpp`: `fillSlice`/`encodeRingSlice` (~1022/999) are the ISR
  encode entry — the "this is the ISR/tail path" signal originates here and is already distinct from the
  prime's `primeRingRange` call. No DMA/ISR structural change; the encode just reads a different source ptr.
- `src/light/drivers/MoonLedDriver.h`: the `encode` trampoline seam that carries the source selection.
- `platform::ptrIsPsram` (added this session) is the residency guard; `platform::allocInternal` the alloc.

## Verification

1. **Host ctest** (`unit_ParallelLedDriver_ring`): the staged-source encode must be byte-identical to the
   direct-snapshot encode — encode a frame both ways and memcmp. Add a case that forces the staging path
   (simulate `ptrIsPsram` true on the host mock) and asserts identical output bytes.
2. **S3 build + flash the giant wall** (`.194`, port `11201`): `ringDbg` should show the tail now sourced
   internal; **PO eyes: bottom row clean at 48×256, dense effect, snapshot+multicore on**. This is the
   measurement — counters are blind to the sparkle.
3. **Regression on the small wall** (shiffy `.150`): must stay `snI` and clean — the staging path is
   skipped there (guarded on `ptrIsPsram`), so it is byte-for-byte today's proven-clean behavior.
4. **fps check**: the extra ~7 KB task-context memcpy per frame must not dent fps meaningfully (it is ~20%
   of the existing snapshot copy). Read `ts`/frame time before/after.

## Flash-and-confirm the other bench boards (PO requested — AFTER the fix)

Order chosen: implement + verify the sparkle fix on the giant wall FIRST, then flash the final fix build to
the other bench boards as one multi-board confirmation pass, confirming each still serves + drives:
- **SE16** (esp32s3-n8r8, `.191`, port `/dev/cu.usbserial-20213432`)
- **testbench-P4** (esp32p4-eth, `.139`, port `/dev/cu.usbmodem5ABA0767291`) — needs the P4 build variant.
- **classic testbench** — needs the classic build variant; no classic bench board is on a live USB port
  right now (DigUno/DigQuad/Shelly/olimex offline or portless), so plug one in or skip classic for this pass.
This doubles as the "robust to any input across boards" check the fix must not regress.

## Out of scope (backlog)

- The `ringBufs`-past-RAM hard-fails-bus-init robustness bug (logged tonight).
- Moving correction into the snapshot (an fps lever, not needed for the sparkle).
- The prime-vs-drain barrier fps cost (already backlogged).
