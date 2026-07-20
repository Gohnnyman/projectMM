# Plan — Shift encode: template the hot loop on compile-time counts (the 48×256 encode lever)

## Context

Lapping-v2 streams 256 lights/strand clean (committed d263c274), but the **48-strand** target still
misses: the ISR refill measured **466 µs** against a **181 µs** padded deadline — a ~3× capacity deficit
the pool/pad can't absorb (the plan named this exactly). hpwit's standing advice is the lever: *"fill out
all the ones you know / unwrap certain loops."* The prefill ("ones you know") already ships. This plan is
the **loop unroll**.

**Why the compiler can't unroll it today — measured against the code.** The hot encoder
`encodeWs2812ShiftData<Slot>` (ParallelSlots.h:449) is a 4-deep nest whose three outer bounds all arrive
as **runtime `uint8_t` arguments**, so the compiler must treat them as unbounded and keeps the loops
rolled (branch + counter per iteration, no register-resident accumulators):

- `for ch < channels` — 3 or 4 (RGB/RGBW), a runtime arg
- `for c < outPerPin` — **always 8 in expander mode** (`kPinExpanderOutputs`), yet passed as a runtime arg
- `for p < physPins` — the DATA-pin count: 2 on the bench, **6 at the target**, a runtime arg
- `for bit` — already a fixed `7..0` literal (the compiler unrolls this one)

The insight: **`outPerPin` is morally a constant** (`outputsPerPin()` returns `kPinExpanderOutputs`=8 or
1, ParallelLedDriver.h:247) — the compiler simply can't see it through the argument. Bringing
`outPerPin` (and ideally `channels`) to the type level lets the compiler unroll the `c`-loop's 8
iterations and the `ch`-loop, collapse `bitStride`/`pos`/shift math to constants, and keep the SWAR
accumulators in registers across the unrolled body. This is the same compile-time-specialization pattern
the code ALREADY uses for `Slot` width (`if (slotBytes()==1) encodeRows<uint8_t> else <uint16_t>`,
ParallelLedDriver.h:470) — extended one axis further, so it passes *Common patterns first* (a recognizable
template-dispatch, not bespoke).

**Measure-first is mandatory here** (this session burned real time on unmeasured encode work, and templates
*can* disappoint — GCC may already partially unroll, or code-size/icache may eat the win). The plan gates
implementation on a host microbenchmark showing a real delta BEFORE any device work.

## Design

### Phase 0 — HOST BENCHMARK FIRST (gate the whole plan)
Before templating anything, measure the current encoder and a hand-specialized copy on the host, at the
target shape (physPins=6, outPerPin=8, channels=3, the 16-bit bus Slot=uint16_t). A `test/` micro-bench
(or a throwaway `main`) that runs N encodes and times them. Decide from the delta:
- **< ~15%**: stop. The compiler already unrolls enough; the real lever is the 19.2 MHz clock (backlog).
  Report and close the plan — no device churn.
- **≥ ~15%**: proceed to Phase 1. Record the host number as the pre-registered expectation for the S3.

### Phase 1 — Template `outPerPin` (the highest-value axis)
Add a compile-time `OutPerPin` template parameter to `encodeWs2812ShiftData` (keeping the runtime-arg
overload as a thin forwarder for any non-hot caller), so the `c`-loop bound and all `outPerPin`-derived
constants (`bitStride`, `pos`, the pin-packing offsets) fold. Dispatch at the existing Slot-width branch
in the trampoline: expander mode is always `kPinExpanderOutputs`, so it's a single extra specialization,
not a combinatorial explosion — `<Slot, 8>` for expander, the runtime forwarder for direct mode
(outPerPin=1, cold path). The prefill sibling `prefillWs2812ShiftConstants` and `shiftActivePins` get the
same treatment only if the bench shows they matter (prefill is skipped on recycled buffers already).

### Phase 2 — Template `channels` only if Phase 1 leaves a gap
`channels` is 3 or 4. If Phase 1's re-measure still misses the 181 µs deadline, add a second parameter
(`<Slot, 8, 3>` / `<Slot, 8, 4>`) — 2 more instantiations. Guarded behind its own re-measure so we don't
pay code size for a win we don't need. `physPins` stays runtime: it varies 1..16 (too many instantiations)
and the `p`-loop is the cheap packing loop, not the transpose cost.

### The dispatch seam
The trampoline `MoonLedDriver::ringEncodeTrampoline` (MoonLedDriver.h:367, verified) already branches
`slotBytes()==1 ? <uint8_t> : <uint16_t>` — 4 more sites mirror it in ParallelLedDriver.h (tickSync 470,
tickAsync 502, reinit-prefill 693, loopback 1555). `encodeRows<Slot>` (767) and `prefillShiftRows` (724)
gain the compile-time `OutPerPin` and forward it to the two call sites (818, 744). Expander mode always
passes `kPinExpanderOutputs`=8; direct mode (outPerPin=1) uses the shift encoder's sibling
`encodeWs2812ParallelSlots`, NOT this path, so the runtime forwarder is only a safety fallback. Net:
**2 hot instantiations** (uint8/uint16 × outPerPin=8), maybe 4 if Phase 2 (channels) fires.

## Code grounding

- `src/light/drivers/ParallelSlots.h` — `encodeWs2812ShiftData` (449): add `<..., uint8_t OutPerPin>`,
  replace the `outPerPin` arg uses with the template constant, keep a runtime-arg forwarder overload.
  Sibling `prefillWs2812ShiftConstants` (414) / `shiftActivePins` (399) only if Phase 1 bench says so.
- `src/light/drivers/ParallelLedDriver.h` — `encodeRows` (767) and `prefillShiftRows` (724): thread the
  compile-time OutPerPin; the two call sites at 818 and 744. `outputsPerPin()` (247) is the value source.
- `src/light/drivers/MoonLedDriver.h` — `ringEncodeTrampoline` (~316): the dispatch (expander → `<Slot,8>`,
  direct → runtime forwarder). Whole-frame `encodeFrame` (ParallelLedDriver.h:470) mirrors.
- `test/unit/light/unit_ParallelSlots.cpp` — the byte-for-byte pins. The existing `sweep()` case (618)
  ALREADY runs `encodeWs2812ShiftData` vs the reference `encodeWs2812ShiftSlots` across **physPins 1..8 ×
  both Slot widths × 3 mask patterns** — so a physPins/OutPerPin template is pinned the moment its dispatch
  reaches that sweep (make the sweep instantiate the templated form). But it fixes **channels=3** and
  **outPerPin=8** — so Phase 2 (channels template) needs the sweep extended to channels ∈ {3,4}. The
  prefill-equals-whole-slot pins (543, 579) cover the prefill sibling. This existing coverage is why the
  unroll is safe: the template must reproduce every swept byte identically or the case fails.

## Verification

1. **Host bench (Phase 0 gate)**: current vs specialized encode µs at (6,8,3,uint16). Proceed only on ≥15%.
2. **Host ctest**: the new byte-identity case + all 17 existing shift cases green (the unroll changes
   NOTHING observable — same bytes, faster).
3. **S3 build** (nm-verify the templated encoders still land in IRAM at 0x4037xxxx — templates + MM_RAMFUNC
   need the litpool flag, already set).
4. **Bench re-measure** on shiffy: `enc` at the 2-pin 256/strand config (expect the host %); then the true
   48-strand shape (6 pins, all 12288 lights) — does worst `enc` drop under the padded deadline, does the
   wall render clean, and the definitive **fps vs 100** number. `late`=0 is the machine gate; PO eyes final.
5. If 48-strand still misses after Phase 1+2: the 19.2 MHz clock is the named next lever (its own plan) —
   report the residual gap, don't chase it here.

## Out of scope (named)
- 19.2 MHz PLL shift clock (the budget lever, not the encode lever) — separate plan if this misses.
- Assembler transpose kernel (3 ISAs = 3 bespoke versions; last resort, only if templating + clock miss).
- The ~5 s white-flash residual (its own instrument/hunt: the intrusive-loopback soak).
- `physPins` templating (too many instantiations; the p-loop isn't the cost).

## Outcome — GATE FAILED, plan closed (2026-07-18)

Phase 0 host benchmark (`scratchpad/bench_encode.cpp`, target shape physPins=6/outPerPin=8/channels=3/
uint16, byte-identical to the shipped encoder): templating `outPerPin` as a compile-time constant is
**−35% at -O2 (SLOWER) and +1.2% at -O3 (a wash)** — far under the 15% proceed bar on the host clang
proxy. The compiler already extracts everything the unroll would; specializing perturbs its heuristic and
at -O2 hurts. **The encode is not the closable lever.**

Two consequences:
- **The channels axis stays a runtime loop** — which is also the answer to "how do we support RGBCCT / an
  arbitrary N-channel fixture": the generic design keeps `for ch < channels` runtime, no per-channel
  instantiation, no cap on generality, and (per this bench) no speed cost vs specializing.
- **The 48-strand path is the 19.2 MHz shift clock** (budget lever, +~78 µs/slice), its own plan — NOT
  the encode. hpwit's "unroll loops" advice doesn't transfer: his loops have compile-time bounds where
  ours has the runtime channel count the fixture generality requires.

Kept: nothing shipped (measure-first gate; no code touched). The bench is scratch, deleted with the
session. This is a clean example of the gate working — a template that would have been neutral-to-harmful
was caught before any device churn.
