# Plan: MoonLive on a stack machine — the frame is where values live

Supersedes [Plan-20260809 — MoonLive scales](Plan-20260809%20-%20MoonLive%20scales%20%E2%80%94%20right-sized%20IR,%20and%20the%20stack%20as%20the%20register%20overflow.md),
whose steps 1–3 shipped and stand. This replaces its steps 4–5 (the register allocator) with a
different answer to the same goal.

## Why the change

The 20260809 plan set the right goal — a script bounded by memory, not by register count — and
reached for register allocation with spilling. That shipped, works on the host at every budget from
14 registers down to 10, and is textbook-correct.

It still leaves MoonLive broken on Xtensa, and the arithmetic says it always will:

```text
10 registers (a2..a11)  −  1 inline scratch  =  9
                        −  5 fixed ABI vregs =  4
                        −  4 reload temps    =  0 keepable
```

Zero keepable registers means the allocator has nothing to allocate with, so **every looped script
is refused on Xtensa** — `codegen failed (unsupported on this target, or too large)`. Bench-measured
on an S3: straight-line scripts run, every `for` is refused. The ten is not negotiable: a12/a13 are
call scratch and the address register for `store8`, and a14/a15 carry the routine's own `retw.n`
return linkage (using them as vregs is what corrupted the return path and produced
`Guru Meditation (IllegalInstruction)` on every scripted layout).

So on the smallest supported target the allocator's working budget is zero. A design whose margin is
zero on the platform it exists to serve is the wrong design, however correct its algorithm.

## The decision: a stack machine

**Every script variable gets a home in the call frame.** A read is a load from a frame offset, a
write is a store to it. Registers hold expression temporaries only, for the span of one expression.

It is also the shape prior art on this chip settles on: every variable carries a stack position,
values move by `l32i`/`s32i` against the frame pointer, and registers come from a small rotating
pool popped for expression temporaries.

### Why this is subtraction, not a sideways move

The register model makes every language feature interact with the allocator. The stack model makes
each one an application of the same recursive mechanism:

| feature | register machine | stack machine |
|---|---|---|
| nested `for` | loop-extended live intervals, innermost-first | nesting is nesting |
| `if` | live-range merge at the join | a branch over a region; storage is untouched |
| function call | live-across-call analysis, a save-set per call site | push args, `call`; callee addresses its own frame |
| function arguments | bounded by spare vregs (ours is fixed at 3) | as many slots as you push |
| recursion | a fixed slot file cannot hold two activations | each activation gets its own frame |
| local functions calling each other | compounding of all of the above | nesting again |

In a stack machine a function's frame size falls out of the same stack position that placed the
variables, and an argument is one more slot in that frame. One mechanism, three jobs — locals,
arguments and the frame itself.

What this deletes: `MoonLiveSpill.{h,cpp}` (375 lines), `RegBudget` and its 34 plumbing sites across
three lowerers, the reload-temp accounting, loop-extended interval analysis, and `kMaxVRegs` as a
ceiling. CLAUDE.md Principle 3: *the first question on any change is what it can remove.*

### What it costs

A variable access becomes a load or a store rather than a register reference — roughly two extra
instructions per touch. On arithmetic-heavy inner loops expect 20–40% slower; on scripts dominated
by host calls (`setRGB`, `beat`, `sin`, and every layout's `addLight`, which is a `call8`) the
difference is close to nothing, because the call already dwarfs a pair of loads.

Two things make that the right trade today. Architecture.md's claim is "near-hand-written speed **in
the hot path**", and the hot path is the inline buffer write and the host calls, not counter
arithmetic. And the honest comparison is not slower-versus-faster: on Xtensa the current alternative
is *refused*. A script that runs slightly slower beats a script that does not compile.

### One implementation per construct, not one per target

A stack machine makes most of the lowering ISA-independent, and that has to be spent on removing
duplication rather than replicating a simpler design three times. Measured today: the three lowerers
are 142/138/150 lines and roughly **65% identical** (48 differing lines between Xtensa and RISC-V
after normalising names) — the same walk over the same IR, three times.

What is genuinely per-target is small and nameable: instruction encodings, the register/frame ABI,
and the branch forms. Everything above that — how a `for` becomes an entry guard and a back edge,
how a call passes arguments, how an expression evaluates, where a variable's frame slot is — is one
algorithm that belongs in core, written once.

**The rule for this rework:** a new target should be a new *assembler* (encodings + ABI constants),
not a new lowerer. Adding an 8086, an ARM32, or anything else must not mean copying the IR walk
again. Where a construct genuinely differs per ISA, the difference is a named hook on the assembler
surface — not a forked copy of the surrounding logic. This is CLAUDE.md Principle 3: core owns the
hard construct, written once; the platform layer holds only what is truly platform-specific.

### One binding, three roles

The same duplication exists in the light domain. `MoonLiveLayout`, `MoonLiveEffect` and
`MoonLiveModifier` are 196/140/182 lines and each carries its own copy of the same machinery:
`char script_[32]`, `uint32_t compiledHash_`, a `MoonLive engine_`, and the load-compile-cache-report
sequence around them. A fourth role (a scripted DRIVER) is coming, and it must not mean a fourth copy.

Factor the shared part into one place — the script name, the compiled-content hash, the engine, the
compile-on-demand-and-report path, and the "a failed load leaves a decided state" rule that a bug
this session had to fix in the layout alone. What stays per-role is only what genuinely differs:
what the compiled program is called with, and what it does with the result.

Architecture.md already states the standard this meets: *"When a scripted binding needs a mechanism
its compiled sibling does not, that is a finding: either the mechanism belongs in the base for
everyone, or the divergence needs its reason stated where it is introduced."* Three copies of the
same mechanism is the same finding, one level down.

### One system-variable vocabulary, not three

The three roles are handed three different `SysVarTable`s today — `layoutSysVars` (the clock only),
`effectSysVars` (+ `width`/`height`/`depth`) and `modifierSysVars` (+ `x`/`y`/`z`). **Collapse them
into one table every script gets**, and let the script author use what makes sense for the job.

The current split buys less than it costs. It does not prevent a mistake — a layout that reads
`width` gets a compile error rather than a wrong answer, which is the same outcome as reading a
variable that is always zero — and it creates a trap: the tables are *different vocabularies, not
nested ones*, so a name means one thing in one role and is reserved in another. That trap is not
theoretical. A layout may use `x`/`y` as ordinary loop counters precisely because it is NOT handed
them, which the shipped `grid.mlv` does — and `disasm.py`, which compiled everything against the
widest table, therefore refused the one script most worth inspecting with "name is a system
variable". The tool was blind to the default layout for as long as it existed, and that cost more
debugging time this session than any single bug.

Two changes make one table work:

- **`width`/`height`/`depth` mean the same thing in every script**: the dimensions of the grid. A
  layout DEFINES them by the coordinates it places; an effect and a modifier READ them. Same name,
  same meaning, no per-role reservation.
- **A modifier's per-light coordinate is renamed `xPos`/`yPos`/`zPos`.** `x`/`y`/`z` are the names a
  script author naturally reaches for as loop counters, so reserving them globally would break the
  most ordinary code there is. The renamed form is unambiguous, and it frees `x`/`y`/`z` for their
  obvious use everywhere.

The arena offsets are already fixed constants (`kSysWidth`, `kSysX`, …) shared by every role, so
this is removing a distinction the storage layer never made — not introducing one. What a binding
still decides is which slots it WRITES each frame; reading is uniform.

This is a breaking change for any script using a modifier's `x`/`y`/`z`, so it needs its
[MIGRATING.md](../../MIGRATING.md) entry and a sweep of the shipped `moonlive/` scripts.

### Clean first, with speed decisions made deliberately

Build the industry-standard construct first and keep it whole. Where a compromise is genuinely
needed for speed, make it **at that moment, explicitly, with its reason recorded next to it** — not
by pre-emptively complicating the design against a cost nobody has measured.

That means: no speculative fast paths, no "we might need this in a register" hedges, and no
special-casing a construct because it *might* be hot. Write the clean form, measure it
(`collect_kpi.py`, and the bench), and when a number says a specific thing is too slow, fix that
specific thing and say why in a comment where it lives. A compromise with its rationale attached is
maintainable; a design pre-bent around an unmeasured fear is not.

### The limits: which are physics, which are choices

"Bounded by memory, not by constants" needs to know which constants can actually move. Measured:

**Hardware-fixed — these cannot be raised at all.**

| limit | ceiling | why |
|---|---|---|
| `kRegCount` | Xtensa **10**, RISC-V 14, host 14 | the machine's register file — the wall this whole plan routes around |
| `kArenaBytes` | 255 | `LoadCtrl` lowers to `l8ui`/`lbu`, whose offset immediate is one byte |

**Chosen numbers, far below what the hardware allows — raisable when there is a reason.**

| limit | now | hardware allows | what raising costs |
|---|---|---|---|
| `kMaxSpillSlots` / `kMaxLocals` | 16 | **243** on Xtensa (`s32i`'s offset byte counts 4-byte words; `entry` reaches a 32 KB frame), **512** on RISC-V | 16 B per local in `locals[]`, plus frame bytes per running script |
| `kMaxVRegs` | 32 | 255 (the index is a `uint8_t`) | `Interval iv[]` in the spiller, ~16 B per vreg of compile stack |
| `kIrLabels` | 16 | 255 | 32 B per lowerer |
| `kMaxCtrls` | 8 | the arena's 255, minus the system variables | 16 B each — and the UI has to render them |

**Already unbounded in practice.** `kMaxIrOps` and `kCodeCap` size HEAP allocations that are already
right-sized per script, and `platform::alloc` prefers PSRAM where a device has it. They are sanity
bounds so a runaway source fails with a diagnostic rather than exhausting the heap — not working
limits. The remaining fixed arrays total roughly 600 bytes per compile (`locals[16]` at 256 B is the
largest); moving those to the heap would add allocation, failure paths and lifetimes to save half a
kilobyte on a cold path, which is the opposite of subtraction.

**`kMaxLocals` and `kMaxSpillSlots` must move together.** They index the ONE frame: the front end
numbers a script's variables from zero and the register allocator numbers its spills above them, so
raising one alone silently shrinks the other's room. `spillToBudget` refuses a compile when the front
end asks for more slots than the backend can address, which turns a mismatch into a diagnostic rather
than a truncated offset writing over a live value.

**Do not raise any of these speculatively.** Once variables live in the frame, frame slots — not
registers — are what a complex script consumes, so `kMaxLocals` becomes the real ceiling on script
size. Set it from a measurement once the mechanism exists, not from a guess before it does.

### Register optimization is explicitly out of scope

Deliberately deferred, in full, including the tempting parts:

- No promotion of hot values into registers.
- No pinning of the fixed ABI vregs (`buf`, `nLights`, `cpl`, `ctrls`, `t`) — **whether any of them
  stay in registers is itself a register decision, and it is reopened, not assumed.** They may all
  live in the frame in the baseline.
- No caching of the innermost loop counter.

The reason is that register optimization intertwines with everything it touches; adding it to a
design that already has it half-present makes both harder to read. Get a clean stack machine first,
measure it, and then decide what — if anything — to promote, as an addon over a correct baseline.
That inverts the failure mode from "refuse to compile" to "compile, possibly slower", which is the
Robustness principle.

## Verification comes first

Today's session produced five wrong theories about emitted Xtensa code, each plausible from reading
the source, each falsified by the device. The lesson is that **no test executes Xtensa code** — only
arm64 runs in tests, so a codegen defect ships silently and is debugged by flashing.

That gap is fixed **before** the rework, not after:

1. **Golden-bytes tests over the shipped scripts.** `disasm.py` emits Xtensa for a script on the
   host; assert the byte stream for `grid.mlv` and friends. Any codegen change that alters emission
   fails loudly and visibly, with the diff readable.
2. **A structural checker on emitted code**: every branch and jump target lands on an instruction
   boundary inside the program; frame offsets stay inside the frame `entry` allocated; no
   instruction names a register outside the backend's map. These are the three defect classes found
   today, each made unrepeatable.
3. **Keep the squeezed-budget behavioural tests.** They pin that the same script computes the same
   pixels regardless of storage decisions — which is exactly the invariant this rework must preserve.

`disasm.py` itself needed two fixes to be usable at all (it did not link `MoonLiveSpill.cpp`, and it
compiled every script against `modifierSysVars()` — so `x`/`y` were reserved and it had never once
successfully read the shipped `grid.mlv`). Tool blindness cost more time this session than any bug.

## Sequence

Each step is independently verifiable, and the branch stays green throughout:

1. ✅ **The emitted-code tests above**, against current behaviour. Two per-ISA test TUs share one
   body, so the checks are written once: the device backends now compile and run on the development
   machine through a `lower` seam on `compileSource`. 11 tests.
2. ✅ **A frame slot per script variable.** A loop's counter and limit each get a slot; reading a
   variable emits a Reload into a temp that dies immediately. The guard that protected locals'
   registers is GONE — every vreg reaching freeTemp is now a temp. Measured on Xtensa: grid.mlv
   212 → 186 bytes, and three-deep nesting compiled for the first time.
3. ✅ **Registers become expression temporaries.** Call arguments are staged through the frame —
   each is parked as soon as it is computed and all are reloaded for the one instruction that reads
   them, so only ONE argument holds a register at a time. This is what let a looped effect, a
   four-deep nested layout and plasma compile on Xtensa at all.
3b. ⬜ **The HOST ARGUMENTS go to the frame too** (`buf`, `nLights`, `cpl`, `t`, `ctrls`). They are
   read by the inline ops and LoadCtrl and never written, yet they permanently occupied FIVE
   registers — on Xtensa, five of the six the windowed ABI leaves a routine that calls. Parked at
   entry and reloaded where read, they cost a load at the point of use and free the register file
   for temporaries. This belongs in CORE, not in a lowerer: it is the same "the frame is where
   values live" rule the script's own variables follow, and doing it per backend would be three
   copies of one policy — the duplication step 4 exists to remove.
3c. ⬜ **Unlimited call arguments.** The plan's own table promises "as many slots as you push", and
   the machinery for it already exists — `parseCall` parks every argument in a CONSECUTIVE frame
   slot as it is evaluated. Three arbitrary constants cap it anyway: `VReg args[4]`, `n >= 4`, and
   "a call takes at most three arguments" (`HostCallFn` is a 3-parameter C function pointer).

   That cap is a real design limit, not a detail. A `line(x0, y0, x1, y1)` does not fit, and the
   workaround — splitting it into `lineH`/`lineV`, or setting colour through ambient state — is
   exactly the bespoke special case a stack machine exists to avoid. Every builtin added after it
   would inherit the same distortion.

   **Do NOT widen `HostCallFn` and the three assemblers' `call()`.** That spends the change on the
   most fragile code in the project (the Xtensa call sequence has produced three separate defects)
   and still leaves a fixed maximum, just a larger one.

   **Pass a POINTER to the argument slots.** The arguments are already in consecutive frame slots;
   the call just has to say where. Each assembler already knows its own frame layout — `spillStore`
   and `spillLoad` compute exactly this address — so each can materialise `framePtr + argBase*4`
   into the first argument register. The host signature becomes:

   ```cpp
   using HostCallFn = uint32_t (*)(const uint32_t* args, uint32_t argc, const uint8_t* arena);
   ```

   Three parameters, so `call()` and its 3-argument sequence are UNCHANGED in all three assemblers —
   the fragile Xtensa windowed-call code is not touched. Arity is bounded by frame slots, which is a
   memory question: the goal of this plan.

   NOT the arena: its bytes are `uint8` (the offset is an 8-bit immediate in `l8ui`/`lbu`), so it
   cannot carry a `uint32_t` argument. Frame slots are 4-byte words and already hold full values.

   Each existing builtin becomes `args[0]`, `args[1]`, `args[2]` instead of named parameters — a
   mechanical change, and one that finally lets `line(x0, y0, x1, y1)` and the seven-argument
   `draw::line` be ordinary calls rather than a special case. Script-local functions will pass their
   arguments the same way when they arrive.

4. **Collapse the three lowerers into one.** With storage no longer per-target, the IR walk is one
   algorithm; what remains per-backend is encodings, the frame/ABI constants and the branch forms.
   Verify by adding nothing to the platform layer that is not genuinely platform-specific.
5. **Delete the allocator** (`MoonLiveSpill.{h,cpp}`, `RegBudget`, the per-backend budget plumbing)
   once nothing calls it. The `prologue(slots)` / `spillStore` / `spillLoad` surface on the three
   assemblers is KEPT — it is already frame-addressed through a frame pointer for exactly this
   reason, and the stack machine uses it directly.
6. **One system-variable table for every role**, with a modifier's coordinate renamed to
   `xPos`/`yPos`/`zPos`. Independent of the codegen work — different files, no shared risk — so it
   can land whenever, but it comes before step 7 because it is what makes the bindings differ by
   almost nothing. Needs a MIGRATING entry and a sweep of the shipped scripts.
7. **Factor the three bindings onto one shared base**, so the script name, hash, engine and the
   compile-and-report path exist once. The scripted driver that follows should then be a small
   subclass, and that is the test of whether this step actually worked.
8. **Bench: S3 and P4**, a scripted layout and a scripted effect, both with nested loops.
9. **Measure** with `collect_kpi.py` and record the cost honestly in performance.md, so the later
   decision about register promotion is made against numbers rather than intuition.

Steps 4, 6 and 7 are the deduplication, and they come AFTER the mechanism works rather than during it —
collapsing three copies while the design underneath is still moving would mean doing it twice.

## Xtensa: the one target still failing, and why

**Status after steps 1-3: two of three targets run the whole stack.** Desktop (arm64) renders a
scripted grid layout with a plasma effect at 76,923 fps. An ESP32-S31 (RISC-V) has held layout +
effect + modifier for over an hour. An ESP32-S3 (Xtensa) crashes on any script that stores a pixel,
and did so before this rework too — this is a pre-existing backend defect the stack machine exposed,
not one it introduced.

### What a windowed register ABI is

Most CPUs have a **flat** register file: one set of registers, and a function that wants to keep a
value across a call must save it to the stack itself. Every save is an instruction you can read in
the disassembly.

Xtensa instead has a large physical register file (64 registers) of which a function sees a **window**
of 16 at a time. `call8` does not jump — it *rotates the window by eight* before jumping. The callee's
`a0..a7` are physically the caller's `a8..a15`, and the callee's `entry` instruction slides the window
further. Nothing is saved by an instruction; the renaming IS the save. When the physical file wraps
around, hardware exception handlers spill the oldest window to the stack automatically.

### Why Xtensa has it and the others do not

It was a 1990s answer to "calls are expensive": rotating a window makes a call cheaper than pushing
registers, which mattered when memory was slow relative to the core. SPARC made the same choice.
Modern designs went the other way — RISC-V and ARM64 both use a flat file with an explicit
caller/callee-saved split, because compilers got good at register allocation and a predictable,
visible ABI is worth more than saved store instructions. Xtensa is a configurable core and ESP32
ships the windowed option, so the ESP32 classic and S3 have it; the P4 and S31 are RISC-V and do not.

### Why this breaks a JIT specifically

A compiler emitting Xtensa knows the rule and never puts a live value in `a8..a15` around a call. Our
backend chose its vreg map by counting free registers rather than by asking which survive a call:
`kXtReg` is `a2..a11`, and ESP-IDF's own `coreasm.h` states plainly that `a8..a15` are **clobbered**
by `call8`. So four of ten vregs sat inside the rotation window. The measured crash had `A0 = 0x100`
— the value 256, which is `nLights`, a script value that had landed in the return-address register.

Three separate defects of this family have already been found and fixed here: `a14`/`a15` used as
vregs (they carry the `retw.n` linkage), the call RESULT stashed in `a12` (the callee's `a4` after
rotation, so the callee overwrote it), and branch displacements truncated past ±127. None of them can
exist on a flat-file backend, which is exactly why arm64 and RISC-V never showed a symptom.

### How to deal with it: make it look flat

**Yes — the windowed ABI can be treated as flat, by simply not using the window.** Two options:

1. **Restrict the vreg map to `a2..a7`** — the six registers that survive `call8` — and treat
   `a8..a15` as if they did not exist. The rotation still happens on a call, but no value we care
   about lives in the rotated range, so it becomes invisible. This is the smaller change and it makes
   the Xtensa backend behave exactly like the flat ones.

2. **Use `call4` instead of `call8`** — rotating by four leaves `a4..a15` intact for the caller. It
   widens the usable range, but the callee then sees a smaller window, and every host builtin is
   ordinary compiled C we do not control. Rejected: it constrains code we do not own.

Option 1 is the plan. The blocker is arithmetic, and it is precisely what step 3b removes: six
registers minus the five permanently held by the host arguments leaves one, which fits nothing. Park
the host arguments in the frame — the same rule every other value now follows — and six registers is
ample, because after steps 2 and 3 registers only ever hold one expression's temporaries.

**So the Xtensa fix is not a special case; it is step 3b plus a one-line map change.** That is the
argument for doing 3b next rather than treating Xtensa as its own problem: the same subtraction that
simplifies all three backends is what makes the smallest one correct.

An attempt at 3b during this session got every shipped script compiling on Xtensa at eight registers,
then hit a SIGSEGV in the HOST backend and was reverted. The cause is known: register numbering is
derived independently in four places — core's compaction, the spill pass's reservations, each
lowerer's scratch arithmetic, and each assembler's map — and moving the host arguments perturbs all
four. `src/core/moonlive/register-and-slot-contract.md` now writes that ownership down, and 3b should
be re-attempted against it rather than by iteration.

## Then, separately

Only after the above is on main and measured:

- Whether to promote anything into registers, and which — including whether any of the fixed ABI
  vregs earn a register at all.
- Script-local functions with arguments, `if`, and recursion — the features this design exists to
  make cheap.
- A scripted DRIVER as the fourth role, which is the honest test of step 6: if it needs more than a
  small subclass, the factoring did not go far enough.
