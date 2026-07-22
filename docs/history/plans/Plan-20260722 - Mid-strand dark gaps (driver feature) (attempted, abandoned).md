# Plan: Mid-strand dark gaps — a shared driver feature

## Context

A user needs to drive a continuous LED strand that has **dark segments between lit ones** (e.g. one data pin: 250 lit, 50 dark, 250 lit) where the strand is fixed/sealed and the WS2812 data must flow THROUGH the dark LEDs to reach the lit ones beyond them. Cutting the LEDs isn't an option (a commercial/potted fixture), and the existing `start`/`count` window can only select ONE contiguous subset per driver — not per-pin dark runs mid-strand.

This is a **general** capability (any fixed continuous strand with mid-run dark LEDs — signage, slat walls, repurposed panels), not one user's wiring. The backlog item `docs/backlog/backlog-light.md § "Mid-strand dark gaps (slat wall)"` already settled the analysis: it is a DRIVER feature (a layout emits only real lights; the gap lives at the driver's wire-position→light-index seam), and the only case needing code is the continuous-strand-through-the-gap one.

**Original user request (cross-checked):** the user asked for a *"spacer layout"* on their **P4** slat wall — "Pin 1 drives Column 1 0-250 leds and column 2 is 300-550 with 251-299 remaining black." That is EXACTLY the single-pin continuous-strand-through-the-gap case: one pin, 550 wire positions, lit 0-250 + dark 251-299 + lit 300-550 → our `ledsPerPin = "250+49g+251"` (or similar). Two consequences: (a) the P4 uses `ParlioLedDriver`, which IS `ParallelLedDriver<ParlioLedDriver>` (CRTP) and shares `encodeRows`/`prefillShiftRows` — so the ParallelLedDriver scope covers the user's actual hardware directly; (b) the user's mental model is a *layout* they pick, but we deliver a *driver control* (`ledsPerPin` gap) — a discoverability gap the docs + the user-facing reply must close (tell them: not a layout, it's the `ledsPerPin` gap syntax on the Parlio driver).

The insight from design discussion: the gap concept is NOT lane-specific. `assignCounts` (`src/light/drivers/PinList.h`) is the SHARED distribution primitive that ParallelLedDriver, RmtLedDriver, and NetworkSendDriver (`lightsPerIp`) all use identically — each output owns a contiguous source slice `[offset, offset+counts[i])`. So the gap belongs in that one shared primitive, and every driver inherits it (*Complexity lives in core*).

## Design (settled with the user)

- **Syntax:** in `ledsPerPin` / `lightsPerIp`, a `g`-suffixed run is a gap (dark, addresses no source light). Within-output delimiter `+`, between-output `,`. Example: `"250+50g+250, 300"` = output0 is lit-250 / dark-50 / lit-250, output1 is a plain 300. A gap-free spec is fully backward-compatible (one lit segment).
- **Gap semantics = black/zero on the wire:** LED drivers emit the existing idle-LOW/dark word for gap positions (data still clocks through); NetworkSendDriver packs zero (black) for gap channels, so the receiver's addressing stays contiguous.
- **Hot-path guarantee (load-bearing):** gap resolution happens in `prepare()` (cold path), folding into FLAT precomputed per-lane/per-output arrays — NEVER a per-light segment-list walk in the render loop. The gap-free case (the overwhelming majority) must be PROVABLY unchanged, not just "cheap": the whole gap logic is gated behind `if (laneGapN_[lane])` (0 for a gap-free lane), so a gap-free lane runs one always-not-taken branch (predicted, ~0) and `srcRow == row` exactly as today. Only a lane that actually declares gaps pays the ≤`kMaxGapsPerOutput` bounded resolution. Enforced by: (a) a unit test asserting a gap-free config produces a BYTE-IDENTICAL frame to before, and (b) a gap-free encode-µs A/B on the wall (verification #1) — any measurable regression on the gap-free path is a bug to fix, not accept.

## Findings from exploration (what's shared vs per-driver)

`assignCounts` (`src/light/drivers/PinList.h:61`) is the shared distribution primitive used by FOUR drivers (ParallelLedDriver — the CRTP base for MoonI80/Parlio/MultiPin — plus RmtLedDriver and NetworkSendDriver). All accumulate a contiguous offset so output *i* owns `[offset, offset+counts[i])`. But only the **parse/distribution** is shared; each driver's **gather** differs and none has any concept of "a wire position that emits black but consumes no source light":

- **ParallelLedDriver** (`encodeRows`, ~line 840-853): per-lane row-major. Source index = `winStart_ + laneStart_[lane] + row`; the existing short-strand skip `if (row >= laneCounts_[lane]) continue;` (line 845) ALREADY emits an idle-LOW/dark word (mask bit unset + zeroed `wire`). A mid-strand gap is that same mechanism at a different row range. `prefillShiftRows` (line 767-787) also reads `laneCounts_` per run and must treat gap edges as run boundaries. Per-lane state is fixed member arrays `laneCounts_[kMaxStrands]`/`laneStart_[kMaxStrands]` (line 1006-7); populated in `parseConfig` (line 1391-1409, cold path). `maxLaneLights_` (line 1408) must be fed the gap-INFLATED wire length (lit+gap).
- **RmtLedDriver** (~line 250-279): flat encode, source-index==symbol-index locked; the `pinStart = pinOffset/wordsPerLight` fast-path (line 272) breaks once gaps decouple the two. (Deferred — see Scope.)
- **NetworkSendDriver** (~line 283-322): byte-cursor packing straight from `data`. Gap = pack zero for gap channels. (Deferred — see Scope.)

Key inversion: today every advanced source light IS emitted; a gap advances the WIRE cursor while NOT advancing the source cursor. That's the one new idea each gather must learn.

Existing tests: `assignCounts` is tested via `test/unit/light/unit_RmtLedDriver_pins.cpp` (the `--- assignCounts ---` section, ~line 134-259) — no dedicated PinList test file. ParallelLedDriver has `unit_ParallelLedDriver_*` / `unit_MultiPinLedDriver.cpp`.

## Scope (this pass) — decided with the user

Concrete-first: **the shared parser gets the gap now (all drivers inherit the parsed gaps), but only ParallelLedDriver's gather is implemented** — it's the driver the backlog named, and its short-strand skip already IS the dark-emit mechanism. RMT/Network gathers are follow-ups (they'll already receive the parsed gaps; only their small "emit black" edit remains). N gaps per output are supported (a panel on one pin may gap more than once).

## Implementation

### 1. Shared parse layer — `src/light/drivers/PinList.h`

`assignCounts` today fills `counts[i]` (lit lights per output). Add a parallel **gap descriptor** output so a gap-aware caller gets, per output, the bounded set of gap boundaries; a gap-free caller keeps using `counts[]` unchanged (full backward compatibility).

- **Syntax:** a `g`-suffixed run is a gap; within-output delimiter `+`, between-output `,`. `"250+50g+250, 300"` = output0: lit-250 / dark-50 / lit-250; output1: plain 300. A bare number / list / broadcast (no `g`, no `+`) parses exactly as today.
- **Output shape (flat, bounded, no heap):** alongside `counts[i]` (= total LIT lights for output i, what downstream offset-accumulation still uses), emit a small fixed per-output structure describing the gaps as WIRE-relative boundaries — e.g. `struct GapRuns { uint8_t n; nrOfLightsType at[kMaxGapsPerOutput]; nrOfLightsType len[kMaxGapsPerOutput]; }` with a small cap (`kMaxGapsPerOutput`, e.g. 4). `at[k]` = wire row where gap k begins (in this output's local wire coordinates), `len[k]` = dark length. The parser is the ONE place `+`/`g` is understood.
- Keep the existing broadcast/even-split/list cases and the `maxPerPin` clamp working; gaps only appear when the `+`/`g` tokens are present. Add a new overload (or an optional out-param) so the 3 non-Parallel callers compile unchanged until they opt in.

### 2. ParallelLedDriver gather — `src/light/drivers/ParallelLedDriver.h`

- **parseConfig (cold path, ~1391-1409):** after `assignCounts`, store the per-lane gap runs into fixed member arrays (mirroring `laneCounts_`/`laneStart_`, e.g. `laneGapAt_[kMaxStrands][kMaxGapsPerOutput]`, `laneGapLen_[...]`, `laneGapN_[kMaxStrands]`). Compute each lane's **wire length** = `laneCounts_[lane] + Σ gap len`, and feed the MAX wire length into `maxLaneLights_` (so gap rows are clocked) and `frameBytes_`. `laneStart_` stays the SOURCE offset (unchanged: gaps consume no source).
- **Per-lane "is row lit, and its source index" helper (hot path):** replace the bare `row < laneCounts_[lane]` test with a small inline that, given `row` (WIRE position) and the lane's gap runs, returns {lit?, sourceIndex}. For the common no-gap lane it's identical to today (one compare). For a gapped lane it's ≤`kMaxGapsPerOutput` compares — bounded, no per-light walk. sourceIndex = `winStart_ + laneStart_[lane] + (row − gapLenBefore(row))`.
- **encodeRows (~840-853):** use the helper: a lit row sets the mask bit + gathers from sourceIndex; a gap row (or past-end) does the existing `continue` (mask bit stays clear → dark word).
- **prefillShiftRows (~767-787):** the run-boundary loop already jumps `row = runEnd`; extend `runEnd` to also stop at the nearest gap edge across lanes, and the per-lane active test to the same helper, so gap rows lay down a mask-clear (dark) constant. This is the one spot that's more than a drop-in of the short-strand skip (gaps add mid-lane run boundaries the current `runEnd` doesn't model).

### 3. Tests

- **Parser** (extend `unit_RmtLedDriver_pins.cpp` `--- assignCounts ---`, or a new `unit_PinList.cpp`): gap token parse (`250+50g+250`), multiple gaps (`100+20g+100+20g+100`), `+`/`,` interplay, backward-compat (a plain list/broadcast yields zero gaps), bounds (over `kMaxGapsPerOutput`), malformed (`50g` alone, trailing `+`, bad tokens).
- **ParallelLedDriver gather** (extend `unit_ParallelLedDriver_*`): a lit/gap/lit lane produces the right wire pattern — the gap rows emit the dark word (mask bit clear), the post-gap lit rows read the CORRECT source index (source not advanced across the gap), and the frame is sized to the inflated wire length. A multi-gap lane. A gap-free config is byte-identical to before (regression pin).

### 4. Docs + backlog

- Document the `g`/`+` syntax on the `ledsPerPin` control (its `///` + the drivers doc card), **explicitly framed for the "spacer" mental model**: the user expects a layout; the docs must say "a mid-strand dark gap is a driver control, not a layout — set `ledsPerPin = 250+50g+250`," so a user searching for a spacer finds it.
- Update `docs/backlog/backlog-light.md § "Mid-strand dark gaps"`: mark the ParallelLedDriver reference SHIPPED (covers the P4/Parlio slat-wall case directly), keep a slim follow-up for the RMT + Network gathers (they inherit the parsed gaps; only their emit-black edit remains).

## Verification

1. **No hot-path regression (load-bearing):** build + flash the wall/bench; A/B a GAP-FREE config's encode µs (48×256, `lt=0` clean) before vs after — must be unchanged. The KPI tick timing is the guard.
2. **Unit:** `cmake --build build --target mm_tests && ./build/test/mm_tests -tc="*PinList*,*assignCounts*,*ParallelLedDriver*"` all green; full suite green.
3. **On hardware:** flash a board, set `ledsPerPin = "N+Mg+N"` on a real strand, confirm the middle M LEDs stay dark while the strand continues lit past them (data flows through), and the source lights map correctly (the effect isn't shifted). Invite the product owner to look — LEDs on a bench are the measurement.
4. Spec check + platform-boundary + scenario gates green (no platform code outside `src/platform/`; this is all `src/light/`).
