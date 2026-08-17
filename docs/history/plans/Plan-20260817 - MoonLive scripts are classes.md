# Plan: MoonLive scripts are classes

Takes over from [Plan-20260813 — MoonLive on a stack machine](Plan-20260813%20-%20MoonLive%20on%20a%20stack%20machine%20%E2%80%94%20the%20frame%20is%20where%20values%20live%20(shipped).md),
whose step 7 (factor the three bindings onto one shared base) this replaces. That plan finished the
MACHINE: values live in frame slots, one lowering serves every backend, one system-variable
vocabulary serves every role. This plan changes what a script IS: how it is written (steps 1 to 5)
and what it can say (steps 6 to 10).

**MoonLive launches when all of it is there**, so the order below is the one that is best to BUILD
in, not the one that shows best soonest. The consequence worth stating out loud is that the most
visible work (`if`, reading a light back, particles, the editor) comes last, which is a deliberate
trade rather than an oversight.

## What is missing before it can launch

The engine is not the gap. Live-editing a script on a running device and watching 12,288 lights
change on the next tick, at native speed, is already true, and it is the thing nobody expects from a
microcontroller. The gap is what a script can EXPRESS: today that is smooth arithmetic over a grid,
which gives plasma, ripples and gradients and stops there. Steps 6 to 9 are that list.

Recursion is in scope and ships in step 1, but it is not on that list, because it is not what an
effect author is missing: they do not write recursive functions. It earns its place for a different
reason. It is what makes this a real language rather than a macro expander, which is a credibility
floor rather than a feature anyone points at, and the stack machine already bought it, so the cost
now is that a script function gets a real frame instead of being special-cased.

## Why the change

A MoonLive script today is a bag of declarations and statements, and a control is declared by a
COMMENT that changes behaviour:

```
uint8_t bpm = 30;   // @control 1..240
for (y = 0; y < height; y = y + 1) { ... }
```

That is not C, and it does not resemble the compiled module it stands in for. A compiled effect is a
class with `defineControls()` and `tick()`; a scripted one should read the same way, so that what a
contributor learns from one transfers to the other. The end state:

```
class PlasmaEffect {
  uint8_t bpm;
  uint8_t zoom;

  defineControls() {
    addUint8("bpm", 30, 1, 240);
    addUint8("zoom", 24, 1, 64);
  }

  tick() {
    for (y = 0; y < height; y = y + 1) { ... }
  }
}
```

Step 7 of the previous plan tried to reach the same goal from the other end, by factoring the three
BINDINGS onto a shared base. It was superseded for two reasons, and both are what this plan is built
on. The code objected: the three bindings derive from three sibling bases under `MoonModule`, so a
shared base needs virtual inheritance and changes the layout of every module in the system to serve
three of them. And the direction objected: once a script defines NAMED ENTRY POINTS, the three
bindings stop being three kinds and become one kind with different entry points present, which is a
dispatch question rather than an inheritance one. The structure falls out at the end (step 5) instead
of being designed up front.

## The enclosing class declaration

`class PlasmaEffect { ... }` is the only top-level form. It makes the class semantics VISIBLE rather
than implied: without it a script merely behaves like a class and a reader has to be told, while with
it `defineControls` and `tick` stop looking like magic top-level names and read as what they are,
members the host calls. It also gives the engine a NAME that is not the filename, for the UI, the
status line and error messages, which matters the first time somebody renames a file.

Two constraints on it:

- **The filename loads it; the class name identifies it.** The `script` control still holds the file
  name, because that is what the engine reads from the filesystem, and the class name is what the
  status line and compile errors report. This is how a C translation unit works: `plasma.c` is what
  you compile, and the diagnostics name the function inside it. Both can be renamed independently
  without breaking the other.
- **The name is just a name.** Role does NOT come from the suffix. Inferring "this is an effect" from
  `...Effect` is the kind of magic that surprises people the first time a rename changes behaviour;
  the role comes from which entry points the class defines, which is step 5's dispatch model, and it
  is also what lets one class define both `tick` and `modifyLogical`.
- **The declaration is MANDATORY.** One top-level form, not two. Optional was considered, on the
  argument that `onered.mlv` is a single `setRGB` and a class around it is ceremony, and rejected:
  it would keep a bare statement list in the grammar forever, which is a second parse path, a second
  set of rules to document, and a second thing to test, permanently, so that a handful of two-line
  scripts can stay two lines. That is more code to support less clarity, and the whole point of this
  plan is that a reader can tell what a script is by looking at it. Nothing is released yet, so the
  only cost is rewriting the shipped scripts once, which is our own work.

## Decisions taken

**The standard: a script IS a class.** Not "like" one loosely: it has members (script-level
variables) and functions, some of which the host calls from outside at known times (`tick`,
`defineControls`, `forEachCoord`). Syntax may be simplified, and semantics may be simplified where
that buys something, but where a reader has an expectation from any class-based language, the
behaviour meets it. Most of what follows is settled by asking "what would a class do".

**Where script-level state lives.** The moment a script has more than
one function, a variable shared between them cannot live in a frame slot, because the frame belongs
to one call and dies with it. That is this plan's own premise ("the frame is where values live")
meeting the one case it does not cover. The same mechanism is what a variable persisting ACROSS
`tick()` calls needs, which is the stateful-effect family (fire, trails, decay) the language cannot
express at all today, so it is worth solving once rather than twice.

The storage already exists: the CONTROL ARENA outlives every call, keeps a stable address across a
recompile, and is reachable from emitted code through `kArg4`. A script-level variable is close to
"a control the UI does not show", which is a good sign about the shape. What is missing is the rules,
and each is a real decision rather than a detail:

- **Scope: SETTLED by the class model.** A variable declared outside any function is a member:
  visible in every function, one per script instance.
- **Initialisation: SETTLED by the class model.** A member is initialised once when the object is
  constructed, which here is compile time, seeded exactly as a declared control already is. Persisting
  across `tick()` calls follows from that rather than needing a `setup()` entry point to explain it.
- **Width and TYPE: the open one, and the real work.** Arena slots are BYTES today, which is why a
  coordinate clamps at 255 and a shift modifier cannot walk a light off a large grid. A member must be
  able to be a scalar, a STRUCT (`Coord3D`), an ARRAY, or an array of structs. Not on day one, but the
  storage has to be designed for it, which makes this a typed addressable region rather than a wider
  row of bytes. Everything else here is downstream of this decision.
- **Cost.** Every access becomes an arena load rather than a frame slot read. Cheap at today's script
  sizes, worth measuring before it is the default for every variable.

The type question is settled before step 2 writes any of it, because `defineControls()` setting a
value that `tick()` reads is exactly this case, and finding the rules wrong late means rebuilding
whatever was laid on top of them.

**Argument passing: by value for scalars, by reference for aggregates.** The ABI already does both
with one mechanism, so neither is a special case: arguments are staged in consecutive frame slots and
the callee receives a POINTER to that block, so a value argument is "copy the value into the slot"
and a reference argument is "put the address in the slot".

Passing everything by reference was considered and rejected. It is not a simplification of the class
model but a departure from it: a script could then not have an ordinary scalar parameter, since
assigning to it would write through to the caller's variable, surprising in precisely the place this
design promises no surprises. It also collides with the host built-ins, which are compiled C
functions taking values (`sin`, `beat`, `scale`) and are not ours to change. The value/reference split
is what a contributor already predicts, and an explicit `&` can be added later without redesign.

**One exec block, an offset per entry point.** A script compiles to a single allocation holding
every function it defines, and the engine records each named entry's offset within it; the binding
gets a function pointer to that offset. This is what a symbol table is, and what every compiler and
JIT does: one code section, a name-to-address map over it.

The alternatives lose on specifics rather than on taste. A block per entry makes every
script-to-script call cross-allocation, so an ordinary relative call becomes an absolute address
fixed up at load, and it multiplies `allocExec` calls, which on ESP32 is scarce fragmenting IRAM. A
selector argument on one entry point puts a branch on every call, including `tick()` at 60 fps
forever, paying at run time for something known at compile time. With one block, a call between
script functions is just a call, and step 5's dispatch question ("which entry points does this
script define") is answered by which names are in the table.

The offset recorded must be the address in the FINAL PLACED block, not in the staging buffer:
`writeExec` copies to a different address, and the single-entry path already accounts for this, so
this is the same rule applied per name.

**Functions are REAL CALLS, and recursion works.** Not inlining, and not a later nicety. The
predecessor plan's own table lists recursion as something the stack machine BUYS ("a fixed slot file
cannot hold two activations" becomes "each activation gets its own frame"), which is one of the
reasons the rework happened at all. Inlining would satisfy `tick()` and nothing else: a recursive
function cannot be inlined, so choosing it would quietly drop the payoff. What real calls require:

- **A frame per activation, at run time.** Today the emitted routine has ONE frame from one
  `entry`/prologue, sized at compile time. A script function needs its own, so the prologue and the
  frame-slot addressing become per function rather than per program.
- **On Xtensa, a nested `call8`.** Every activation therefore owes the 32-byte window-save reserve
  the frame contract demands, and the structural checker has to see a script function's frame the
  same way it sees the entry routine's. This is the one place where the ISA makes recursion cost
  more than bookkeeping, and it is exactly the defect class that cost three days, so the checker
  extension belongs to this step rather than to a follow-up.
- **A depth bound with a clean diagnostic.** An ESP32 render task has a fixed stack, so unbounded
  recursion is a reset, which the robustness rule forbids. A general compile-time depth limit is not
  possible, so this is a runtime guard that degrades visibly.

**Inlining is NOT part of this.** It was proposed twice while writing this plan, first to keep the
hot path flat and then to get both answers at once, and it does not survive its own cost/benefit:

- **What it saves is not the cost.** Inlining removes one call and one frame setup per call site, on
  the order of a microsecond. MoonLive's time goes elsewhere: `ripples.mlv` ticks at 1695 us, nearly
  all of it in ~15 HOST calls per cell into libm. Script-to-script calls are not the bottleneck and
  are not on a path to becoming one.
- **What it costs is a pass.** A call graph, cycle detection over it, a size heuristic, and the
  substitution itself: rewriting a callee's IR into the caller with members remapped, labels
  renamed against collision, and arguments bound to caller expressions. Hundreds of lines in the
  compile path, on every device, in a compiler where only one backend executes in tests.
- **Its failure mode is worse than its win.** Getting the analysis wrong inlines a mutually
  recursive pair forever ("does it call itself" does not catch `a` calls `b` calls `a`), which hangs
  the compiler on a device rather than reporting an error.

So: real calls, always. It is the simplest thing that fully works, and it is what delivers recursion.
Inlining stays available as a pure optimisation if a measurement ever shows script-to-script calls
mattering, and it can be added then without changing any semantics, which is exactly why it does not
need to be decided now.

Note this is orthogonal to argument passing. Value-for-scalars and reference-for-aggregates holds
whether or not a call is inlined: references are about how a callee REACHES its caller's data, while
recursion is about each activation owning its OWN locals. A recursive function still passes scalars
by value, and still needs a frame per activation; using references to avoid frames would make every
activation share one set of locals and corrupt itself.

## Sequence

Steps 1 to 5 are the SHAPE: how a script is written. Steps 6 to 10 are the VOCABULARY: what it can
say. The shape comes first so that every feature in the back half lands on finished ground rather
than being retrofitted into a language still moving underneath it.

1. 🟡 **The `class` declaration and script functions, together.** PARTLY DONE: the class form
   ships and every script and test uses it; script-to-script CALLS and recursion are what remain,
   scoped at the end of this entry.

   Originally: They are one change: making the
   declaration mandatory means there is no bare-statement-list form left, so the grammar's new top
   level is a class body, and a class body holds functions. `tick()` is the first named entry point
   (an effect is the simplest case), and the shipped scripts convert in the same commit, because
   there is nothing to fall back on. First rather than last: with one top-level form, everything
   after it is written inside a class, and converting `moonlive/` twice would be the alternative.
   Calls are real from the start, per the section above: a script calling its own function, and then
   calling it recursively, is the acceptance test.

   **What recursion still needs, measured against the code as it now stands.** Per-function frames
   are DONE and each activation owns its frame, so the hard half is behind us. What is missing is
   that a script cannot yet call its own function at all: `parseCall` resolves a name against the
   BUILTIN table only, so `helper()` inside a class reports `unknown function`. Closing that is a
   defined piece of work rather than a subtlety:

   - **A script-call IR op.** `IrOp::Call` carries an absolute host function pointer; a call to a
     script function is a different thing, a jump to a label inside this block.
   - **A relative call in each assembler.** `call(...)` takes a `const void*` host address on all
     three backends. A script-to-script call needs call-to-label with the same fixup machinery the
     branches already use, which is a new instruction per ISA (`call0`/`callx` forms on Xtensa,
     `jal` on RISC-V, `bl` on arm64).
   - **The depth guard.** A fixed render-task stack means unbounded recursion is a reset, which the
     robustness rule forbids, so this is a runtime counter that degrades visibly rather than a
     compile-time limit.

   None of it is blocked, and none of it changes what is already verified: every shipped script,
   all three bindings and the moment model work on hardware without it.

2. ⬜ **Typed script-level members**, per *Where script-level state lives* above: a variable declared inside the
   class but outside any function lives in the arena, is visible in every function, is initialised
   once and survives every call. Scalars first, with the storage designed so a struct and an array
   can follow without moving anything. This is what makes a stateful effect (fire, trails, decay)
   expressible at all, so it is worth landing on its own and measuring before anything is built on
   it.

3. ⬜ **`defineControls()`, replacing the `// @control` comment.** A control is declared by calling
   `addUint8("bpm", 30, 1, 240)` inside a `defineControls()` the script defines, the same call a
   compiled module makes. Today's form is a COMMENT that changes behaviour, which is not C and does
   not resemble the thing it imitates; the lexer's `ControlAnno` token and its capture path go away
   with it. Comes after step 1 because it IS a function, and after step 2 because the control it
   declares is a member. The shipped scripts and the three docs move with it.

3b. ✅ **A frame per FUNCTION, not per program.** Done: each function emits its own prologue and
   epilogue, the host arguments are parked per function (they were spilling into a frame that did
   not exist yet, which was half the segfault), and the structural checker re-reads the frame at
   every prologue rather than judging the block by its first. Verified by control: shrinking the
   Xtensa reserve to 16 makes the checker fire, restoring it passes.

   Originally: The lowering emits one prologue before the first
   op; each function needs its own, with the epilogue to match, so that its recorded offset is an
   address a caller can actually jump to. Three parts, and the second is the one this project has
   already paid for once:

   - **Prologue and epilogue per function**, sized from that function's own slots rather than the
     program's total, which is also what makes each activation independent.
   - **On Xtensa, every activation owes the 32-byte window-save reserve.** A script function calling
     a built-in is a nested `call8`, so the frame contract applies to it exactly as to the entry
     routine, and the structural checker has to see a script function's frame the same way it sees
     the entry routine's. Extending the checker belongs to this step: it is the defect class that
     cost three days, and it is silent when wrong.
   - **The block start stops being the program.** With several functions in one block, falling off
     the end of one into the next is a real hazard, so each function returns rather than running on.

4. ✅ **The remaining entry points per role.** Done, and simplified by the moment model above:
   layouts declare `forEachCoord`, modifiers `modifyLogical`, effects `tick`, and each binding runs
   its moment IF the script defined it. `modifyLogicalTick` is not built; it is a new moment the
   Layer would have to own, so it belongs with whatever needs it.

   Originally: once the mechanism holds: `forEachCoord`/`lightCount`
   for a layout, `modifyLogical`/`modifyLogicalSize` for a modifier, plus `modifyLogicalTick` (a
   per-drawn-light hook we never implemented; MoonLight has it, and it is what a dynamic rotation
   modifier needs).

   **Every shipped script uses `tick()` until this step**, including the layouts and modifiers, which
   is a way-station rather than the shape: a layout does not tick, it is ASKED how many lights it has
   and where they are, and a modifier is asked to fold one coordinate. Naming both `tick` hides what
   the host actually does with them. It is what step 1 could deliver while `tick` was the only entry
   point in existence.

   **The byte-offset map is DONE** (landed with step 1). The parser records the IR index each
   function starts at, the shared lowering converts it to a byte as it emits, and `CompileResult`
   reports name plus offset: a symbol table over one code section. Pinned by a two-function test
   whose second entry must start after the first, verified to FAIL when the map is stubbed back to
   zero. `MoonLive::entry(name)` turns a name into a callable address.

   **But the map is necessary and not sufficient, which the code taught us by segfaulting.** Wiring
   a binding to CALL its entry point crashes, because the lowering emits ONE prologue for the whole
   program, before any function. An entry's recorded offset therefore points PAST the frame setup,
   and calling it directly runs a routine whose frame was never established; the first frame access
   faults. Per-function frames were sequenced after this step and belong before it: a named entry
   point is not callable until each function owns its frame. That is now step 3b, and this step is
   the wiring that follows it.

**A NAME IS A MOMENT, NOT A ROLE** (PO, during step 4). The binding does not pick which entry point
belongs to its kind. The HOST owns moments and calls whatever the script defined for each: `tick`
when a frame renders, `forEachCoord` when lights are placed, `modifyLogical` when one coordinate is
folded. An entry a class did not define is simply not called.

This is simpler than a per-role name in every direction. There is no selection, no fallback and no
"which name is mine" question; a binding checks whether the moment it owns is defined and runs it.
Nothing validates which names a class may use, which is what leaves the author in control and
responsible: a script that defines a name no moment calls has a function that does not run, and that
is visible immediately rather than silent. It is also what makes the stretch goal free rather than a
feature: an effect that also defines `modifyLogical` gets both, because it defined both.

It settles step 5 before step 5 starts. The three bindings already differ only by which moments they
own, so there is no inheritance question left to answer, and `tick` stays available to mean something
in a layout or a modifier later without a grammar change.

5. ⬜ **Consolidate the three bindings onto a HELD HELPER.** The design question this step existed
   to answer is settled: the moment model above means the bindings no longer differ in behaviour,
   only in which base they extend and which moment they own. What is left is measurable duplication,
   and the shape it should take is now concrete rather than anticipated.

   **A `MoonLiveScript` MEMBER, not a shared base.** It owns `engine_`, `script_`, the compile-and-
   report path and `defineControls`, and each binding holds one and forwards. A base class was
   re-checked against the code and is still wrong for the same structural reason: the three derive
   from three SIBLING bases under `MoonModule`, so a shared base needs virtual inheritance and would
   change the object layout of every module in the system to serve three of them. A held member
   needs no inheritance change at all.

   **What it removes, measured:** `defineControls` is already byte-identical in the layout and the
   modifier, and the compile trunk is the same in all three; roughly 75 lines of ~537. What stays
   per binding is ~20 lines of genuinely its own: the role virtuals (`forEachCoord`/`lightCount`,
   `modifyLogical`/`modifyLogicalSize`, `tick`/`dimensions`) and the base-class call in `release`.

   **Its own change, with its own bench pass.** Not folded into the language work: these three files
   currently work and are verified on hardware, and mixing a restructure into a grammar change means
   a reviewer cannot tell which broke what. The test of whether it worked is the scripted DRIVER as
   a fourth binding: if it needs more than the member plus its own moment, the factoring did not go
   far enough.

The shape is finished at that point. What follows decides whether MoonLive is an impressive
mechanism or a language people build with.

6. ⬜ **`if` / `else`.** The single largest gap between what MoonLive can express and what an effect
   IS. Today the language does smooth arithmetic over a grid (plasma, ripples, a gradient) and
   nothing that branches, so fire, sparkles, particles, a boundary test, "respawn this one if it
   died" are all unreachable. The stack machine already made this cheap (the predecessor plan's
   table: "a branch over a region; storage is untouched"), and the emitter already has the
   conditional branches the loops use. This is where the language stops being a demo.

7. ⬜ **Reading a light back: `get(x, y)`.** One builtin, and an entire family of effects becomes
   expressible: fire, decay, trails, blur feedback all work by reading what was drawn and modifying
   it. The buffer already persists between frames, which is why every script begins with `fill` to
   clear it, so the data is there and only the read is missing. Needs a decision on how a colour
   comes back: three builtins (`red`/`green`/`blue`) or bit operators, which is the same question
   the seven-argument `line()` answered for arguments and would answer once for both.

8. ⬜ **Arrays, and arrays of structs.** Step 2 designs the storage for it; this is where it works.
   A particle array is the difference between an effect that draws a formula and one that simulates
   something, and it is what most of the effects people ask for are built on. Includes the arena
   ceiling and its diagnostic: an array lets a script ask for more memory than a classic ESP32 has,
   and the answer must be a clear compile error rather than a failed allocation at run time.

9. ⬜ **Wider values than a byte.** Coordinates, members and arena slots are 8-bit, so a script
   cannot address a 256-wide wall correctly, and a modifier cannot walk a light off a large grid.
   This is a correctness wall on exactly the installations worth demonstrating on, and it touches
   the same typed-storage decision as steps 2 and 8, so those three want to agree with each other.

10. ⬜ **The editing loop, which is the thing people will actually see.** Editing a script means the
   File Manager today: find the file, edit it, save it, then re-name it on the module. The demo is
   live authoring, and that wants an editor on the module's own card, saving to the same file the
   engine compiles. Tooling rather than language, and the last step because it is worth building
   against the finished shape rather than twice.

## Files

Per step, the surface each touches. The pattern is that the FRONT END grows and the backends do not:
the shared lowering and the three assemblers are finished work, and a step that needs to change them
is a step whose design is wrong.

- `src/core/moonlive/MoonLiveCompiler.cpp`: every step from 1 to 9 lands here first: the class body,
  function definitions, the member symbol table, `if`, types wider than a byte.
- `src/core/moonlive/MoonLiveIr.h`: new ops as the language grows (a call to a SCRIPT function, a
  conditional branch, a typed member load/store).
- `src/core/moonlive/moonlive_lower.h`: one arm per new IR op, and nothing else. Touching more than
  that means an ISA fact leaked into the language.
- `src/core/moonlive/MoonLive.{h,cpp}`: the arena becomes typed storage (step 2) and gains its
  ceiling (step 8); entry-point discovery lives here (step 1) for the bindings to consume.
- `src/light/moonlive/MoonLive{Effect,Layout,Modifier}.h`: call an entry point instead of running
  the whole program (step 1), then collapse onto dispatch (step 5).
- `src/light/moonlive/MoonLiveBuiltins_light.h`: `addUint8` for step 3, `get`/`red`/`green`/`blue`
  for step 7.
- `src/platform/esp32/moonlive_asm_xtensa.cpp` + `test/unit/core/moonlive_structural.inc`: step 1
  only: a per-function frame means a nested `call8`, so the frame contract and the checker that
  enforces it both extend to script functions.
- `moonlive/**.mlv`: converted in step 1 (mandatory class) and again in step 3 (`defineControls`).
- `docs/moonmodules/light/MoonLive*.md`, `moonlive/README.md`: the language reference, which is what
  a user reads; it moves with each step rather than at the end.
- `src/ui/`: step 10 only.

## Verification

The governing risk is unchanged from the predecessor plan and is what shapes all of this: **only the
host backend is EXECUTED by tests**, while the constraints that bite hardest are Xtensa's. Every step
therefore needs a host test that proves the semantics and a bench run that proves the encoding.

1. **A script calling its own function, and then calling it recursively** (step 1). The recursion case
   is the one that proves a frame per activation, and it is the acceptance test for the step.
2. **The frame contract, extended to script functions** (step 1). The structural checker must refuse a
   script function whose frame intrudes into the window-save reserve, and it must be shown FAILING on
   a deliberately wrong frame before it is trusted: the same control that caught the original bug.
3. **A member written by one function and read by another**, and a member that survives across
   `tick()` calls (step 2). The second is what a stateful effect depends on and is not provable by
   inspection.
4. **The same script at the host's real budget and a squeezed one renders identical pixels.** The
   predecessor plan's technique, still the only way the register work is testable off hardware, and
   every new construct has to keep passing it.
5. **Recursion depth degrades visibly** (step 1): a script that recurses without bound reports an
   error and keeps the device rendering, rather than resetting it.
6. **An arena ceiling reports a compile error** (step 8), not a failed allocation at run time.
7. **The bench, on all four boards**, after each step: S3 and classic (Xtensa), P4 and S31 (RISC-V),
   a scripted layout and a scripted effect. Exec-block sizes compared against the previous step, since
   an unexplained jump is the cheapest signal that codegen went wrong.
8. **`collect_kpi.py` after step 2**, because members change how EVERY variable is accessed. That is
   the one step where a hot-path regression is plausible, so it is measured rather than assumed.

## Deliberately not in this plan

- **Inlining**, per the decision above: it optimises what is not the cost, and its failure mode hangs
  the compiler.
- **`while`, `break`, `continue`.** `for` and `if` cover what an effect does; the rest is language
  completeness rather than expressiveness, and each one costs a grammar rule and a test surface.
- **Floating point.** The render path is integer by rule ([coding-standards](../../coding-standards.md)),
  and the Xtensa classic has no FPU, so a float in a script would be a silent softfloat call per light.
- **A scripted DRIVER as the fourth role.** It is the honest test of step 5's dispatch, but it needs
  the driver surface to be as settled as the other three are, and that is its own question.
