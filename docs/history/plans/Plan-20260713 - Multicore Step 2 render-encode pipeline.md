# Plan — Multicore Step 2: the render↔encode pipeline

## Context

Every frame today runs `render → composite → encode → transmit` **serially on one task** (core 0). Step 1.5 (shipped) already hid the WS2812 **wire** wait behind a DMA double-buffer *inside the driver*. The remaining serial cost is `render + composite + encode`, and on a 16K-light grid the **encode dominates** (measured on the SE16, 16 lanes, async ON: **~3 µs/light → 49,916 µs at 16,384 lights**). Running that encode inline on core 0 **starves the network stack** (measured 2026-07-12, LightCrafter 16: the W5500 Ethernet on core 0 lost CPU → link dropped, HTTP timed out while the render loop kept ticking).

**Step 2 = the multicore pipeline:** render effect frame N+1 on **core 0** while a dedicated **core-1** task encodes+transmits frame N. This overlaps the two heavy stages — **render↔encode** — turning per-frame `render + encode` (serial) into `max(render, encode)`. Core 0 also stays responsive to HTTP/WiFi/WS under a load that previously stalled them.

**Gate note:** the network-starvation half of the design gate is proven; the sigrok LED-flicker cross-check was not run (PO opened the gate 2026-07-13 anyway — Step 1.5's whole-frame DMA already made the *output* underrun-immune, so the remaining win is fps + network responsiveness, both measured). Recorded per the auditable-gate rule.

## Where each stage runs — the load-bearing decision

Three stages, each a producer/consumer link:

| Stage | reads | writes | cost (16K) | core |
|---|---|---|---|---|
| **render** (effects) | — | logical Layer buffers | heavy | **0** |
| **composite** (`blendMap` logical→physical) | logical buffers | `outputBuffer_` | **cheap** (one linear pass) | **0** |
| **encode** (`encodeRows`: correction + SWAR transpose) | `outputBuffer_` | DMA buffer | **dominant, ~3 µs/light** | **1** |

**Composite runs on core 0** (with render), not core 1. Rationale: it's cheap (a fraction of the encode), and keeping it on core 0 makes core 1 a **pure encode consumer with one clean dependency — `outputBuffer_`** — so core 1 never touches the *logical* Layer buffers, which persist frame-to-frame for trails (`Layer.h`) and would otherwise entangle the two cores. The overlap is therefore **render+composite(N+1) ‖ encode(N)**, and since composite ≪ encode, core 0's per-frame producer cost ≈ render.

The clean cut is at the **Drivers boundary**: `Scheduler::tick()` ticks `drivers` last (`src/main.cpp:490`), so everything up to and including `Drivers::tick()`'s composite = render-side (core 0); the Driver *children's* `tick()` (encode+transmit, `ParallelLedDriver.h:205-255`) = the work offloaded to core 1. The Scheduler stays a single loop (`Scheduler.h:33-40` invariant preserved); the split is owned by `Drivers`.

## Step 2a (build now) — one output buffer, boundary handoff

Reuse the **existing single `outputBuffer_`** as the cross-core handoff — **no second buffer**. The composite fully rewrites it each frame (the bottom layer does `clearFirst`, `Drivers.h:266-271`), so it's a clean producer. The handoff is a frame boundary:

1. Core 0: render effects (N+1 logical) → composite into `outputBuffer_` (N+1). **Before overwriting `outputBuffer_`, wait for core 1's encode-done for frame N** (so the shared buffer isn't torn).
2. Core 0 notifies core 1; core 1 encodes `outputBuffer_` (now N+1) → transmits (Step 1.5 wire overlap intact).
3. While core 1 encodes, core 0 renders N+2's logical buffers (the overlap).

So the *cheap* composite is the serialization point; the two *heavy* stages (render, encode) overlap. Sync = an atomic done-flag + the `waitNotify`/`notifyTask` fence (SPSC, one producer/one consumer). No mutex.

**Identity / one-layer case inverts from today's behavior — deliberately.** Today a lone no-LUT layer skips `outputBuffer_` (zero-copy: drivers read the Layer buffer directly). With the split we *want* that output buffer — it's the stable buffer core 1 reads while core 0's effects mutate the logical buffer. So when the split engages, we **create `outputBuffer_` even for the identity case** (if memory allows), turning zero-copy into a real handoff. If memory doesn't allow it → inline fallback (today's zero-copy path unchanged).

## Step 2b (deferred — evaluate after 2a, gate on a measured need)

Add a **second output buffer (ping-pong)** so even the composite overlaps: core 0 composites into buffer B while core 1 encodes buffer A. Costs one full extra frame buffer (~48 KB at 16K lights).

**The metric that decides whether 2b is ever worth building** (capture it in 2a): **how long core 0 stalls at the composite boundary waiting for core 1's encode-done.**
- If effects are *heavy* (render ≈ encode): core 0's render(N+1) finishes ~when core 1's encode(N) does → **negligible stall → 2b buys nothing.**
- If effects are *light* (render ≪ encode): core 0 finishes early and stalls for most of the encode → **2b's ping-pong would recover that idle time.**

Instrument the stall in 2a (a `stallUs` KPI on the render loop, same shape as `wireUs`). Build 2b only if a real light-effect use-case shows a stall worth the 48 KB — same measured-need discipline as Step 1.5 / Step 2 gating. 2a's one-buffer path is a strict subset of 2b (front==back), so 2b is an additive change, not a rewrite.

## Implementation (Step 2a)

### 1. New `platform::` worker seam (core primitive — async-ArtNet reuses it)
`src/platform/platform.h`, near the RTOS-introspection block (`:52-81`). Opaque handle keeps FreeRTOS out of the header (same rule as `RmtWs2812Handle`):
```cpp
struct WorkerTask { void* impl = nullptr; };
using WorkerFn = void(*)(void* user);
bool spawnPinnedTask(WorkerTask&, const char* name, WorkerFn, void* user, size_t stackBytes, uint8_t priority, int core); // false → run inline
void notifyTask(WorkerTask&);                      // producer wake (any core)
bool waitNotify(WorkerTask&, uint32_t timeoutMs);  // worker blocks; false on timeout → service WDT
void stopPinnedTask(WorkerTask&);                  // signal+wake+join
void taskWdtReset();                               // esp_task_wdt_reset; no-op desktop
```
Textbook FreeRTOS `xTaskCreatePinnedToCore` + `ulTaskNotifyTake`/`xTaskNotifyGive` (the documented lock-free "lightweight binary semaphore"; matches `Buffer.h:22-25`).
- **esp32**: new `src/platform/esp32/platform_esp32_worker.cpp` (register in esp32 CMake); `impl` = `TaskHandle_t` + stop atomic; `esp_task_wdt_add(NULL)` at fn entry.
- **desktop**: `platform_desktop.cpp` — `std::thread` + condvar; ignores `core` (core-split scoped out, but the handoff invariants are host-testable on a real 2nd thread).

### 2. Handoff + boundary — `src/light/drivers/Drivers.h`
- Keep the single `Buffer outputBuffer_;` (`:293`). Add `std::atomic<bool> encodeDone_{true};` (core 1 sets it, core 0 waits on it).
- **Core 0** — in `Drivers::tick()` (`:253-288`), *before* the composite writes `outputBuffer_`: if split active, spin on `encodeDone_` (with `platform::yield()`) until core 1 signals the previous encode finished. Then composite, then `platform::notifyTask(encodeTask_)`. (The spin is bounded by the encode time; in the heavy-effect case it's ~0 because render already took that long.)
- **Core 1** — worker fn (`Drivers::runEncodeLoop`): `waitNotify` → for each offloaded driver `drv->setSourceBuffer(&outputBuffer_); drv->tick();` → `taskWdtReset()` → `encodeDone_.store(true, release)` → loop. It calls the **existing `ParallelLedDriver::tick()` unchanged**; Step 1.5's intra-driver DMA double-buffer is local to that call and stays correct on the new core.
- **Offload only pure encode+transmit drivers** (I80/Parlio). Keep Preview / NetworkSend / Hue (`main.cpp:204-205,438`) ticking inline on core 0 — they do socket/HTTP I/O; don't move network I/O across cores. Smaller, safer cut.

### 3. Task lifecycle — `src/main.cpp`
- Spawn at ~`:519` (after `scheduler.setup()`, before the loop): `if (drivers->wantsEncodeTask()) drivers->startEncodeTask();`.
- Stop at ~`:588` (after loop, before `scheduler.release()`): `drivers->stopEncodeTask();` — stop atomic, notify, wait for the worker to drain its in-flight encode before DMA buffers free (render-side analog of `drainInFlight`, `ParallelLedDriver.h:279-282`).

### 4. Engage predicate + degrade (small devices + identity mappings)
In `Drivers::prepare()` (`:215-227`). The split is an engaged capability, decided from the alloc outcome — **no `if constexpr(hasPsram)`**:
```
renderSplitActive_ = false;
if (a real pure-encode Driver child exists) {
    // In split mode we WANT outputBuffer_ even for the identity case (stable buffer for core 1).
    if (!outputBuffer_.data()) outputBuffer_.allocate(out->physicalLightCount(), out->channelsPerLight());
    if (outputBuffer_.data()) renderSplitActive_ = true;   // buffer fits → split ON
}
// else / on alloc-fail: outputBuffer_ per the existing needOutput rule (Drivers.h:217-226), inline path.
```
- **Low-mem alloc-fail → inline.** `outputBuffer_` stays null for the identity case (or the existing `DEGRADE` printf fires for the composite case), `renderSplitActive_ = false`, no task, zero-copy re-enabled — **byte-for-byte today's single-task path** (the only new code on this path is the false-branch of one `if (renderSplitActive_)`). Same allocate-and-degrade as `Drivers.h:218-226` + the Step 1.5 second-DMA-buffer degrade.
- **Live-reconfigure, no reboot.** Enabling a 2nd layer, adding a LUT, a grid resize, or a memory change all re-run `prepare()` (via `applyState`/`prepareTree`, `Scheduler.cpp:141-144`) and re-evaluate the predicate: not-engaged→engaged spawns the task; engaged→not-engaged (`stopEncodeTask`, free the buffer, inline). Both transitions land inside a **one-frame quiesce** (wait `encodeDone_` before `prepare()` reallocates/frees `outputBuffer_`), so core 0 never frees a buffer core 1 is mid-encode. `prepareTree` is cold-path → the pause is invisible. Matches the Live-reconfiguration principle (`Drivers.h:176-179`).

### 5. `stallUs` KPI (the 2b trigger metric)
Add a read-only KPI on the render loop = the time core 0 spent spinning on `encodeDone_` at the composite boundary (same shape as `wireUs`, `ParallelLedDriver.h:261-266`). This is the single number that says whether Step 2b (ping-pong) is ever worth the 48 KB. Report it on the `tick:` line.

### 6. `renderTaskName()` follow-up
`platform_esp32_tasks.cpp:94-101` returns the caller's task name — wrong once a 2nd task exists (affects only TasksModule's diagnostic row-nesting). Keep render = "main", expose the encode task name separately. Diagnostic-only, not a Step 2a blocker.

## Verification

**Host (extend the Step 1.5 MockDriver pattern, `test/unit/light/unit_ParallelLedDriver_doublebuffer.cpp`):**
- `unit_platform_worker.cpp` — spawn/notify/wait/stop round-trips on the desktop `std::thread` backing; one notify wakes exactly one wait; `waitNotify` times out → false.
- `unit_Drivers_rendersplit.cpp` — core 0 waits `encodeDone_` before overwriting `outputBuffer_` (assert no frame the worker reads is overwritten mid-encode — sentinel byte per frame, read exactly once); `stopPinnedTask` drains; **degrade** (buffer refused → no task, inline, identical output); **identity engage** (`needOutput=false` but split forces the buffer → real handoff; and if that alloc fails → inline zero-copy); **live engage/disengage** (toggle mid-run → task start/stop inside the quiesce, no use-after-free). **Run host suite under ASan/TSan** — a race/use-after-free on `outputBuffer_` fails the build (the proof the one-buffer boundary is sufficient).

**Hardware (P4 + S3/LC16):**
- **fps lift + stall**: `tick: Nus (FPS: M)` (`main.cpp:534`) + the new `stallUs`. Compare heavy vs light effects: heavy → fps ≈ `max(render,encode)`, stall ≈ 0; light → fps rises but stall > 0 (the 2b signal). Compare engaged vs forced-degrade on the same effect.
- **network no longer starved**: heavy 16K effect + concurrent `/api/*` + WS preview on core 0 stays responsive. Watch `resetReason()` for `TASK_WDT` — the core-1 encode must self-reset its WDT.
- **robustness**: loop add/delete driver + grid resize + `applyState` under a heavy effect; watch `LoadProhibited`/PANIC (the `Drivers.h:305-309` dangling-`layer_` class). RMT boards (separate hierarchy, split N/A) show no regression via the loopback self-test.

## The one open decision (verify during build)
**Boundary-handoff sufficiency for the structural-change race.** Assumed: atomic `encodeDone_` + notify fence + one-frame quiesce is enough, no hot-path mutex. Verify via host TSan + the hardware structural-change stress. **Mutex fallback only if a real race shows** — one short mutex around the worker's "read source → tick drivers" re-wire, core-0-only during `prepare()`; never the per-pixel encode (`Buffer.h:22-25`).

## Scope guards
Step 2a only (one buffer). Step 2b (ping-pong 2nd buffer) is DEFERRED, gated on a measured `stallUs`. NOT composite-on-core-1 (keeps core 1 off the trail-persisting logical buffers). NOT split-encode. NOT per-module core-affinity. NOT desktop/Teensy core-split (host seam = std::thread for compile + invariant tests). NOT Step 4 chunked transfer. NOT an fps cap. NOT offloading Preview/NetworkSend/Hue. Scheduler stays a single loop.

## Files
- `src/platform/platform.h` — worker seam.
- `src/platform/esp32/platform_esp32_worker.cpp` (new) + esp32 CMake.
- `src/platform/desktop/platform_desktop.cpp` — std::thread backing.
- `src/light/drivers/Drivers.h` — `encodeDone_`, boundary spin, worker fn + lifecycle, engage predicate + identity-forces-buffer, degrade + quiesce, `stallUs`.
- `src/main.cpp` — spawn (~:519) / stop (~:588) + `stallUs` on the tick line.
- `test/unit/light/unit_platform_worker.cpp` + `unit_Drivers_rendersplit.cpp` (new; CMake).
- Follow-up (not blocking): `platform_esp32_tasks.cpp` renderTaskName under multi-task.
