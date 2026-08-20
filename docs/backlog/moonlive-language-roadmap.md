# MoonLive — language roadmap

> **Forward-looking backlog document — exception to the CLAUDE.md present-tense rule.** Third in a
> family: [livescripts-analysis-top-down.md](livescripts-analysis-top-down.md) designs the ENGINE
> (grammar, IR, codegen), [power-functions-analysis-top-down.md](power-functions-analysis-top-down.md)
> designs the BUILTIN SURFACE the engine calls into. This one is the ordered plan for closing the
> gap between them: which language features to add next, and why that order. Every limit quoted
> below was measured against the shipped compiler, not read off the source.

## What this is for

MoonLive compiles a C-subset to native code on the device. The subset is deliberately small — it
started at "fill a buffer with a colour" and grew a feature at a time. This document is the ordered
list of what to grow next, and why that order.

**The bar is a corpus, not one effect.** MoonLight publishes a body of live scripts at
[MoonModules/MoonLight/livescripts](https://github.com/MoonModules/MoonLight/tree/main/livescripts),
and the goal is that MoonLive can **run them** — not that we migrate them into this repo. They are
written against a language with floats, structs and a wide builtin surface, so they are an honest
external measure of how far the subset still has to grow: each one that compiles unchanged is a
feature below that landed, and each one that does not names the next gap.

The measure throughout is **what a real effect needs**. A simulation effect — particles, physics,
anything where this frame depends on the last — is the honest test, because it exercises state,
arithmetic and control flow together rather than one at a time. `moonlive/effects/balls.mle` is the
worked example: bouncing balls, written against the language as it stands today, and every
compromise it had to make is a line item below.

## The ceilings, measured

Five hard limits, all found by hitting them:

| limit | value | where |
|---|---|---|
| script state | **64 bytes** shared by all members | `kCtrlBytes`, `MoonLiveBuiltins.h:132` |
| distinct members | **8** | `kMaxCtrls`, same file |
| branch labels | **16** (an `if` or `for` takes up to 2) | `kIrLabels`, `MoonLiveIr.h:201` |
| numeric types | `uint8_t`, `uint16_t` | no float, no signed, no division operator |
| ~~builtin table~~ | ~~16, and 16 used~~ → **64** ✅ | `BuiltinTable::kMax` — raised, with an overflow assert |

The branch budget was binary-searched with generated scripts: **6 `if`/`else` + 2 `for` compiles,
7 does not.** The state budget is what caps the balls effect at 4 balls rather than 25 — six fields
per ball needs ~150 bytes and six members.

## What a simulation effect gives up today

Each row is a compromise the balls effect makes, and the language feature that would remove it:

| forced to | because | wants |
|---|---|---|
| 4 objects, not 25 | 64-byte arena, 8 members | a bigger arena |
| whole-pixel motion | no fractional type | fixed-point or float |
| a direction bit per axis | unsigned only | signed values |
| one flat colour | no `hsv()` builtin | `hsv()` |
| a disc, no radial falloff | no `/` operator | division |
| one array per field | no structs | structs |
| the helper reads a member for its index | functions take no arguments | arguments |
| guards folded into `mod()` | 16 branch labels | a bigger label budget |

## The library is already built — the engine is what is missing

This is the part worth stating plainly: **the power-functions library exists and is largely
shipped** (particle kernels, the typed math surface, twelve showcase effects). It was built to make
MoonLive rich, and today almost none of it is reachable from a script.

Its top-down spec already records what it needs from the engine, in
[§ 4 MoonLive requirements](power-functions-analysis-top-down.md) — deferred at the time so the
library could finish compiled-side first. That list is the real roadmap, and it is more specific
than "add a language feature":

1. **A builtin table of ≥ 64 entries.** Today `BuiltinTable::kMax` is **16 — and the light domain
   registers exactly 16.** The table is FULL. `add()` returns false past the limit and nothing
   checks the result, so the seventeenth builtin is dropped silently and the script fails later
   with "unknown function". This is the first thing to fix, and it is a one-line constant plus a
   guard that makes overflow loud.
2. **Typed multi-argument host calls** (≤ 6 args, optional return). Today a builtin takes one
   `uint32_t` and returns one, which is why `line()` had to be given a bespoke seven-argument
   staging path and why most of the library is inexpressible.
3. **Script symbols** `x/y/z/w/h/d/time` — already threaded to the runtime entry point, needing
   only grammar exposure.
4. **Two entry shapes:** `frame()` for composing kernels (the scalable path) and `pixel(x,y,z)`
   for per-pixel ergonomics (honest ceiling around 32×32).
5. **Stateful handles** — a `Pool`, a `BeatPhase` — script-declared, arena-allocated at compile
   time, passed as an opaque first argument. No script-side memory management.

Item 5 is worth noting against the arena work below: a particle pool as a HANDLE means the script
does not spend its own 64 bytes on particle state at all. That is a better answer than widening
the arena, and it is already designed.

## The order, and why

Ordered by **what removing it buys**, not by implementation cost.

### 1. A bigger builtin table — ✅ *shipped*

`BuiltinTable::kMax` was 16 and the light domain registered exactly 16 — **the table was full**,
and it failed SILENTLY: `add()` returned false, no caller checked it, and the next builtin would
have surfaced as "unknown function" in a script with nothing pointing at the cause.

Now 64 (what the power-functions spec asks for), with `MM_ASSERT_NO_BUILTIN_OVERFLOW` so a dropped
registration is loud at startup rather than silent. This gated every other builtin; the palette
work below went in immediately behind it.

### 2. Typed multi-argument host calls — *the library's blocker*

A builtin takes one `uint32_t` and returns one. `line()` needed a bespoke seven-argument staging
path to exist at all, and most of the power-functions surface cannot be expressed without this.
The spec asks for ≤ 6 arguments plus an optional return.

Doing #1 and #2 together is what actually opens the library; either alone leaves it stranded.

### 3. A bigger arena and more members — *and check the handle route first*

64 bytes across 8 members is why an effect holds four objects rather than twenty-five.

**But check the handle route first.** The power-functions spec's item 5 — a particle pool as an
arena-allocated HANDLE — means a simulation effect stops storing its own particle state entirely,
which removes the pressure without touching these constants. Widen the arena for the scripts that
genuinely hold their own state; do not widen it as a substitute for handles.

Not purely a constants bump, and the blockers are known:

- `kCtrlBytes` / `kMaxCtrls` are both `uint8_t`, so the arena caps at 255 bytes before any type
  change. 150 bytes of particle state fits under that; much more does not.
- `static_assert(kCtrlBytes <= 64, "seeded_ is a 64-bit mask, one bit per script arena byte")`
  (`MoonLive.h:285`) is the real gate. Past 64 bytes the seeded-member mask needs re-indexing —
  and there is a worked example, because it was widened 16 → 64 once already. The assert exists
  because the earlier `uint32_t` version silently aliased members mod 32.
- Watch `sizeof(MoonLive)`. It is held BY VALUE in every scripted module and constructed on the
  main task's stack by `registerType`'s probe, which is what boot-looped the P4 at 1440 bytes.
  Growing the arena grows every scripted module.

### 4. `setPaletteColor()` — ✅ *shipped*

A script now writes one call where it used to write three:

```
setPaletteColor(x, y, index, brightness);
```

`paletteR/G/B(i, bri)` shipped first and worked, but the shape was wrong: three host calls per
pixel, and — because the compiler evaluates each argument independently — three evaluations of
whatever expression produced the brightness. A per-pixel radial falloff made that visible.

**Measured on an S3, 64x64 grid: 2331 us → 1838 us, 21% off the effect's own cost**, and the call
site went from three copies of a six-term expression to one. It also takes x/y rather than a flat
index, so the buffer layout (`mod(x, width) + mod(y, height) * width`) stops being open-coded in
every script.

No engine change was needed: `line()` already proved a Call builtin can write pixels through the
per-run draw canvas, so this rode the existing seam. `paletteR/G/B` stay for a script that needs
the components rather than a pixel.

What is still missing is the general form — a builtin RETURNING a colour, so a script can hold one
in a variable and pass it on. That is #2's multi-value ABI, and #4b is what makes it worth having.

### 4c. Insights from porting a per-pixel effect — *the cost is CALLS, not maths*

A second port (`moonlive/effects/octopus.mle`, a polar spiral) measured very differently from the
first, and the difference is the useful part.

| effect | shape | S3, 64x64 |
|---|---|---|
| balls | four small discs, ~500 lit pixels | **1278 us** |
| octopus | every pixel, every frame | **21762 us** |

That is ~5.3 us per pixel, and it is not the arithmetic — it is **~32,000 host calls per frame**.
Each `polarA`/`polarR`/`sin`/`scale`/`beat` is a real call through the builtin ABI, and a
whole-canvas effect makes eight or so per pixel.

Three things follow, and they sharpen the priorities above:

1. **No common-subexpression elimination.** `polarR(x - cx, y - cy)` appears twice in one
   expression and is CALLED twice. The compiler has no way to name an intermediate, so a script
   cannot hoist it either — the same limitation that made a palette pixel cost three brightness
   evaluations before #4. A single-assignment local (part of #4b/#6's territory) would remove a
   third of this effect's calls on its own.
2. **Per-pixel builtins want to be inline ops, not calls.** `setPaletteColor` is a Call and that is
   fine at ~500 pixels; at 4096 the call overhead dominates. The `Inline` kind already exists
   (`setRGB` uses it) — the polar pair and the palette write are the candidates.
3. **A `frame()` entry shape would sidestep it entirely.** The power-functions spec's item 4 —
   compose kernels over a whole frame rather than script per pixel — is exactly the answer for
   this class of effect, and octopus is the concrete case that argues for it.

Worth stating plainly: 35 fps for a full-canvas 64x64 effect is usable, and the port needed no
new language features beyond two builtins. The ceiling here is call overhead, not expressiveness.

**A naming lesson, learned by breaking two scripts:** the polar builtins were first called
`angle`/`radius`, which silently broke `ring.mll` and `rose.mll` — both declare a `radius` member,
and a builtin shadows a member name. Builtins share one namespace with every script's own
variables, so a new builtin should take a name a script would not: `polarA`/`polarR` rather than
the obvious ones. The compile-every-script test caught it immediately, which is the argument for
keeping that test cheap to run.

### 4b. Two predefined structs: `Coord3D` and `CRGB` — *and they make #4 land properly*

Most of what a script manipulates is a POSITION or a COLOUR, and today both are loose integers: a
coordinate is three separate values or an index the script computes by hand
(`mod(bx+dx, width) + mod(by+dy, height) * width`), and a colour is three `uint8_t`s that cannot
travel together. Two predefined types would carry them:

- **`Coord3D`** — `{x, y, z}`, the shape `setXYZ`, `addLight` and every layout already think in.
- **`CRGB`** — `{r, g, b}`, the FastLED name, matching `RGB` in `core/color.h`.

Predefined rather than user-declarable structs (#10): these two are what the ENGINE already passes
around, so they need no general struct machinery — just two known layouts the compiler understands
and the builtins can take and return.

The payoff is that #4 becomes the natural signature rather than a special case:

```
setColorFromPalette(pos, index, brightness);     // pos is a Coord3D
```

One call, one brightness evaluation, and the index arithmetic stops being open-coded at every call
site. It also removes the `mod(...) + mod(...) * width` flattening a script writes today, which is
the buffer layout leaking into every effect.

Worth noting the ordering: this only pays off with the multi-value call ABI from #2, and it is what
makes that ABI worth having beyond colour — a `Coord3D` in and a `CRGB` out is the shape most of
the power-functions surface wants.

### 5. Fractional arithmetic — *expensive, and the real unlock*

Every remaining visual compromise traces back to this: smooth motion, real velocities, a bounce
that conserves speed, and any falloff term that makes a shape look round rather than flat. Without
it each is a separate workaround.

Two routes, and the choice matters more than the schedule:

- **Fixed-point** (a `q8_8` over the existing `uint16_t`): no new backend work — it lowers to the
  integer ops all three ISAs already have. Enough for positions, velocities and most ramps.
- **True `float`**: closer to a precompiled effect's source, which is the near-verbatim porting
  goal the top-down analysis sets. Costs an FPU path per backend; the P4 and S3 have one, the
  classic ESP32 does not, so it would soft-float or refuse.

Fixed-point first is the pragmatic order: most of the quality at a fraction of the cost, and it
does not foreclose float.

### 6. Function arguments — *moderate, removes a real footgun*

`draw(i)` instead of setting a member the helper reads. The current shape is not just verbose:
caller and callee agree by convention and nothing checks it, so a helper called from two places
with different state silently does the wrong thing. It is also what makes helpers composable.

### 7. Signed values — *moderate, and it removes a whole class of workaround*

Unsigned-only forces a sign bit alongside every value that can go negative — a velocity, a delta,
an offset from a centre. It also makes ordinary expressions dangerous: `a - b` wraps instead of
going negative, so scripts guard every subtraction. Comes naturally with fixed-point (#3) if that
type is signed, which argues for doing them together.

### 8. More branch labels — *probably a constant, worth measuring first*

16 is tight enough that a straightforward nested draw does not fit. Raising `kIrLabels` costs
compile-time table space and nothing at run time. Measure what a realistic effect needs before
picking a number — the balls port wanted ~12 and had to be folded down.

### 9. Division — *narrow, but some maths needs it*

`mod` and `scale` cover the cyclic cases, so this is mainly for ratios and falloff. Note that no
ISA here has a cheap integer divide, so it lowers to a host call the way `mod` already does: fine
on a cold path, questionable per-pixel. Worth documenting that cost at the call site.

### 10. Structs — *readability, once the arena is bigger*

`ball[i].x` instead of parallel arrays. Genuinely nicer and closer to how a precompiled effect
reads, but parallel arrays work the moment the arena is big enough. Last because #1 removes most
of the pain, not because it does not matter.

## How to know a step landed

Two measures, one local and one external.

**Local:** re-port a simulation effect after each step and delete the row it removes from the
compromise table above. A port with no rows left — dozens of shaded, coloured objects with
fractional velocities on a full-size grid, at a frame rate the bench can measure — means the
language carries a real simulation.

**External, and the harder bar:** take the
[MoonLight livescripts corpus](https://github.com/MoonModules/MoonLight/tree/main/livescripts) and
count how many compile and run unchanged. That number is the honest progress metric, because those
scripts were written without regard for our subset. Track it per step; a feature that moves it is
worth more than one that does not, whatever this document guesses.
