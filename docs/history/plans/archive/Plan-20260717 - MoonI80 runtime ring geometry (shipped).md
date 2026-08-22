# Plan — MoonI80 ring: runtime geometry (`ringRows`), rebuilt clean, ragged-safe

## Context

**The goal, unchanged: 48 pin-expanded strands × 256 lights at 100 fps.** The ring is the only route — a 256-light frame is 144 KB contiguous internal, which does not exist (measured `maxBlock: 31744`), and the LCD DMA cannot read PSRAM at the 26.67 MHz expander clock (measured: same frame internal works, in PSRAM gives "no LED output").

**What the first attempt (`da67edf9`, reverted) proved.** A per-light ring (`kRingRows=1`) **works and streams**: RAM went 147 KB → **18 KB, constant at any strand length**, with real buffer reuse and no descriptor errors. What it did *not* do is meet the deadline: **46 µs encode against a 21.6 µs wire**. It was reverted because it hard-coded `kRingRows = 1` and deleted the sliced geometry, so the two could not be compared.

**Three findings reshape this rebuild:**

1. **Geometry is not a mode, it is a number.** `rowsPerBuf` is already a runtime member (`platform_esp32_moon_i80.cpp:217`); the ISR, the prime loop and `nSlices` (`:882`) all read `st->rowsPerBuf`, never the constant. `kRingRows` has four trivial uses. **7 rows/buffer is exactly as easy as 1 or 16** — so the control is an integer, not a two-valued enum (which would be bespoke per *Common patterns first*).
2. **The optimum is unknown and must be measured.** RAM and per-call overhead move in opposite directions: at 1 row the ring is 18 KB flat but pays the per-call fixed cost (the `slotBytes()`/`pinExpanderMode()` branches, two calls, two loop setups) **every light, inside the ISR, at 16× the EOF rate**; at 16 rows that cost amortizes but RAM scales with strand length and caps the driver near 240. Against a 21.6 µs budget the overhead is not negligible — **a middle value may beat both ends**, and only a bench sweep can say.
3. **The ragged machinery already exists and is correct.** `prefillShiftRows` (`ParallelLedDriver.h:686-702`) already splits rows into RUNS sharing an active mask, and the *encoder-level* ragged tests are good (`unit_ParallelSlots.cpp:397/579/642`). `da67edf9`'s trampoline simply bypassed it by passing row 0's mask — exact only for uniform strands, and it would drive an exhausted strand HIGH (flash white at full brightness). **Every driver-level ring test is uniform** (`wireShift` never sets `ledsPerPin`), which is why it shipped.

**Outcome:** one ring whose geometry is a control, so the 1-vs-7-vs-16 question is answered on the bench instead of in a commit message — with the ragged path correct and pinned.

## Design

**`ringRows` (1..64) and `ringBufs` (2..32) become number controls on `MoonLedDriver`**, alongside `forceRing`. RAM = `ringRows × ringBufs × rowBytes`, shown in `ringDbg`. Both are `prepare` triggers — a geometry change is already a full bus rebuild (`ParallelLedDriver.h:1207-1229` does `deinit()` then `busInitRing()`), exactly like `forceRing` today.

### The four trade-offs (why the optimum can only be measured)

RAM is the only axis that favours a SMALL `ringRows`; every other axis favours a big one. That tension IS the design problem:

| axis | favours | detail |
|---|---|---|
| **RAM** | **small** | `ringRows × ringBufs × rowBytes` — constant in strand length for ANY fixed `ringRows`/`ringBufs` (the pool size is absolute, not per-light). This plan initially reached for `ringRows=1` to minimize that constant, but the near-prime pool that shipped keeps RAM flat at a larger, safer `ringRows`; the per-light `ringRows=1` branch was tried and abandoned (see the lean-rows=1 plan). Smaller pool = less RAM, at the cost of the runway/overhead/interrupt axes below. |
| **Per-call overhead** | big | Fixed cost per `encode` call (`slotBytes()`/`pinExpanderMode()` branches, 2 calls, 2 loop setups) amortizes over `ringRows`. At 1 it is paid **every light, inside the ISR**. |
| **Interrupt rate** | big | One EOF per buffer → `lights/ringRows` interrupts per frame. At 1 row, 256 lights, 100 fps = **25,600 int/s**; at 16 rows, 1,600. Measured precedent: a ~19 ms core-0 encode starved the W5500 ethernet on the LC16 (HTTP died, render ticks fine). |
| **Lap-time runway** | big | Runway before the DMA laps a buffer the ISR is still refilling = `ringRows × ringBufs × 21.6 µs`. At 1×32 ≈ **690 µs**; at 16×12 ≈ **4.1 ms**. A per-light ring is far less forgiving of a WiFi preemption — which is why `da67edf9` needed `ringBufs=32`. |

**So the per-light ring is not "better" — it is the only geometry whose RAM is flat.** If the sweep shows 7 or 8 meets the 21.6 µs deadline at 256 lights *and* fits, that beats 1 on three axes out of four.

**Above 16 is legal and worth sweeping** (hence 1..64, not 1..16): nothing in the code caps it, and at shorter strands a big `ringRows` buys interrupt rate and runway cheaply. The real floor is the other end — `nSlices = ceil(totalRows / ringRows)` must be enough slices to be a ring at all; at 2 slices it is a whole frame in two pieces (which is what `bf16` was silently doing).

**`forceRing` drops AUTO** → `{ring, wholeFrame}`, default `ring`. AUTO's question ("does the whole frame fit internal?") has one right answer at 48×256 (it never does), so it was a decision dressed as a choice — and it made the fallback invisible. Values shift, so this is a `MIGRATING.md` note.

**Ring geometry becomes parameters, not constants** — per the agent's classification, Option B (fixed `kRingBufsMax` array bound + runtime `ringBufs` count): keeps the ISR's `st->ring[slot]` a single load, and the free loop's existing null guard already tolerates a half-built ring.

## Steps

1. **Save this plan** to `docs/history/plans/Plan-20260717 - MoonI80 runtime ring geometry.md` (per CLAUDE.md).

2. **Platform: geometry as parameters** (`src/platform/esp32/platform_esp32_moon_i80.cpp`, `src/platform/platform.h`)
   - `moonI80Ws2812InitRing` gains `rowsPerBuf` + `ringBufs` params (`platform.h:777-780`; `MoonLedDriver.h:182` is the only caller).
   - `:216` → `uint8_t* ring[kRingBufsMax]` + a runtime `uint8_t ringBufs` member. `kRingRows`/`kRingBufs` constants go; `:927/:930/:941/:955` read the members, `:1022-1024` (fit check, pre-`st`) reads the params.
   - Loop bounds `:803/:898/:942/:958/:959` and the ISR modulus `:345` → `st->ringBufs`. Free loop `:407` → indexed over the full array bound, keeping the null guard. Stats `:1152` → `st->ringBufs`.
   - **Delete the stale comment block at `:835-849`** — it describes a linear self-terminating chain and argues *against* looping; the code implements the looping chain described at `:884-895` (`GDMA_FINAL_LINK_TO_HEAD`, `:964`). It is the first thing a reader hits. (*Default to subtraction*.)

3. **Domain: the controls** (`src/light/drivers/MoonLedDriver.h`)
   - `addNumber("ringRows", …, 1, 64)` + `addNumber("ringBufs", …, 2, 32)`, hidden unless `pinExpanderMode()`, both in `busControlTriggersBuild`.
   - `kForceRingOptions` → `{"ring", "wholeFrame"}`; `wantsRing()` loses the AUTO branch.
   - `busInitRing` passes the geometry through.

4. **Fix the ragged prefill properly** (`MoonLedDriver.h` trampoline)
   - Keep prefill-per-slice calling the **existing** `prefillShiftRows` run-splitting (correct at any `ringRows`, including a buffer that straddles a strand's end). Do **not** reintroduce `da67edf9`'s row-0-mask shortcut.
   - The per-light prefill cost (384 constant stores vs 192 data at `ringRows=1`) is what the frame-arm prefill seam existed to fix. **Re-measure before adding that seam back** — it is an optimization, and its benefit depends on `ringRows`, which is now a knob. Only add it if the sweep says the constants dominate. (*Concrete first*; the decomposition's own lesson: a per-slice cost becomes a per-light cost when the slice IS a light.)

5. **Tests** (`test/unit/light/unit_ParallelLedDriver_ring.cpp`)
   - Parameterize the mock's `kMockRingRows`/`kMockRingBufs`; keep a multi-row case (a 1-row slice cannot express a tiling bug) **and** add `rowsPerBuf == 1`.
   - **Add a `wireShift` overload taking `ledsPerPin`** and ragged ring tests where **a strand ends mid-buffer** — the untested interaction (run-splitting × slice tiling), and the one that goes from rare at 16 rows to *every buffer* at 1.
   - Fix the mock's stale `:33` comment ("the platform's kRingBufs (8)" — it is 12, and about to be a variable).

## Verification

- **Host:** `cmake --build build` clean, `ctest` (the 26 ring/slots tests pin sliced==whole-frame, recycled==fresh, ragged), scenarios. The ragged tests must **fail on `da67edf9`'s row-0-mask trampoline** — if they pass on it, they are not testing the bug.
- **Bench (shiffy, S3-N16R8, 192.168.1.150):** flash, then **sweep `ringRows` 1 / 2 / 4 / 7 / 8 / 16 / 32 at a fixed light count**, reading `ringDbg` (`enc` = worst ISR refill µs, `gap` = worst EOF-to-EOF µs) and `frameTime`. The deadline is **21.6 µs/light**. This sweep is the deliverable: it answers "what is the optimum" with numbers.
- **Then ragged on the wall:** an unequal `ledsPerPin` (e.g. two strands on one '595 at different lengths) — the exhausted strand must go **dark**, not white.
- **The gate is the PO's eyes.** Report what the instrument says and hand it over; do not self-certify. (`frameTime` is the real frame; the header's fps is the module TICK rate and lies — known, listed below.)

## Risks / notes

- **A per-light ring is necessarily a deep-reuse configuration** (`nSlices == totalRows`, so "no reuse" would need `ringBufs > totalRows`). The comments at `:137-158` call deep reuse unproven and name `GDMA_FINAL_LINK_TO_NULL` (self-terminating) as the "real fix". `da67edf9` ran 160 ISR refills/frame with `descErr=0`, which is evidence reuse works — but it is the main structural risk.
- **The fit check uses total-free, not largest-block** (`:1024`) — correct for N small allocations, but it ignores per-block heap overhead (~8–12 B), which is a real fraction of a 576 B buffer at `ringRows=1`. Watch it if the sweep goes to many tiny buffers.
- **Not in this change** (tracked, not lost): the encode's remaining 46 → 21.6 µs gap (the emit loop; the correction pass), the lying fps header, `_DMA_EXTENSTION` (costs the RAM we lack — see `docs/history/shift-register-driver-analysis.md` § 7.6).
