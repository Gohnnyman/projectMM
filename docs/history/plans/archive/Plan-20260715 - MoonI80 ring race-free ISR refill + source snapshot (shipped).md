# Plan — MoonI80 ring: race-free ISR refill + source snapshot (reliable 256/strand, and the 100fps foundation)

> **Outcome (2026-07-15): source snapshot (steps 1–3) SHIPPED; ISR refill (steps 4–5) DEFERRED.**
> Step 4 (IRAM the encode path) collided with the platform-boundary hard rule: putting `IRAM_ATTR` on the
> domain encode functions in `src/light/` crosses the boundary, and contradicts platform.h's own stated
> reason the refill is a task. Resolution (PO): introduce a platform-neutral `MM_HOT` macro (→ `IRAM_ATTR`
> on ESP32, nothing elsewhere) and adopt it GRADUALLY in a fresh, focused `/plan`, informed by bench
> reuse-boundary measurements — rather than the single big ISR-refill leap this plan assumed. The snapshot
> half was independent and landed on its own (steps 1–3 + docs). The reuse race at ≥192 is therefore still
> open; see `docs/backlog/backlog-light.md` (MoonI80 ring HALTS entry).

## Context

**The goal:** make the MoonI80 shift-register ring reliable at **≥192 lights/strand** (the full 48×256 target), and lay the foundation for the **100 fps / 48-strand / 256-light** goal. Today the ring ships **128/strand** but stalls at ≥192 (and intermittently at 128) — the buffer-reuse race.

**The two coupled bugs (both backlogged):**
1. **Reuse race** — at >8 slices the DMA reuses ring buffers (node k and node k+8 point at the same buffer). A pinned FreeRTOS **task**, woken by a semaphore, re-encodes the drained buffer. If the task is late (task-wake latency + encode cost), the DMA reads a stale/half-written buffer → stall or 1-pixel shift.
2. **Source snapshot** — the refill task reads the live Layer buffer for the ~5 ms wire window; a grid resize / RGBW switch mid-wire → use-after-free read + frame tearing.

**What the GDMA research settled (decisive):**
- The GDMA **owner bit is NOT a stall primitive** on the S3/P4 — enabling `owner_check` only converts a silent race into an unrecoverable `DSCR_ERR` fault (worse for WS2812). No IDF driver uses it for flow control (zero runtime call sites). Keep `owner_check=false`.
- IDF's own **continuous-gapless** DMA driver (RGB-LCD **bounce buffers**, `esp_lcd_panel_rgb.c:1062-1084`) solves this identical producer/consumer race by doing the refill **synchronously inside the GDMA EOF ISR** (IRAM), sized/timed so the ISR-priority refill always beats the DMA draining the *other* buffer. The difference from our current design — and the whole race window — is that **our refill is a lower-priority task woken by a semaphore**, not an ISR.
- **The fix is to move the time-critical refill into the EOF ISR (IRAM)** — IDF's field-proven gapless pattern, and the same shape **hpwit** uses (Level-3 IRAM ISR refill, how he reaches ~100fps at 48×256 with WiFi up). This is the architecture that scales to the 100fps goal, not just a 256 patch.

**The IRAM constraint is a non-issue (was a misread):** the "IRAM 16384/16384 = 100%" is the tiny reserved *pure-instruction* window; the S3 has **unified DIRAM** (`IRAM_LOW`/`DRAM_LOW` are the same physical RAM at an offset), so `IRAM_ATTR` code draws from the **~342 KB DIRAM pool that's only 43% full (193 KB free)**. The P4 is likewise unified. The ring is `SOC_LCDCAM_I80_LCD_SUPPORTED`-gated (S3/P4 only, never the IRAM-tight classic ESP32), so the ISR-refill's cost lands only where there's room.

**This reverses the original plan's decision** ("task refill first; escalate to ISR only on measured underrun"). The escalation condition — a *measured* underrun (128/256 stall) — is now met, and both IDF and hpwit confirm ISR-refill is the correct shape.

## Design (settled)

### 1. Source snapshot (the immutable-input half — do this first, it's independent)
- At transmit time, `memcpy` the driver's window slice of the source (`winLen_ × srcCh` bytes, ~11.5 KB worst case for 16×256) into a **driver-owned staging buffer**.
- The encode trampoline reads the **snapshot**, not `sourceBuffer_->data()`. Makes the refill's inputs entirely driver-owned → no UAF, no tearing, regardless of what the render thread does mid-wire.
- `drainInFlight()` before `parseConfig()` (already added) covers the `wire_`/`laneCounts_` side; the snapshot covers the source-buffer side.

### 2. ISR refill (the reuse-race fix — IDF's bounce-buffer pattern)
- Move the per-slice refill from `moonI80RefillTask` (semaphore-woken task) **into the GDMA EOF ISR** (`moonI80EofCb`), `IRAM_ATTR`, matching `lcd_rgb_panel_eof_handler`.
- This requires the encode path reachable from the ISR to be IRAM-resident: `encodeRows` / `prefillShiftRows` / the ParallelSlots templates / `Correction::apply`. Mark them `IRAM_ATTR` (they land in DIRAM, 193 KB free — verified).
- The ISR does the refill directly (no task-wake latency), so it always finishes before the DMA laps into the reused buffer — the race window closes.
- Keep the drain-count termination (stop after `nSlices` drains) and the linear self-terminating chain — those are correct; only *who does the refill* changes (task → ISR), which is "a change of who calls the seam, not a rewrite" as the original plan foresaw.
- The `MoonI80EncodeFn` seam signature is unchanged; the trampoline (`MoonI80LedDriver::ringEncodeTrampoline`) and its `encodeRows` call just become IRAM-reachable.

### 3. Cache/coherency
- Internal DIRAM is not CPU-cached on the S3 for the LCD GDMA (verified: `esp_cache_get_line_size_by_addr` returns 0 for internal), so no `esp_cache_msync` needed — the ISR write is immediately visible to the DMA. Keep the existing no-op-guarded msync for correctness if a buffer ever lands cache-mapped.

## Implementation steps

1. **Save this plan** to `docs/history/plans/` (process rule).
2. **Snapshot** (`ParallelLedDriver.h` + `MoonI80LedDriver.h`): add a driver-owned staging buffer (grow-only, sized `winLen_ × srcCh`); `tickRing`/`startRingTransfer` fills it before the frame; the trampoline's `encodeRows` reads the snapshot. Host-testable (the mock can assert the encode reads the snapshot, not a mutated source).
3. **IRAM the encode path** (`ParallelSlots.h`, `Correction.h`/`.cpp`, the `encodeRows`/`prefillShiftRows` in `ParallelLedDriver.h`): `IRAM_ATTR` on the functions the ISR reaches. Verify the S3/P4 build still links (DIRAM has room) and the classic build is unaffected (ring inert there).
4. **ISR refill** (`platform_esp32_moon_i80.cpp`): move the refill body from `moonI80RefillTask` into `moonI80EofCb`'s ring branch (IRAM). Remove the task + `refillReady` semaphore (or keep a fallback path behind a flag). `on_trans_eof` → refill the drained buffer inline, advance the cursor, drain-count-terminate.
5. **Docs**: update the stale `asyncTransmit`-OFF-for-shift guidance (async+shift is now stable on the ring path) and the `shiftRegister` control doc.

## Verification

**Host (`ctest`):** the snapshot invariant (encode reads the snapshot, not a mutated source); the existing ring slice/tiling/recycled==fresh/clean-pad tests still pass (the ISR-vs-task move is platform-side, inert on host).

**Hardware — board B (shiffy, 192.168.1.150) — THIS is the acceptance test:**
1. **Loopback bit-verify PASSES at 192 AND 256** — the instrument that proves per-bit correctness through the reuse boundary (it stalled/corrupted before). This is the primary gate.
2. **128 no longer intermittently stalls**; 192 and 256 render coherently, no 1-pixel shift, zero GDMA errors.
3. A grid resize / preset change *while driving at 256* does not crash (snapshot proven).
4. `wireUs` at the ~5.5 ms floor; measure fps toward the 100 fps goal (this ISR-refill is the foundation for it).

**Also investigate during bring-up:** why 96/strand intermittently stalls — confirm whether it's on the whole-frame or ring path at the stalling moment (the 16-strand → 16-bit bus doubles the frame, so 96 may be crossing the internal-fit threshold into the ring).

**PO's eyes are the measurement — stop and hand over at "it's running on board B"; do not self-certify.** Build only the 3 ESP32 variants (classic/S3/P4).

## Scope guards

NOT touching `I80LedDriver` (esp_lcd) or Parlio. NOT the classic-ESP32 path (ring inert there; no IRAM cost lands on the IRAM-tight chip). The 100fps effort's *other* levers (encode SIMD, multicore) are separate increments — this plan delivers the race-free ISR-refill foundation they build on, and reliable 256/strand as the immediate win.
