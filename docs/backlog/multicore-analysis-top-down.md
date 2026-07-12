# Multicore & driver scaling — top-down build plan

> **Forward-looking build spec — exception to CLAUDE.md present-tense rule.** The Stage-2 implementation plan for scaling the render pipeline, built on the measured findings in the companion [multicore-analysis-bottom-up.md](multicore-analysis-bottom-up.md) (read that first: it establishes *what the bottleneck is* — the ~24 ms CPU WS2812 encode, not the DMA wait — and *why*; this document is *what to build, in what order*). Same bottom-up/top-down split as the [LED-driver](leddriver-analysis-top-down.md) and [live-script](livescripts-analysis-top-down.md) analyses. Each step below is its own increment: spec → `/plan` → implement → hardware re-verify. Citations use `file:line` against projectMM `HEAD`.

## The one-line thesis

The measured bottleneck is the **CPU encode** (the WS2812 bit→slot transpose, ~24 ms of a ~28 ms driver frame at 16384 lights), not the DMA transmit (~0 µs — the encode of frame N+1 already overlaps the clock-out of frame N for free). So the ordered levers are: **make the encode cheaper (one core) → then parallelise it (two cores, only if measured need) → then raise the ceilings (independent)**. Fix-the-encode is the prerequisite that unblocks or obviates everything after it. And the ceiling that matters is **lanes, not per-lane depth**: ~1024 LEDs/lane is the practical animated max (30 µs/light → ~33 fps), so more *animated* total comes from more lanes (16-lane, then shift-register fan-out), not from packing a lane deeper (2048/lane = 16 fps, a slideshow).

## Dependency graph

```
Step 0  Driver correctness ─── DONE (bottom-up § bugs)
                │
Step 1  Encode optimisation (1 core) ── SWAR transpose DONE ──► MEASURE ──► if still short: Step 2 (multicore)
                │                                                    │
                └── the fast transpose feeds ────────────────────────┴──► Step 3 (16-lane widening)
```

- **Step 1 unblocks Step 3**: 16 lanes doubles the transpose cost, so it *needs* the fast transpose first — now shipped (the SWAR transpose), so Step 3's 16-lane widening is unblocked.
- **Step 1 did NOT obviate Step 2**: the re-measure (post-SWAR) shows the encode is still ~22 ms at 16384 lights and a heavy effect adds ~17 ms of render, so `render+encode` on one core is 25–41 fps — a second core still buys a real ~45 fps floor. But the encode being the whole cost (DMA-wait ≈ 0) settles the *shape*: the pipeline (render on c0, encode on c1), not split-encode. See Step 2.
- So **Step 1's core (the transpose) is done and re-measured**; Step 2's *shape is decided* (the pipeline) but its *build* is still gated on a measured need (a flicker or a throughput ceiling); Step 3 is independently unblocked.

---

## Step 0 — Driver correctness (DONE)

The two bugs the P4 investigation surfaced are fixed + HW-verified (details in the bottom-up § "two driver bugs found — FIXED"): the Parlio over-limit **fail-loud guard** and the shared **bus-rebuild-on-shrink** fix. The shared-base reinit fix benefits every present and future `ParallelLedDriver` (LCD, Parlio, the coming parallel-I2S). Nothing further here — listed so the sequence is complete.

## Step 1 — Encode optimisation (the biggest single lever, one core) — SWAR transpose DONE

**Target:** the ~24 ms `encodeWs2812LcdSlots` transpose (85% of driver time; the `correction.apply` LUT is the other ~4.4 ms and already cheap). One-core, helps **every driver on every target**, no cross-core machinery.

**What shipped: the zero-memory SWAR transpose (`transposeLanes8x8` in `LcdSlots.h`).** The data slot is an 8×8 bit-matrix transpose (8 lane bytes → 8 bit-plane bytes); the hot loop now does it with the branch-free 3-delta-swap SWAR trick (Warren, *Hacker's Delight* §7-3; the same shape FastLED's `transpose8x1` uses) instead of a per-bit-per-lane gather. **~an order fewer ops on the 85% slice, zero table, less code.** Pinned bit-perfect by an exhaustive `unit_LcdLedEncoder.cpp` case (SWAR == naive gather over all lane patterns × masks) plus the on-device loopback self-test.

**Measured (P4 .133, 128×128 = 16384 lights, 8 Parlio lanes, 2026-07-12):** the `Drivers` container tick dropped **35961 µs → ~30100 µs (−16%, −5.9 ms)**, whole-device fps **25 → 30 (+20%)** — the only change was the transpose, so the whole delta is the SWAR win. Below the theoretical op-count ceiling because `correction.apply` (~4.4 ms) and DMA setup are untouched and the RISC-V compiler doesn't vectorise the 64-bit swaps as hard as hand-SIMD would; a free 20% fps on every frame (animated or static) from a zero-memory, less-code change validates the minimalism call — skip-when-unchanged would have been 0% on this moving content.

**LCD_CAM path verified (S3 n16r8 .159, 8-lane i80, 2026-07-12):** the *same* `encodeWs2812LcdSlots` runs on the S3's i80 peripheral driving a real 8×8 panel (pin 18) — healthy (`status: None`), encode scales linearly ~6 µs/light (8×64 = 512 → 3.8 ms; 8×512 = 4096 → 23 ms; 8×1024 = 8192 → 50 ms). **Side finding — the S3 LCD single-DMA-buffer ceiling is between 8192 and 12288 lights:** 8×1024 inits, 8×1536 fails with "LCD init failed — check pins / memory" (the ~1 MB single DMA allocation the i80 bus needs won't fit). The i80 bus fixes its transfer size at creation, so this is a hard per-config init limit, not a soft clamp — a candidate for the [LCD/Parlio DMA buffer → PSRAM](backlog-light.md) item.

**Two candidates were considered and rejected — the minimalism record:**

- **Skip-when-unchanged (rejected).** A per-driver frame-signature gate that skips the re-encode when the source bytes match last tick. It optimises the *rare* case (static/paused content) while *taxing the common* case (an animated effect changes every frame, so the hash never matches and the gate is pure per-tick overhead), and leaves the actual bottleneck — the animated-frame transpose — untouched. Net-negative against the hot-path rules (unconditional per-tick work + a fragile "every cold-path rebuild must invalidate" contract) for a narrow static-only win. Not built.
- **Bit→slot lookup table (rejected in favour of SWAR).** A 256-entry table trading ~2 KB for the per-bit math still needs the per-lane OR loop, so it's a *modest* win at a memory cost. The branch-free SWAR transpose beats it on every axis — memory (0 vs 2 KB), code size, speed, *and* recognizability (it's the textbook 8×8 transpose) — so the doc's old "lookup table → then SIMD" two-step collapses into the one SWAR step above.

**Still open (only if measurement shows the transpose is still the ceiling):** a wider SWAR path for the 16-lane case ("N strips × 8 bits" → "8 bit-planes × N-lane words") folds into [backlog-light § 16-lane](backlog-light.md) — study **troyhacks' claimed-faster Parlio transpose** and FastLED's `parallel_transpose.h` there, write our own, credit by name. The single-8-lane transpose is done; 16-lane widens the same construct.

**Verification:** re-run the 128×128 P4 measurement (the bottom-up's instrumented method); the transpose ms is the KPI. Loopback self-test stays bit-perfect after every change.

## Step 2 — Multicore: effects on core 0, encode+transmit on core 1 (the pipeline) — SHAPE DECIDED

**The shape is settled (product owner, 2026-07-12): the pipeline — render effect frame N+1 on core 0 while core 1 encodes+transmits frame N.** Not the split-encode alternative (both cores transposing one frame); the reasoning, from the post-SWAR re-measure + the effect-weight range, is below. "90% of the benefit for 10% of the cost."

**Why the pipeline, not split-encode — the re-measure conclusion.** The frame cost is `render + encode` on one core; two cores can make it `max(render, encode)` (pipeline) or, in the best case, `~max(render, encode/2)` (a fork-join hybrid). The numbers that decide it (P4, 16384 lights, post-SWAR: encode ≈ 22 ms, DMA-wait ≈ 0):

| Effect weight (P4 render, 16K) | 1 core `render+encode` | Pipeline `max(render,encode)` | Hybrid `~max(render,encode/2)` |
|---|---|---|---|
| **Light** (Checkerboard, ~2 ms) | ~24 ms → 41 fps | ~22 ms → **45 fps** | ~11 ms → 89 fps |
| **Heavy** (Noise, ~17 ms) | ~40 ms → 25 fps | ~22 ms → **45 fps** | ~17 ms → 57 fps |

Render is **not** a rounding error — a heavy effect (Noise) is ~17 ms at 16K on the P4 ([performance.md § Effect compute](../performance.md)), comparable to the encode. Three things fall out: (1) the pipeline gives a **~45 fps floor across the whole effect-weight range** (its `max(render,encode)` is encode-bound at ~22 ms whether the effect is light or heavy); (2) the hybrid's big win is **light-effects-only** (89 vs 45) — for a heavy effect core 0 is busy rendering the whole frame, so there's no spare time to "join" the encode and the hybrid collapses toward the pipeline (57 vs 45); (3) the hybrid costs far more (a fork-join mid-encode, a render→encode phase handoff) **and** carries the WiFi-asymmetry risk on classic/S3 (core 0 rejoining the encode gets WiFi-jittered; the halves finish unevenly and the join stalls on the slower core). The pipeline puts the *whole* encode on core 1, away from WiFi, so it's asymmetry-free on every platform. So: pipeline now; the hybrid is a possible *later* light-effect optimisation, gated on a real need.

**The DMA-wait ≈ 0 finding is what makes the pipeline's win real.** The pipeline overlaps render(N+1) with encode(N) — NOT with the transmit (the transmit wait is already ~0, nothing to overlap). The value is hiding render under encode (or vice-versa), turning `render+encode` into `max`. That's the ~45 fps floor.

**Audio / sensors don't need a core.** Audio FFT is ~13–22 µs on a non-completing tick and ~3 ms on the ~1-in-N tick that finishes a 512-sample block ([performance.md](../performance.md); a 22 kHz block spans ~23 ms, longer than a tick, so it completes roughly once per frame); the gyro/IMU is a 50 Hz I²C poll, not compute. Both fit in slack on either core — the encode (~22 ms every frame) dwarfs them. So the core split is just render↔encode; keep audio on the same (WiFi-free-*enough*) core as render so a completing-block tick doesn't land on the timing-critical encode core.

**Platform notes (the WiFi-asymmetry map):** on the **P4** WiFi runs on a *separate on-board ESP32-C6* co-processor (esp-hosted; the `esp32p4-eth` variant has no WiFi at all), so **both P4 cores are jitter-free** — the pipeline is clean and even the hybrid would be asymmetry-free here. On **classic ESP32 / S3** WiFi is on-die pinned to core 0, so the encode MUST live on core 1 (which the pipeline does anyway) — this is *why* the pipeline is the portable choice.

**Do NOT build speculatively.** Gate: run the [sigrok flicker cross-check](backlog-light.md) — a *measured* WiFi-induced glitch or a throughput ceiling justifies opening the build (the [Task core-pinning note](backlog-core.md)'s "defer until contention is observed"). What's already ours (so the gap is narrow): WiFi is already pinned to core 0 on classic/S3; the loop already yields `vTaskDelay(1)`; the producer/consumer seam already exists (bottom-up § mapping). What's new is the thread boundary + the handoff. Its own `/plan` when the gate opens.

**Design (when the gate opens):**

- **Split.** A **core-1 task owns encode+transmit** (the CPU-bound, timing-critical half — WiFi is on core 0, so the transmit is never preempted). Core-0's main loop renders the next effect frame + services HTTP/WiFi/WS. This is MoonLight's Effect-c0/Driver-c1 shape (bottom-up § MoonLight), re-aimed: *both* the encode and the transmit move to c1 (our encode is the cost, not just the transmit).
- **Handoff — one double-buffer + a task notification.** Core-0 composites frame N+1 into buffer B while the core-1 task encodes+transmits frame N from buffer A; swap the pointers at the frame boundary and `xTaskNotify` the core-1 task. A notification, **not** a full mutex+semaphore pair (lighter; the async-ArtNet-send item needs the *same* primitive — **build it once**, shared).
- **Memory — allocate-and-degrade, no `hasPsram` gate.** The second buffer is `platform::alloc` (PSRAM-first-else-DRAM); if it won't fit, **fall back to the inline single-task path** (the existing `Buffer::allocate`-returns-null → `tick()` checks pattern). No `if constexpr (hasPsram)` branch — the bottom-up establishes we reason from available memory, not a PSRAM flag.
- **Structural-change race — frame-boundary pointer-swap before a mutex.** Going multi-task, a structural change (add/delete a module, resize a grid, `prepareTree`) can land while the core-1 task reads the buffer — the single-task loop never had to guard this. MoonLight uses a `layerMutex`; **prefer a frame-boundary pointer-swap first** (the core-1 task reads the frame it was handed until handed a new one; a structural change lands on the *next* composite, never mid-read). Our `applyState`/`prepareTree` run at well-defined points, so verify a pointer-swap is sufficient before reaching for a hot-path mutex.
- **Watchdog.** At 16K+ lights a long encode can trip the task WDT — keep the `vTaskDelay(1)`/`esp_task_wdt_reset()` discipline MoonLight documents (we already yield `vTaskDelay(1)`).

**Split-encode — considered and set aside (see the re-measure table above).** Splitting one frame's transpose across both cores (~halve the encode → up to 89 fps for a *light* effect) was the tempting alternative, but it competes with rendering N+1 for the same two cores, collapses toward the pipeline for heavy effects, adds fork-join complexity, and inherits the classic/S3 WiFi asymmetry. Kept as a *possible later* light-effect-only optimisation on the P4 (where both cores are jitter-free), not the Step 2 shape.

**Out of scope for Step 2:** per-module core-affinity controls (a later refinement, only if a specific module needs pinning); desktop/Teensy equivalents (desktop is OS-threaded, Teensy single-core).

## Step 3 — Lane/byte ceiling raises (independent track)

These raise the *maxima* (more lights), not the *fps* — a separate axis from Steps 1-2, done when a board or need demands. All inherit the shared `ParallelLedDriver` base + Step 1's fast transpose.

**The fps wall sets the priority — lanes are the lever, per-lane is not.** WS2812 clocks 30 µs/light and all lanes clock in parallel, so the *per-lane* count alone sets the frame rate (bottom-up § TL;DR):

| LEDs/lane | fps (transmit-bound) | Verdict |
|---|---|---|
| 512 | 65 | smooth |
| **~1024** | **~33** (≈23 with encode) | **the practical animated ceiling** |
| 2048 | 16 | ambient/slideshow only |
| 4096 | 8 | slideshow |

So more LEDs *per lane* past ~1024 buys no usable animation — it's slower, not bigger-at-speed. The total that scales **at a usable frame rate** comes from **more lanes** (`~1024/lane × lane-count`): 16 lanes × 1024 = **16384 animated**. That reorders the items below by real value: **16-lane widening is the valuable one** (it's the only lever that raises the *total* without dropping fps); Parlio chunking is **marginal** (it only matters to reach the ~897→1024 band a single Parlio shot can't); per-lane beyond ~1024 is a static-install concern, not an animation one.

- **16-lane widening + variable 1..16 lanes — the valuable ceiling raise.** `lcdLanes`/`parlioLanes` 8→16 (SOC-derived; P4/S3 accept it), drop `kExactLaneCount` for LCD, `bus_width` = the pin count. This is the lever that raises the *animated* total (16×1024 = 16384 at ~33 fps), because it adds parallelism, not per-lane depth. Already fully specced in [backlog-light § 16-lane parallel output](backlog-light.md) — that item is the home; it folds in Step 1's transpose optimisation (the 3rd sub-step there). **Needs Step 1 first** (16 lanes doubles the transpose).
- **Virtual (shift-register) driver — the multiplier beyond 16 lanes.** Where 16 native lanes aren't enough, a virtual driver clocks external shift registers (74HC595-class) so **one GPIO fans out to 8+ physical strands** — hpwit's virtual-driver work claims up to ~120 outputs. This is the *right* answer to "more total animated LEDs" past the pin count: it multiplies lanes (throughput-preserving parallelism) rather than deepening a lane (fps-killing). In the pipeline as its own `ParallelLedDriver` peripheral, downstream of the 16-lane base. Study hpwit's `I2SClocklessVirtualLedDriver` prior art hard, write our own against `LcdSlots.h`, credit by name.
- **Parlio chunked transfer — marginal.** Lifts the 65535-byte/lane single-shot ceiling (897 RGB lights) by splitting a lane's frame into ≤65535-byte transactions with correct WS2812 inter-chunk timing (idle-LOW < 300 µs so the strand doesn't latch mid-frame). Parlio-specific (RMT streams, LCD chains — neither needs it). **Narrow value:** it only matters to close the ~897→1024 gap (a lane just over Parlio's hardware cap that's still animatable) — beyond ~1024/lane is a slideshow regardless, so this doesn't unlock a *fast* large display, only a slightly-larger-per-lane one. Specced in [backlog-light § Parlio chunked transfer](backlog-light.md).
- **Parallel-I2S driver** (classic ESP32 >8 lanes, hpwit `I2SClocklessLedDriver` lineage) — a new `ParallelLedDriver` peripheral; [backlog-light § classic ESP32 I2S](backlog-light.md). The shift-register virtual driver above is the I2S lineage's fan-out extension.
- **Per-model deviceModels pin defaults** — the catalog data that lets a fresh flash pre-fill the right lane GPIOs; the per-MCU usable-pin reference is in [gpio-usage.md § Usable LED-output GPIOs](../reference/gpio-usage.md), the per-device mapping is tracked in [backlog-core § LED output pins](backlog-core.md).

## What stays out (settled decisions, not steps)

- **Frame pacing** — decided against (bottom-up § Appendix A): redundant with time-aware effects, the wire rate already paces, the yield already guarantees responsiveness. Parked as a ~15-line opt-in only if a CPU-starved device appears.
- **The DMA-done-wait as a target** — the wait is ~0 (the encode overlaps the transmit for free); optimising *it* buys nothing. The encode is the lever. (This is the correction the whole investigation turned on.)
