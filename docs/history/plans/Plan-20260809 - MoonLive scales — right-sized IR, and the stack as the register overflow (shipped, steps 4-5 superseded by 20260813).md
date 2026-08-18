# Plan: MoonLive scales — right-sized IR, and the stack as the register overflow

> **Steps 1–3 shipped. Steps 4–5 (the register allocator) are SUPERSEDED by
> [Plan-20260813 — MoonLive on a stack machine](Plan-20260813%20-%20MoonLive%20on%20a%20stack%20machine%20%E2%80%94%20the%20frame%20is%20where%20values%20live%20(shipped).md).**
> The allocator was built and works on the host at every budget, but on Xtensa it leaves ZERO
> allocatable registers (10 − 1 scratch − 5 ABI vregs − 4 reload temps), so every looped script is
> refused there. Bench-measured on an S3. The successor plan puts every variable in the frame
> instead and keeps registers for expression temporaries only.

## Context

MoonLive scripts hit hard walls far below what a user would call a complex script. Two separate
ceilings, both measured on the host, neither obvious from the error text (every one reports
`codegen failed (unsupported on this target, or too large)`):

- **Seven `addLight` statements in a row fail.** `kMaxIrOps = 64` ([MoonLiveIr.h:33](src/core/moonlive/MoonLiveIr.h#L33))
  and a call costs ~9 IR ops. No nesting, no register pressure — this is the wall a user meets first.
- **Nested `for` loops are refused on Xtensa.** Measured: `LOWER BAIL: vregsUsed=11 +2 > kRegCount=12`
  ([moonlive_lower_xtensa.cpp:24](src/platform/esp32/moonlive_lower_xtensa.cpp#L24)). They work on
  desktop and RISC-V, which have larger register maps. The shipped default layout script `grid.mlv`
  is a nested loop, so the module's own default cannot compile on the smallest target.

The goal is that a script's complexity is bounded by memory the device actually has, not by
constants chosen when a script was one statement. That means two changes, and they are
independent: **right-size the IR** (removes the statement wall) and **spill to the stack**
(removes the register wall). Neither alone is enough — shipping only the spiller leaves the
7-statement wall, which is the one users hit first.

Spilling is also the industry-standard answer: values that outlive the register file live in the
frame. It is what every real compiler does, and it is the mechanism that makes "how complex can a
script be" a memory question instead of a register-count question.

## Every ceiling, and what happens to it

Seven fixed constants bound a script. They are **not** one problem: what each costs, and where its
storage lives, decides the treatment. Measured sizes:

| Ceiling | Value | Limits | Where it lives | Treatment |
|---|---|---|---|---|
| `kMaxIrOps` | 64 | total instructions | `IrProgram` = **2056 B stack local** ([MoonLiveCompiler.cpp:512](src/core/moonlive/MoonLiveCompiler.cpp#L512)) | **right-size on the heap** — the wall users hit first |
| `kCodeCap` | 768 B | emitted machine code | `buf_[kCap]` inside the assembler, itself a **1368 B stack local** ([moonlive_lower_xtensa.cpp:28](src/platform/esp32/moonlive_lower_xtensa.cpp#L28)) | **right-size on the heap**, same mechanism |
| `kMaxVRegs` | 16 | values a program can name | index width in `IrInst` | **raise to 32** once spilling makes >16 usable; `IrProgram::push` keeps validating |
| `kMaxFixups` | 32 | branches | assembler member | **right-size** with the code buffer (same owner, same lifetime) |
| `kIrLabels`/`kMaxLabels` | 16 | ~8 loops | IR + assembler members | **right-size** with the op array; the estimator counts `for` tokens |
| `locals[4]` | 4 | loop nesting depth | Parser, stack | **raise to 8**, 12 B — a fixed bump, not worth an allocation |
| `kMaxCtrls` | 8 | script-declared controls | 128 B in Parser + the binding's name pool | **raise to 16**; bounded by UI sanity, not by memory. Note the *binding* mirrors this in a fixed name pool, so both move together |

**The constraint that drives this:** `CONFIG_ESP_MAIN_TASK_STACK_SIZE = 12288`
([sdkconfig.defaults:8](esp32/sdkconfig.defaults#L8)), and the compile path already burns **~3.4 KB**
of it (`IrProgram` 2056 + assembler 1368, both live at once). Naively raising `kMaxIrOps` to 256
makes `IrProgram` alone 8 KB of *stack* — a bootloop, not a fix. This project has already lost a P4
to a large stack frame. So the two big arrays move to the heap and are sized to the script; the small
ones are simply raised, because 12 B or 128 B does not need an allocator.

## The end state (PO, 2026-08-11)

MoonLive is **bounded by memory, not by registers or fixed arrays** — a fairly complete language,
large scripts, nice effects. Three goals, and every step towards them is judged against the
**classic ESP32**: 320 KB internal, no PSRAM, so anything assuming plentiful RAM fails there first.
(The classic is not structurally blocked — its Xtensa backend compiles and its exec heap is enabled;
what stopped it was a crash, parked separately.)

1. **Spilling** — register allocation with spilling to the stack, so a script is never refused for
   naming more live values than the ISA has registers. Linear-scan (Poletto & Sarkar), designed
   below.
2. **Scripts on the filesystem** — the source lives on LittleFS, not in a fixed per-module array.
3. **Classic ESP32 runnability** — the yardstick for all of the above.

### Decided: a heap buffer, not a streaming lexer

Goal 2 could load the script into a right-sized heap buffer for the compile and free it after, or
stream it from the file so no buffer exists at all. **Heap buffer**, for three reasons:

- **The waste is the fixed array, not the transient buffer.** `source_` + `compiled_` + names is
  **2240 B per module, always resident — 13.1 KB across six modules, 4.1% of a classic's internal
  RAM, held whether or not a script is loaded.** A compile-time buffer is proportional to the script
  and freed immediately; the fixed arrays are permanent and mostly empty. Removing them is the win.
- **Seeking a file is not simpler than seeking RAM.** LittleFS does wear-levelling and block
  caching, so `parseFor`'s backward re-lex of the step clause could hit flash mid-compile —
  unpredictable latency in place of a pointer decrement.
- **Streaming needs compiler surgery first.** `parseFor` re-lexes the step from a saved source
  pointer after emitting the body, and `DeclaredControl::name` points INTO the source and outlives
  the compile (13 sites hold such pointers). Both are fixable, neither is a lexer swap.

Streaming stays possible later; it is an optimisation of a transient allocation, not the thing that
makes large scripts fit.

**Two consequences:** `kMaxScriptBytes` stops being a ceiling (a script is bounded by heap), and
`compiled_` — which only answers "did the source change" — becomes a hash rather than a second full
copy, removing another 1 KB per binding. FNV-1a is already the project's idiom for that.

### Sequence

Each step makes the next cheaper:

1. ✅ **Right-size `IrProgram`** (2026-08-11). The op array is heap-allocated and sized from a
   token count before parsing; `IrProgram` owns it RAII (destructor frees, copy deleted), so there
   is no manual free path to miss — unlike the reverted `32026eb5`, whose four independently-
   nullable tables produced the heap corruption its own comment records. `kMaxIrOps` 64 → 4096 is
   now a sanity bound, not the working limit: **7 sequential statements used to fail, 40 compile**,
   and ~2 KB moved off the 12 KB main-task stack.

   **Found while verifying:** widening `count` to `uint16_t` left four `uint8_t` loop counters
   iterating over it — three lowerers plus `IrProgram::hasInline` — which wrapped at 256 ops and
   spun forever. On a device that is a watchdog reset from a script that merely got long. Bisected
   (60 statements fine, 80 hung), fixed, and pinned by a test that HANGS when the fix is reverted.
   The first version of that test passed either way: repeated statements hit the code-buffer
   ceiling before reaching the wrap, so it needed a long arithmetic chain instead — many cheap ops,
   little emitted code.

   **Still standing:** `kCodeCap` is a separate ceiling and now the binding one (40 statements
   exceed it on Xtensa), as are `kIrLabels`, `locals[4]` and `kMaxVRegs`.
2. **Scripts on the filesystem.** Independent of the compiler work — different files, different
   risk — and the step that most helps the classic.
3. **Spilling.** By then it has stack headroom and no 1 KB source ceiling to fight. Landing it first
   would put the hardest algorithm on the tightest stack budget, where an overrun reads as a
   bootloop rather than a compiler bug.

## Decisions taken

- **Allocate to fit the script, not to `kMax`.** The op array and the code buffer move to a
  right-sized `platform::alloc` ([platform.h:55](src/platform/platform.h#L55)), sized from a cheap
  pre-pass over the source and freed when compilation ends. A one-statement script pays for one
  statement instead of 3.4 KB, so this *reduces* peak memory for the common case while removing the
  ceiling for the rare one. Compilation is cold-path, so an allocation there costs nothing that
  matters.
- **Raise the cheap ceilings rather than allocating them.** `locals`, `kMaxCtrls` and `kMaxVRegs` are
  tens of bytes. Subjecting them to an allocator would add machinery that buys nothing — the standard
  construct is only worth it where the size actually varies.
- **The spill algorithm lives in core, once.** Correct spilling across a loop back-edge is the
  hardest logic here, and only the arm64 backend is ever executed by tests — three copies would
  leave two permanently under-tested. Backends supply their register count and consume two new IR
  ops. (CLAUDE.md Principle 3: core owns the hard constructs, written once.)
- **Spill everywhere, not interval splitting.** Once a value is chosen for spilling it is spilled
  for its whole lifetime: every def stores, every use reloads. Splitting halves the reload traffic
  but the correctness argument across a back-edge is exactly the part that goes wrong. The simple
  form is provably safe and these loops are cold-path.
- **Fix the RISC-V scratch aliasing first, on its own.** Verified bug, latent today: `kScratchFn = 16`
  ([moonlive_asm_riscv.cpp:20](src/platform/esp32/moonlive_asm_riscv.cpp#L20)) is x16/a6, but
  `kRvReg[12] == 16` — so it *is* vreg R12. In `call()`, `mv a6, a0` stashes the result, the restore
  loop reloads x16 from frame offset 48 and destroys it, then `mv dst, a6` returns R12's stale value.
  It only bites when `vregsUsed > 12`, which is precisely what this work causes. Landing it inside
  the feature would make the first hardware symptom look like "the new spiller broke calls".
- **Not in scope: narrowing `call()`'s save-sets.** The spill pass computes exactly the
  live-across-call mask that would shrink RISC-V's 18-register and arm64's 14-register unconditional
  saves. Deliberately deferred: an over-long interval only costs an unnecessary spill (fail-safe),
  whereas a register wrongly omitted from a save-set corrupts a value (fail-dangerous) from the same
  analysis. Ship the safe consumer first, backlog the other by name.

## Design

### 1. Right-sized IR and code buffer (removes the 7-statement wall)

`IrProgram` gains a heap op array instead of `IrInst ops[kMaxIrOps]`:

```cpp
struct IrProgram {
    IrInst*  ops = nullptr;      // platform::alloc'd to fit; freed in the destructor
    uint16_t cap = 0;            // what was allocated
    uint16_t count = 0;
    VReg     vregsUsed = kFirstTemp;
    bool reserve(uint16_t ops);  // false on alloc failure — degrade, never crash
};
```

The **assembler gets the same treatment**: `buf_[kCap]`, `labelPos_[]` and `fixups_[]` become one
right-sized allocation with the same lifetime. This is the other 1368 B of stack, and `kCodeCap` is
a ceiling in its own right — a long script overflows the code buffer even when its IR fits.

**Sizing.** One cheap pre-pass over the token stream counts statements, call arguments and `for`
keywords, then multiplies by the known worst-case ops (and bytes) per construct. Over-estimating is
free — a few unused entries; under-estimating must be impossible, so the estimator is deliberately
conservative and `push()`/`emit()` still fail cleanly if it is ever wrong. The existing
`overflow_` path stays as the backstop it already is.

`count`/`cap` widen to `uint16_t`, so the ceiling stops being a `uint8_t`. `kMaxIrOps` and `kCodeCap`
survive as upper *sanity* bounds — a runaway script fails with a diagnostic rather than exhausting
the heap — not as the working limit.

Both allocations are freed when compilation ends: they are compile-time scratch, not part of the
running program. The only thing that outlives a compile is the exec block, which is unchanged.

### 2. Spill to the frame (removes the register wall)

**The frame is a call frame, not a slot file.** Script-local functions are the next feature: callable
from the script, taking arguments, containing loops and `if`, calling other functions, and
recursive. A recursive function's spill slots cannot be one fixed region — each activation needs its
own — so slots are addressed as offsets from a frame pointer that a prologue establishes, which is
the layout a nested call reuses by pushing another frame. Spilling one top-level program is what
ships here; the frame discipline is chosen so functions add a call sequence rather than a redesign.

**New IR ops** ([MoonLiveIr.h](src/core/moonlive/MoonLiveIr.h)):

```cpp
Spill,     // slot[imm] = a
Reload,    // dst = slot[imm]
```

**New core pass** `src/core/moonlive/MoonLiveSpill.{h,cpp}`:

```cpp
/// Rewrite `ir` so no op names a vreg the target does not have, inserting Spill/Reload against a
/// fixed slot file. False when even the spilled form does not fit (fail, never miscompile).
bool spillToBudget(IrProgram& ir, const RegBudget& budget);
```

**Algorithm: linear-scan register allocation** (Poletto & Sarkar) over the op array, with
**loop-extended live intervals**. Three passes, no heap beyond the interval array:

- **Find loops.** The grammar has no `break`, `continue` or `goto`, so a loop is exactly a
  `BranchNe` whose target label is bound earlier in the array. The op array is therefore already in
  reverse-postorder and no CFG needs building — that is the one bespoke simplification, and it
  carries a guard: a branch pattern that is *not* properly nested makes the pass refuse rather than
  allocate against a wrong interval, so a future `break` fails loudly instead of miscompiling.
- **Naive intervals**, then **loop extension**: any value live at a loop header is live to the end of
  that loop, applied innermost-first. This is the step naive "first def to last use" gets wrong, and
  it is conservative — it can only lengthen an interval, so it may cost a needless spill but never
  produces a wrong one.
- **Scan**, spilling the active interval with the furthest end when no register is free.

**Backend surface** — identical on all three, and the algorithm appears nowhere in the platform layer:

```cpp
void prologue(uint8_t slots);        // slots == 0 emits nothing: a non-spilling script pays zero
void spillStore(Reg r, uint8_t slot);
void spillLoad(Reg r, uint8_t slot);
```

Each lowerer gains two switch arms and *loses* its hand-rolled budget bail (the three duplicated
`vregsUsed + N > kRegCount` checks collapse into the one core pass).

Where the slots live differs per target and is the real per-backend work:

| backend | frame today | spill slots |
|---|---|---|
| Xtensa | whole-routine 48 B from `entry a1, 48`; `call()` uses 16/20/24/28 | bytes 32–47 are free — 4 slots at zero cost; `entry` immediate grows for more |
| RISC-V | **none outside `call()`** (`prologue()` is empty) | needs a real 2-instruction prologue/epilogue; `encSw`/`encLw` already exist at file scope |
| arm64 | **none outside `call()`**; the `call()` frame is 100% full | needs a prologue *and* two new `str`/`ldr` encoders — the only backend with no general store/load |

### 3. Conditional inline scratch (the cheap part of the register fix)

All three lowerers reserve scratch vregs for `FillElems` unconditionally — `+3` on host, `+2` on
Xtensa and RISC-V — even when the program contains no such op. A layout script never emits one. This
single unconditional reservation is what makes `grid.mlv` (11 vregs, budget 12) fail on Xtensa.
Core reports which inline ops a program actually contains; each backend maps that to its own scratch
count. Nested loops compile on Xtensa from this alone, and it is the `RegBudget.reserved` field the
spill pass consumes — not throwaway.

## Files

- `src/core/moonlive/MoonLiveIr.h` — heap op array, `uint16_t` counts, `Spill`/`Reload`,
  `inlineScratch()`, the raised `kMaxVRegs`/`kIrLabels`
- `src/core/moonlive/MoonLiveSpill.{h,cpp}` — **new**, the linear scan
- `src/core/moonlive/MoonLiveCompiler.cpp` — the sizing pre-pass, the pass call site (~line 516), the
  temp allocator's failure path (lines 141-160), `locals[]` and `kMaxCtrls`
- `src/light/moonlive/MoonLive{Layout,Effect,Modifier}.h` — the bindings' `ctrlNames_` name pool
  mirrors `kMaxCtrls` and must grow with it, or the extra controls compile but never appear in the UI
- `src/core/moonlive/moonlive_emit.h` — the `RegBudget` seam
- `src/platform/esp32/moonlive_asm_riscv.cpp` — the `kScratchFn` fix (line 20), prologue + spill surface
- `src/platform/desktop/moonlive_asm_host.{h,cpp}` — new `str`/`ldr` encoders, prologue; the only
  backend tests execute
- `src/platform/esp32/moonlive_asm_xtensa.{h,cpp}` — promote the private `s32i`/`l32i` lambdas to the
  spill surface
- the three `moonlive_lower_*.cpp` — two switch arms each, minus their budget bails
- `moondeck/moonlive/` — generalise `emit_xtensa.cpp` to an ISA flag so `disasm.py --isa riscv` works
- `docs/moonmodules/light/MoonLive*.md`, `moonlive/README.md` — the new limits
- `docs/backlog/backlog-light.md` — narrow `call()` save-sets, by name

Add a `static_assert` per backend that no scratch register appears in its vreg map — the invariant
the RISC-V bug broke, made unbreakable rather than commented.

## Verification

The governing risk: **only arm64 is executed by tests**; Xtensa and RISC-V are compile-time-excluded
and validated on hardware. So arm64 carries the correctness proof, and the device backends carry
only encoding risk, which `disasm.py` retires without a flash.

1. **The key test — a squeezed budget on the host.** A test-only budget override runs
   `spillToBudget` with a register count *smaller* than the host's, forcing the spiller to run on the
   one backend that executes. Same script compiled at full and squeezed budgets must produce
   identical pixels. This makes the hard algorithm testable rather than hardware-only.
2. **The back-edge case specifically**: a nested loop at a squeezed budget where the counters are
   guaranteed spilled — every expected light placed exactly once.
3. **Spill across a call**: value spilled, `random16()` called, value used. Proves slots survive
   `call()`'s own frame — the RISC-V case to check hardest, since prologue and `call()` both move sp.
4. **One test per ceiling**, each a script that fails today and must pass after — this is what proves
   the overview table was actually delivered rather than partly delivered:
   - 30+ straight-line statements (`kMaxIrOps`, fails at 7 today)
   - a script whose emitted code exceeds 768 bytes (`kCodeCap`)
   - 6-deep loop nesting (`locals`) and 10+ loops in one script (`kIrLabels`/`kMaxLabels`/`kMaxFixups`)
   - 12 declared controls (`kMaxCtrls`) — and the *binding* surfaces all 12, since it mirrors the cap
     in its own name pool
   - a script needing more than 16 live values (`kMaxVRegs`, only reachable once spilling works)
5. **Stack, not just heap**: assert the compile path's stack frame *shrank*. `IrProgram` and the
   assembler stop being 3.4 KB of stack locals; a one-statement script must allocate proportionally
   less than a hundred-statement one. Without this the change could pass every functional test while
   quietly moving the bootloop somewhere else.
6. **Degrade**: a deliberately absurd script fails with a clear diagnostic and no crash; an alloc
   failure in `reserve()` fails the compile cleanly rather than writing through a null pointer.
6. **Unchanged behaviour**: `unit_moonlive_ir` / `unit_moonlive_fill` (the `fill` behavioural golden,
   and kArg4 surviving a call) stay green — they pin that a `FillElems` program still gets its
   scratch. `unit_MoonLiveScripts.cpp:118` (bare vs commented produce equal length) is the canary for
   the pass accidentally becoming source-dependent.
7. **Encodings on device backends without flashing**: `uv run moondeck/moonlive/disasm.py` on a
   spilling script, reading the actual `s32i`/`l32i` offsets against the frame layout. This is the
   tool that found the `Mov`→`addi 0` bug. Extend it to RISC-V, which has no equivalent today.
8. **Memory + hot path**: `collect_kpi.py --commit`. The IR allocation is cold-path, but a modifier
   script runs once per light, so measure a mapping rebuild on a large grid. Confirm a non-spilling
   script emits no prologue and costs nothing.
9. **Hardware, the final gate (PO)**: flash `grid.mlv` on an S3 (Xtensa — the target that fails
   today) and a P4 (RISC-V — the target with the scratch bug), and look at the wall.

## Suggested commit boundaries

The PO decides commits and branches; this is the order that keeps each step independently
verifiable, riskiest-last:

1. RISC-V `kScratchFn` aliasing fix + the `static_assert`s (independent bug)
2. Conditional inline scratch — nested loops compile on Xtensa
3. Right-sized IR + code buffer, and the cheap ceilings raised (`locals`, `kMaxCtrls`, `kIrLabels`,
   `kMaxFixups`) — the 7-statement wall goes and the compile stack shrinks
4. Spill surface on the three assemblers (encodings verifiable in isolation, dead code until 5)
5. The core spill pass + `kMaxVRegs` raised — the register ceiling goes

Steps 1–3 deliver the ceiling a user meets first and are independently shippable; 4–5 are the
spiller. If the branch needs splitting for review size, that is the seam.
