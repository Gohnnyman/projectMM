# Multicore & driver scaling — landscape analysis

> **⚠️ DATED SURVEY (2026-07-12) — several conclusions have since been SUPERSEDED by shipped work. Read this header before trusting any number below.**
>
> What still holds: the Parlio 65535-byte/lane single-transfer ceiling and its bytes-per-light math; the two driver bugs found + fixed; MoonLight's model as read; the frame-pacing decision (Appendix A). What has been overturned:
>
> | This doc says | Reality (shipped since) |
> |---|---|
> | "the DMA transmit-wait is ~0 µs — the driver does **not** block on the transmit" | **False.** The transmit *did* block. Step 1.5 (async double-buffer, `asyncTransmit`) shipped to overlap it, worth P4 48 → 76 fps. The measurement below missed it because the instrumented build sampled the wait in the wrong place. |
> | 8 lanes; `LcdLedDriver` is S3-only | **16 lanes** shipped for both parallel drivers; the driver is `I80LedDriver` and runs on **classic ESP32 (I2S) + S3/P4 (LCD_CAM)**. |
> | a parallel-I2S driver is future work | **Shipped** (classic ESP32, I2S i80). Its ceiling is internal-RAM, not a transfer cap: 2048 lights at 8 lanes. |
> | "multicore last, gated on need" | **Step 2a shipped** — the whole output stage runs on core 1 (`multicore` on Drivers): +44 % fps, 85 % of the output off the render core. |
> | Parlio must use a refill ring | It uses the **same whole-frame double-buffer** as i80; the ring is unneeded and deferred indefinitely. |
>
> Current, present-tense numbers live in [performance.md § Multi-pin LED driving](../performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid) and [§ Multicore](../performance.md#multicore-the-whole-output-stage-on-core-1-multicore-step-2). The build plan and its outcome are in the [top-down](multicore-analysis-top-down.md). This document is kept as the **design-intent record of how we got there** — including the wrong turn, which is the point.

> **Forward-looking research document — exception to CLAUDE.md present-tense rule.** A Stage-1 bottom-up survey of *scaling the render pipeline* for projectMM: how large a display each driver can drive, where the time actually goes at scale, and whether a second core (à la MoonLight) earns its place. It combines a read of **MoonLight's documented dual-core architecture** ([moonmodules.org/MoonLight/develop/architecture](https://moonmodules.org/MoonLight/develop/architecture/), read **2026-07-12**) with **hardware measurement on the ESP32-P4 at 128×128 = 16384 lights** the same day, and the [frame-pacing decision](#appendix-a--frame-pacing-decided-against) settled with the product owner. Per *[Industry standards, our own code](../../CLAUDE.md#principles)*: study the reference, measure our own, write our own recommendation. Consolidates the multicore threads scattered across the backlog (see [§ Existing backlog](#existing-backlog-this-consolidates)). Citations use `file:line` against projectMM `HEAD`; the ESP-IDF ceilings cite the IDF HAL; MoonLight claims cite the architecture page.

## TL;DR

- **The headline capability question — "how many LEDs can each driver drive?" — has a measured answer, and it surprised us.** The bound is *not* DMA bandwidth or lane count; it is (1) a per-peripheral single-transfer ceiling and (2) the CPU **encode** cost. Measured on the P4: rendering 16384 lights takes ~2.5 ms and the driver's per-frame **WS2812 transpose encode takes ~24 ms** (85% of it). **The "and the DMA wait is ~0" reading below was WRONG** — the transmit really did block; Step 1.5's async double-buffer later recovered it (P4 48 → 76 fps). The durable half of the finding stands: the encode is CPU-bound and dominates.
- **Per-driver maximum lights per pin (the number the product owner asked to pin down):**

  | Driver | Chips | Lanes/pins | **Hard per-pin ceiling** | Practical per-pin | Bound by |
  |---|---|---|---|---|---|
  | **RmtLedDriver** | classic ESP32, S3, P4 | up to 8 (RMT TX channels) | **none** — streams via ping-pong; only `nrOfLightsType` (65535 on classic uint16, ~4 B on PSRAM uint32) + memory | ~1024 (fps) | fps / memory |
  | **LcdLedDriver** (i80) | S3 | exactly 8 | **none** — chains DMA descriptors; type + memory only | ~1024 (fps) | fps / memory |
  | **ParlioLedDriver** | P4 | 1..8 | **65535 BYTES/lane HARD** — one-shot transfer capped at `PARLIO_LL_TX_MAX_BITS_PER_FRAME` = 524287 bits = 65535 bytes. In *lights* this depends on channels/light (see below): **897 RGB**, ~673 RGBW, ~538 RGBCCT | 897 RGB (hardware) | Parlio single-transfer register |

  **The Parlio ceiling is a BYTE limit, not a light limit** — because the DMA buffer holds the WS2812 *waveform*, not colour bytes: one light = channels × 8 bits × 3 slots = **24 bytes/channel** (RGB = 72 B/light, RGBW = 96, RGBCCT = 120, a 24× expansion). Plus a ~864-byte latch pad. So max lights/lane ≈ (65535 − 864) / (channels × 24): **897 RGB, 673 RGBW, 538 RGBCCT**. State it as bytes-per-lane, not a fixed light count, since wider fixtures fit fewer. **RMT and LCD have no hard per-pin maximum** — the `kMaxWs2812LedsPerPin = 2048` in the code is a *clamp-and-warn* (a chosen "still animates ≥16 fps" floor, [PinList.h:23](../../src/light/drivers/PinList.h)), NOT a hardware wall: exceed it and the driver clamps + warns, it does not reject. The only true ceilings there are `nrOfLightsType` width and memory. **Parlio is the exception**: its 65535-byte/lane single-shot limit is a genuine hardware register cap (proven below).
- **Max total lights, one driver, one shot, today (8 lanes):** RMT / LCD reach **8 × 2048 = 16384** RGB at the soft-clamp (and could go higher per pin at lower fps); Parlio reaches **8 × 897 ≈ 7176** RGB (its hardware wall; fewer for wider fixtures). **The product owner's "16K over 8 lanes" claim is real for RMT/LCD; for Parlio it needs the chunked-transfer enhancement (below).**
- **fps, not lanes, is the real ceiling above ~1000 lights/lane.** WS2812 clocks 30 µs/light (24 bits × 1.25 µs). All lanes clock in parallel, so 8×2048 = 16384 transmits in the time of one 2048 lane = 2048 × 30 µs ≈ **61 ms ≈ 16 fps** — the product owner's own `800 kHz / 2048 / 24 = 16.27 fps` is exactly this. Add the ~24 ms encode and 16K is a ~10–16 fps display: usable for slow/ambient, a slideshow for fast animation. **Driving *more* lights is a lane-count + memory question; driving them *fast* is an encode + wire-rate question** — and the encode is the CPU-bound part a second core can attack.
- **Where multicore actually helps — the encode, not the wait (corrected).** MoonLight's model (Effect on core 0, Driver on core 1, double-buffered handoff) is sound, but the *reason* it helps is not "hide the transmit wait" (our measurement shows the wait is ~0). It is that the **~24 ms WS2812 transpose is CPU-bound and embarrassingly parallel** (per-row, per-lane). A second core can (a) encode+transmit frame N while core 0 renders effect frame N+1, or (b) split the encode across both cores (each does half the lanes/rows → ~halve the 24 ms). *That* is the win. Plus the classic WiFi-timing-isolation benefit for the transmit.
- **Half the multicore isolation is already ours.** projectMM already pins WiFi to core 0 (`CONFIG_ESP_WIFI_TASK_PINNED_TO_CORE_0=y`), and the render loop already yields cooperatively every tick (`vTaskDelay(pdMS_TO_TICKS(1))`, [platform_esp32.cpp:169](../../src/platform/esp32/platform_esp32.cpp)) — the same conclusion MoonLight reaches ("`taskYIELD()` is not good enough… we need `vTaskDelay(1)`"). So the gap to MoonLight is narrow: spawn a core-1 task for the timing-critical **encode+transmit** and hand it frames.
- **The producer/consumer seam already exists.** Effects produce into a `Buffer`; `Drivers` composites into `outputBuffer_` **only when needed** (≥2 enabled layers OR a LUT-mapped single layer, [Drivers.h:217](../../src/light/drivers/Drivers.h)); a lone no-LUT layer is handed its buffer zero-copy. MoonLight's core-0-produces / core-1-consumes is that same seam across a thread boundary. The one genuinely new piece is the **cross-core handoff** (a double-buffer + notification) and the **structural-change lock** it forces (a mutation while the driver task reads the buffer — the single-task loop never had to guard this; MoonLight uses `layerMutex`, we should consider a frame-boundary pointer-swap first).
- **Memory model: available-memory, not `hasPsram`-branch.** projectMM allocates PSRAM-first-else-DRAM (`platform::alloc` tries `MALLOC_CAP_SPIRAM`, falls back, [platform_esp32.cpp:98](../../src/platform/esp32/platform_esp32.cpp)) and *degrades* on failure (`Buffer::allocate` returns null, `tick()` checks). So the multicore double-buffer should be **allocate-and-degrade** ("try the second buffer; if it fails, run the inline single-task path"), matching the existing pattern — **not** an `if constexpr (hasPsram)` gate. (`hasPsram` is used only to widen `nrOfLightsType`, [light_types.h:54](../../src/light/light_types.h).)
- **Recommendation: fix the driver correctness first (done), optimise the encode next, multicore last, all gated on need.** (1) The two driver bugs this investigation found are **fixed** (below). (2) The biggest single lever is the **~24 ms encode** — before a second core, ask whether the WS2812 transpose can be cheaper (a bit→slot lookup table, or skip-when-frame-unchanged). (3) Multicore (encode+transmit on core 1) is the parallelism play once single-core encode optimisation is exhausted — and only when a measured flicker/throughput ceiling justifies the second task + handoff machinery, per the [Task core-pinning note](backlog-core.md)'s "defer until contention is observed."
- **Future work that reshapes the ceilings** (product-owner-flagged, 2026-07-12): the axis that matters is **lanes, not per-lane depth** — 30 µs/light caps a lane at ~1024 LEDs for animation (~33 fps; 2048/lane is 16 fps, a slideshow), so *animated* total scales with lane count. **16 lanes (choose 1..16)** for all parallel drivers is the valuable raise (LCD/RMT → 16×1024 = 16384 animated; Parlio → 8×897 RGB one-shot per its byte-limit); a **virtual (shift-register) driver** (hpwit `I2SClocklessVirtualLedDriver` style, ~120 outputs via 74HC595-class fan-out) multiplies lanes past the pin count — the right lever for still-more animated LEDs. A **Parlio chunked-transfer** enhancement only closes the ~897→1024 band (marginal, since >1024/lane is a slideshow anyway); a **parallel-I2S driver** (hpwit `I2SClocklessLedDriver` style) adds another `ParallelLedDriver` peripheral. All inherit the shared-base fixes below. The build ordering is in the [top-down § Step 3](multicore-analysis-top-down.md#step-3--lanebyte-ceiling-raises-independent-track).
- **Out of scope for this survey (→ [top-down](multicore-analysis-top-down.md)).** The build spec — the encode-optimisation schemes, the core-1 task-creation + handoff primitive + buffer-swap, the structural-change race resolution, and how the ceiling-raises fold into the driver backlog — all live in the top-down. This document is the findings + the ceilings + the ordered *what*; the top-down is the *how*.

## The hardware measurement (P4, 16384 lights, 2026-07-12)

Grid set to 128×128 on the P4 (`MM-P4`, `esp32p4-eth`), ParlioLed driver, instrumented build. Serial per-second timing, stable across samples:

| Stage | Time | Notes |
|---|---|---|
| Effect render (`Layer`, BouncingBalls) | ~2.5 ms | scales with grid, cheap |
| Driver `correction.apply` (brightness LUT + reorder + white) | **~4.4 ms** | 15% of the encode |
| Driver `encodeWs2812LcdSlots` (RGB byte → 3-slot bus bytes × lanes) | **~24 ms** | **85% of the encode — the bottleneck** |
| `busWait` (DMA clock-out) | ~0 µs *(MIS-MEASURED — see the header)* | The instrumented build sampled the wait where it could not land. The transmit **did** block (~7.5 ms/frame at 16×256), which Step 1.5's async double-buffer later hid. |

So the driver's per-frame cost is dominated by the **CPU encode** (the WS2812 transpose, 5.4:1 over `correction.apply`) — that part held up and drove the SWAR work. **The "DMA wait contributes nothing" half did not**: the transmit blocked, and hiding it behind a double-buffer (Step 1.5) was worth 48 → 76 fps on the P4. Both stages were real; this survey only saw one of them.

The measured multi-pin driving results across all three peripherals (Parlio/LCD/RMT at 128×128, with the bench pins used) are recorded permanently in [performance.md § Multi-pin LED driving](../performance.md#multi-pin-led-driving-all-three-peripherals-128128-grid) — present-tense, so they outlive this forward-looking analysis. The SWAR win headline: the Parlio `Drivers` tick dropped 35961 µs → ~30100 µs (−16%) at 16384 lights.

## The two driver bugs this investigation found — FIXED

Driving the P4 at 8 lanes surfaced two real defects (both fixed in this change; both HW-verified with LEDs burning at 8×896 = 7168 lights):

1. **Parlio single-transfer hardware ceiling was hit silently.** 8 lanes × 2048 lights = a 148 352-byte frame = 1 186 816 bits, over the P4 Parlio `PARLIO_LL_TX_MAX_BITS_PER_FRAME` = 0x7FFFF = **524 287-bit** single-transfer limit. `parlio_tx_unit_transmit` returned `ESP_ERR_INVALID_ARG` and **nothing lit, with no error surfaced**. This ceiling is **Parlio-specific**: LCD i80 *chains* DMA descriptors (no frame cap), RMT *streams* through a ping-pong buffer (no frame cap) — verified against the IDF HAL. **Fix:** Parlio's `busInit` now rejects a frame over `kParlioMaxTransferBytes` (65535) up front and reports the init failure as a driver status, instead of creating a unit that fails every transmit ([platform_esp32_parlio.cpp](../../src/platform/esp32/platform_esp32_parlio.cpp)). Per the product owner: **document the 65535-byte/lane (897 RGB) ceiling, don't guard the UI input** — the driver surfaces a clear status; the user's remedy is fewer lights/lane, the start/count window, or (future) chunked transfer.
2. **The parallel-driver bus was reused when the frame SHRANK, keeping an invalid unit.** `reinit()` reused the bus whenever `busCapacity() >= frameBytes_` (grow-only). A shrink (2048 → 896 lights) kept the *oversized* unit whose configured `max_transfer_size` still exceeded the hardware limit — so every transmit kept failing silently even at the smaller, valid size. **Fix:** the shared `ParallelLedDriver::reinit()` now reuses the bus only on an **exact** size match (`==`, not `>=`); any grow or shrink rebuilds, so the bus is always valid-or-rebuilt ([ParallelLedDriver.h](../../src/light/drivers/ParallelLedDriver.h)). This is a **shared-base fix** — every `ParallelLedDriver` inherits it: the i80 driver (LCD_CAM on S3/P4, I2S on the classic ESP32) and Parlio.

## MoonLight's model, as read (2026-07-12)

Quoted where specific:

- **Core assignment.** Core 0: WiFi/BT (prio 23), lwIP (prio 18), **Effect Task (prio 3)**. Core 1: **Driver Task (prio 3)**, ESP32SvelteKit UI (prio 2). "If Driver Task were on Core 0, WiFi would constantly preempt it… DMA/I2S/LCD/PARLIO require uninterrupted timing."
- **Effect vs driver.** Effect = "pure computation… tolerant to preemption," ~60 fps. Driver = "timing-critical… requires uninterrupted execution."
- **Handoff (double buffer).** Effect writes per-layer `virtualChannels` → mutex → `compositeLayers()` → `channelsD` → `newFrameReady=true` → release (~10 µs). Driver captures `channelsD`, DMA-sends, gives `channelsDFreeSemaphore`. "Double buffering overhead is negligible (<1%)."
- **Structural lock.** `layerMutex` guards `mapLayout()`, `Node::onSizeChanged/loop`, `NodeManager::onUpdate` against the two render tasks.
- **Watchdog.** At 16K+ LEDs: `esp_task_wdt_reset()` + `vTaskDelay(1)`; "taskYIELD() is not good enough… only yields to tasks of equal or higher priority."

## Mapping onto projectMM

| MoonLight | projectMM today | Verdict |
|---|---|---|
| WiFi pinned to core 0 | already pinned | ✅ same |
| `vTaskDelay(1)` yield | already `vTaskDelay(1)` | ✅ convergent |
| Effect (c0) + Driver (c1), two tasks | one task (`mm_main` loop) | ❌ the core of the work |
| `channelsD` + semaphore handoff | one `outputBuffer_`, no cross-core handoff | ❌ need double-buffer + notification |
| `layerMutex` for structural changes | structural changes inline in one loop | ⚠️ becomes necessary going multi-task — consider a frame-boundary pointer-swap over a hot-path mutex |
| Producer/consumer across cores | producer/consumer within one task | ✅ same model, not yet across a thread |
| Fixed 60 fps | uncapped + time-aware effects | 🔀 deliberate divergence — Appendix A |
| Blocks on transmit | **does NOT — encode dominates, wait ~0** | 🔬 our measurement; multicore targets the *encode* |

## Buffering models compared (2026-07-12)

Before adding buffers (Step 1.5 double-buffer, Step 2 cross-core, Step 4 chunked), we surveyed the buffering techniques the field uses, so our choice is *Industry standards, our own code* and not a guess. **There are THREE distinct buffering models, and which one a peripheral needs is forced by its DMA hardware — not a free choice.** Studied under [*Industry standards, our own code*](../../CLAUDE.md#principles): the models below are the recognized techniques; the source links are where to study each; we carry the technique and write our own code against our architecture, not trace any implementation's structure.

| Model | Where the technique appears | How it works | WiFi-flicker risk | Buffers |
|---|---|---|---|---|
| **Whole-frame → one burst** | **projectMM (LCD + Parlio today)** | encode the WHOLE frame into a DMA buffer, then fire ONE autonomous transfer | **ZERO mid-frame risk** — the buffer is complete before the DMA starts; nothing races it | 1 big |
| **Whole-frame ping-pong** | the standard double-buffer / deferred-wait pipeline (e.g. FastLED's LCD_CAM path) | two whole-frame buffers; encode N+1 into the back while DMA drains N from the front; wait-at-*start*-of-next-`show()` | zero mid-frame; adds **1 frame latency** | 2 big |
| **Chunk-streaming ring** | the standard DMA-ring / transpose-on-the-fly technique (e.g. FastLED's Parlio path, the I2S-clockless family with a tunable `nbDmaBuffer`) | the frame does NOT fit one DMA transfer, so it's split; a small ring of chunk buffers is **refilled on-the-fly by an ISR** as each drains (transpose-on-the-fly) | **HIGH** — a late refill ISR underruns mid-frame → a glitch pulse latches the strand early → **flicker** | 3–**75** small |

**The key finding — why whole-frame buys flicker-immunity for free.** The chunk-streaming model has a conveyor-belt property: the DMA reads while an ISR refills behind it, so **if WiFi preempts the refill ISR for tens of µs, the DMA drains an unfilled chunk → underrun → flicker.** The standard mitigation is a *timing cushion* — a deep DMA ring (a large `nbDmaBuffer`); we ran one at **75** on the S3 in the StarLight era (75 LED-rows of pre-filled runway so a WiFi interrupt burst can't catch the DMA up to an empty slot). **projectMM's whole-frame-then-burst model doesn't hit this at all**: the DMA reads a *finished, self-contained* buffer, so no ISR races it and WiFi cannot underrun a frame mid-transfer. That is why we never needed — and never discuss — a large `nbDmaBuffer`: our peripheral choice (LCD_CAM chains DMA descriptors, Parlio does one autonomous transfer) sidesteps the case a deep ring exists to cover.

**Two DISTINCT WiFi-vs-LED failure modes — don't conflate them:**
- **DMA underrun** (the deep-ring case): WiFi preempts the *refill ISR* mid-frame → glitch. **Only affects chunk-streaming.** Whole-frame is immune.
- **Core-0 starvation** (the LC16 finding, [this doc's measurement + top-down § Step 2](multicore-analysis-top-down.md)): a heavy *encode* hogs core 0 so the *network stack* (also core 0) starves → HTTP dies, and on a device whose LED timing shares core 0, the render can hitch. **Affects whole-frame too** — it's the encode, not the DMA. The fix is the multicore pipeline (encode on core 1), a *different* fix than more buffers.

**Buffer count is not pipeline depth.** A driver can hold several buffers yet still serialize if it waits on the previous transfer at the *top* of each frame (`wait_all_done(portMAX_DELAY)` before encoding) — the buffers exist but encode↔transmit never overlap (frame latency = encode + transmit, not `max`). The overlap comes from wait-*placement*, not buffer count: our Step 1.5 waits at the *start* of the next `show()` and encodes into the back buffer, which is what produces the overlap. (Reference to study for the ring machinery: [troyhacks/MoonLight parlio.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/parlio.cpp).)

**Latency vs throughput — the sound-reactive tradeoff (measured).** Buffering trades latency for throughput. One frame is imperceptible for animation (so it's rarely called out), but projectMM's **sound-reactive** priority makes it worth stating:

| Pipeline | Added latency | Frame @125 fps | Sound-reactive impact |
|---|---|---|---|
| **current (single-buffer, synchronous)** | **0 frames** — sample→photons in the same tick | — | ✅ best possible |
| **+ Step 1.5 (double-buffer DMA)** | +1 frame (DMA of N finishes during tick N+1) | ~8 ms | negligible (beat ≈ 500 ms; A/V sync tolerance ≈ 50–80 ms) |
| **+ Step 2 (render↔encode across cores)** | +1 more frame | ~8–22 ms | small |

So 1–2 frames (~16–30 ms) is well under human A/V-sync tolerance — but it is a real cost. **Consequence for the design: keep the double-buffer OPT-OUT** (a driver/global flag), so a latency-critical sound-reactive setup can keep the 0-latency synchronous path. This opt-out is *our* choice, driven by the sound-reactive requirement.

**What this means per step (folds into [top-down](multicore-analysis-top-down.md)):**
- **LCD (S3): whole-frame ping-pong** (Step 1.5) — the standard double-buffer pipeline; validated as correct.
- **Parlio (P4): a chunk-streaming ring, NOT a full-frame ping-pong** — two full 16-lane frames don't even fit the 65535-byte cap, so the ring is *forced*, and it merges Step 1.5 + Step 4 into one structure (a small ring + underrun counter, refilled by a worker ISR).
- **Classic-I2S (backlog, below): inherits the underrun/flicker problem** — it's chunk-streaming by nature (no LCD_CAM/Parlio on classic), so it needs the ring **and** the `nbDmaBuffer` flicker-cushion tuning the I2S-clockless family carries. Written into that backlog item so we don't rediscover it.
- **Encode buffer stays SINGLE** in every model — only the DMA target is doubled/ringed. Our plan already does this.

Sources (technique study, not code to trace): [FastLED LCD_CAM engine](https://github.com/FastLED/FastLED/tree/master/src/platforms/esp/32/drivers/lcd_cam), [FastLED Parlio engine](https://github.com/FastLED/FastLED/tree/master/src/platforms/esp/32/drivers/parlio), [hpwit I2SClocklessLedDriver](https://github.com/hpwit/I2SClocklessLedDriver/blob/main/src/I2SClocklessLedDriver.h), [troyhacks/MoonLight parlio.cpp](https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Drivers/parlio.cpp).

## Existing backlog this consolidates

| Note | Where | Now |
|---|---|---|
| Core-1 driver task + per-module core-affinity | [backlog-light.md](backlog-light.md) | This doc's multicore recommendation, re-aimed at the **encode** |
| Task core-pinning ("defer until contention observed") | [backlog-core.md](backlog-core.md) | Carried forward — gate on measured need |
| Async ArtNet send (PSRAM handoff) | [backlog-core.md](backlog-core.md) | Shares the same handoff primitive — build once |
| sigrok flicker cross-check | [backlog-light.md](backlog-light.md) | The measurement that opens the multicore gate |
| `rmtWs2812Show` fuller error handling | [backlog-light.md](backlog-light.md) | Dependent of the driver-task work |

**New backlog item this creates: Parlio chunked transfer** — split a lane's frame into ≤65535-byte transactions with correct WS2812 inter-chunk timing (no false latch), lifting Parlio's 65535-byte/lane (897 RGB) ceiling to the soft cap so 8×2048 (and later 16×2048) works. Prerequisite for the product owner's full 16K-over-Parlio claim.

## Recommendation → the build plan

The ordered implementation plan lives in the companion **[multicore-analysis-top-down.md](multicore-analysis-top-down.md)** (same bottom-up/top-down split as the LED-driver and live-script analyses). In brief, in dependency order:

1. **Driver correctness — DONE** (the two bugs above).
2. **Encode optimisation (one core)** — the ~24 ms transpose is the bottleneck; skip-when-unchanged → bit→slot lookup → SIMD. The prerequisite that unblocks or obviates the rest.
3. **Multicore (encode+transmit on core 1)** — only if a *measured* flicker/throughput ceiling justifies it after Step 2.
4. **Lane/byte ceiling raises** (16-lane, Parlio chunking, parallel-I2S) — independent axis, raises maxima not fps.

The top-down expands each: the encode schemes, the task/handoff primitive, the buffer-swap + structural-change race, and how Steps 3-4 fold into the existing [backlog-light](backlog-light.md) driver items.

## Appendix A — frame pacing, decided against

MoonLight targets a fixed 60 fps; projectMM deliberately does not (settled with the product owner 2026-07-12 while assessing PR #45):

- **Architecture is "render uncapped + effects are time-aware"** (`beatsin8`/`millis()`-driven), a CLAUDE.md hard rule. A whole-engine fps cap is redundant with that rule and papers over any effect that breaks it.
- **Higher fps is smoother, so a cap *reduces* quality** below the hardware ceiling.
- **The LED transmit already paces the render physically** (30 µs/light; the wire rate is the natural limiter, confirmed by the 16.27-fps-at-2048/lane math).
- **UI/WiFi responsiveness is already guaranteed by the per-tick `vTaskDelay(1)` yield** ([main.cpp:583](../../src/main.cpp)), not by frame-rate control. Skipping whole frames adds no yield points; a single long-blocking tick is the only real starvation risk, and pacing doesn't fix that either (a rendered frame blocks its full duration regardless).
- **Parked as a ~15-line opt-in** (`targetFps=0` default = unlimited, an elapsed-time gate) *only if* a genuinely CPU-starved device appears.

Multicore addresses the real ceiling (the CPU encode + WiFi-timing isolation); frame pacing addressed a problem the yield already solves. Unrelated; only the first is wanted.
