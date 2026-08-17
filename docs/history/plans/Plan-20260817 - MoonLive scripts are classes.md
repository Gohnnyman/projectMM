# Plan: MoonLive scripts are classes

Takes over from [Plan-20260813 — MoonLive on a stack machine](Plan-20260813%20-%20MoonLive%20on%20a%20stack%20machine%20%E2%80%94%20the%20frame%20is%20where%20values%20live%20(shipped).md),
whose step 7 (factor the three bindings onto one shared base) this replaces. That plan finished the
MACHINE: values live in frame slots, one lowering serves every backend, one system-variable
vocabulary serves every role. This plan changes what a script IS: how it is written (steps 1 to 5)
and what it can say (steps 6 to 10).

**MoonLive launches when all of it is there**, so the order below is the one that is best to BUILD
in, not the one that shows best soonest. Structure first, language second: every feature in the back
half lands on a finished shape instead of being retrofitted into one still moving. The consequence
worth stating out loud is that the most visible work (`if`, reading a light back, particles, the
editor) comes last, which is a deliberate trade rather than an oversight.

## What is missing before it can launch

The engine is not the gap. Live-editing a script on a running device and watching 12,288 lights
change on the next tick, at native speed, is already true, and it is the thing nobody expects from a
microcontroller. The gap is what a script can EXPRESS. Today that is smooth arithmetic over a grid,
which gives plasma, ripples and gradients and stops there: no branch, so no fire, no sparkle, no
particle, no boundary test; no way to read a light back, so no decay and no trails; no array, so
nothing that simulates rather than draws; and a byte-wide coordinate, so a 256-wide wall cannot be
addressed correctly. Recursion, which the stack machine bought and step 1 delivers, is deliberately
not on that list: effect authors do not write recursive functions. It matters because it is what
makes this a real language rather than a macro expander, which is a credibility floor rather than a
feature anyone will point at.

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

## Sequence

**The target these four steps serve: a script IS a class.** Not "like" one loosely: it has members
(script-level variables) and functions, some of which the host calls from outside at known times
(`tick`, `defineControls`, `forEachCoord`). Syntax may be simplified, and semantics may be simplified
where that buys something, but where a reader has an expectation from any class-based language, the
behaviour meets it. That is the standard this design is measured against, and it settles most of the
questions below by saying "what would a class do".

**Prerequisite for all of them: WHERE SCRIPT-LEVEL STATE LIVES.** The moment a script has more than
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

Decide this before step 2 builds it, because `defineControls()` writing a value that `tick()` reads is exactly
the case, and discovering the rules late means rewriting whatever was built on the wrong ones.

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

1. ⬜ **The `class` declaration and script functions, together.** They are one change: making the
   declaration mandatory means there is no bare-statement-list form left, so the grammar's new top
   level is a class body, and a class body holds functions. `tick()` is the first named entry point
   (an effect is the simplest case), and the shipped scripts convert in the same commit, because
   there is nothing to fall back on. First rather than last: with one top-level form, everything
   after it is written inside a class, and converting `moonlive/` twice would be the alternative.
   Calls are real from the start, per the section above: a script calling its own function, and then
   calling it recursively, is the acceptance test.

2. ⬜ **Typed script-level members.** The prerequisite above, built: a variable declared inside the
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

4. ⬜ **The remaining entry points per role**, once the mechanism holds: `forEachCoord`/`lightCount`
   for a layout, `modifyLogical`/`modifyLogicalSize` for a modifier, plus `modifyLogicalTick` (a
   per-drawn-light hook we never implemented; MoonLight has it, and it is what a dynamic rotation
   modifier needs).

5. ⬜ **Dispatch, which is what step 7 was really reaching for.** With entry points named and
   discoverable, what the three bindings share is compile, discover, dispatch. That is a helper or a
   small table, not an inheritance change, and the right structure will be obvious because the
   requirement will be concrete rather than anticipated. The stretch goal falls out of it: a script
   that defines both `tick` and `modifyLogical` is served by a Layer that asks a module which entry
   points it has rather than what type it is.

The structure is finished at that point, and what follows is what a script can SAY. These are the
steps that decide whether MoonLive is an impressive mechanism or a language people build with, and
they come after the structure deliberately: each one lands on finished ground instead of being
retrofitted into a language still changing shape underneath it.

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
