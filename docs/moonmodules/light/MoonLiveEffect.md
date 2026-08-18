# MoonLive

MoonLive is projectMM's **live-script engine** — author an effect as text and run it on a running device, compiled to native machine code so it executes at near-hand-written speed in the render hot path. The broader design lives in [livescripts-analysis-top-down.md](../../backlog/livescripts-analysis-top-down.md) (a backlog design study); this page documents the module.

Scripts call the same [power functions](power-functions.md) compiled effects use, reached through the builtin table — so the vocabulary is shared, in its flat scalar form.

A scripted effect names a **script file** under `/moonlive/`; the UI loads, edits and saves that file, and the module holds only the name (~32 bytes) — the text is read into a right-sized buffer to compile and freed immediately, so nothing script-sized stays resident. A front-end (lexer → parser → IR → per-ISA assembler) compiles it to native code on the next tick.

**A script is a class.** It declares one, and the host calls its functions: an effect's `tick()` runs once per frame. That is the same shape a compiled effect has, so what a contributor learns from one transfers to the other.

```
class RandomPixelEffect {
  tick() {
    setRGB(random16(256), 0, 0, 255);   // a random pixel, blue
    setRGB(5, random16(256), 0, 0);     // pixel 5, a random red
  }
}
```

Inside a function the grammar is a sequence of **statements** — a function call, or a `for` loop over them — with **expression arguments**, so any argument may be a literal or a nested call. The class declaration is required: one top-level form rather than two means one set of rules to learn and one parse path to maintain.

The **class name is not the file name**. `plasma.mlv` may declare `class PlasmaEffect`; the file is what the engine loads, the class is what diagnostics and the module status report. Renaming either leaves the other alone, the same way a C translation unit and the functions inside it are independent.

The functions are **not built into the compiler** — `setRGB`, `fill`, `random16` are registered by the *host* (the light domain) in a builtin table; the core compiler owns only the grammar and a generic call/inline mechanism (the ESPLiveScript / ARTI bound-function model). The compiler emits machine code for whichever ISA the device runs (Xtensa on the classic/S3) or the host ISA on desktop, places it in executable memory, and the engine calls it each render tick.

## Controls

- `script` — the file name under `/moonlive/`, e.g. `lines.mlv`. A fresh module has none: it reports `no script — set the script name` and renders nothing, rather than every new module compiling the same default. Naming one (or re-naming it after an edit) recompiles live: a valid script swaps in on the next tick; a failed compile frees the old code, shows the diagnostic in the module status, and renders dark until fixed (the script-editor loop, robust + no reboot). The directory is created on demand.
- **Scripted controls**: a script declares members, then says which of them the UI shows by calling `addUint8` inside a `defineControls()`, the same call a compiled module makes. Each becomes a real `uint8` MoonModule control (slider + UI + persistence), bound to a live value the running native code reads each tick:

  ```c
  class SpeedyEffect {
    uint8_t speed = 50;
    uint8_t hue   = 128;
    uint8_t phase = 0;          // a member, not a control: the UI never shows it

    defineControls() {
      addUint8("speed", speed, 0, 99);
      addUint8("hue", hue, 0, 255);
    }

    tick() { setRGB(speed, hue, phase, 255); }
  }
  ```

  A declaration sits in the class body, not inside a function: it is a **member**, visible in every
  function and surviving every call. That is the whole of what a declaration means, and whether the
  UI shows one is the separate question `defineControls()` answers. A member no control names is
  simply the script's own state.

  The compiled form is the same call with a receiver: `controls_.addUint8("speed", speed, 1, 255)`. The member is named by identifier rather than by repeating the string, so a typo is a compile error here as it is there, and the quoted name is the UI label, free to differ from the member's name. The **default** comes from the member's initializer, so there is one home for the starting value. The range arguments are ordinary expressions, like every other argument in the language: `addUint8("speed", speed, base, base * 4 + 5)` is valid.

  `defineControls()` runs once after a successful compile, the way the Scheduler runs a compiled module's. Editing a control's slider does **not** recompile: the value lands in the engine's control-values arena and the next render tick reads it (the live-edit guarantee, the *no-reboot* principle). Saving the script and re-naming it recompiles and re-derives the control set; a control kept across the edit keeps its slider value, a removed control's saved value drops. Stage 1 is `uint8` only.

### System variables — what the engine hands a script

Some names are **reserved**: the engine defines them, the script only reads them, and a declaration that reuses one is a compile error (`name is a system variable`). **One vocabulary serves every role** — a name means the same thing in a layout, an effect and a modifier — so what you learn from one script transfers to the next.

| name | what it is |
|---|---|
| `t` | elapsed milliseconds — the clock an animation is written against |
| `width`, `height`, `depth` | the **logical grid**, `0..255` |
| `xPos`, `yPos`, `zPos` | the light being transformed, `0..255` (a [modifier](MoonLiveModifier.md) is the one handed these; elsewhere they read 0) |

Every one but `t` is a byte, because it lives in the controls arena. A grid extent past 255 reports 255 rather than wrapping to a small number, and a modifier handed a coordinate outside `0..255` passes it through untransformed instead of folding a wrong position — so a script never silently sees a value that means something else.

The coordinate is `xPos`/`yPos`/`zPos` rather than `x`/`y`/`z` so that **`x` and `y` stay free as loop counters in every script**, which is what an author reaches for and what the shipped `grid.mlv` uses. Reserving them globally would break the most ordinary code there is; a per-role reservation was the alternative and was worse, because a name then meant one thing in one role and was refused in another — which is how `disasm.py`, compiling against the widest vocabulary, came to refuse the shipped default layout.

`width`/`height`/`depth` are the Layer's own dimensions, derived from the layouts and the modifier chain. An effect is *told* its canvas rather than declaring it: a size restated as a control is a second answer that can disagree with the first, and a script that sets `width` to 16 on an 8×8 panel draws off the edge. A [layout](MoonLiveLayout.md) is upstream of that grid — it is what the dimensions are derived *from* — so it names its own controls instead (`cols`, `rows`) and reads the grid only if it has a use for it.

Reserving is what makes the guarantee hold: without it a declaration would silently shadow the value the engine handed in, and the script would disagree with its layer with no error anywhere.

### The vocabulary — what a script can call

Registered by the light domain, not built into the compiler (the core owns only the grammar and a generic call/inline mechanism), so the list is one edit in `MoonLiveBuiltins_light.h`.

| call | does |
|---|---|
| `setRGB(index, r, g, b)` | write one light |
| `setXYZ(index, x, y, z)` | write one position (a [modifier](MoonLiveModifier.md)) |
| `fill(r, g, b)` | write every light |
| `addLight(x, y, z)` | place the next light (a [layout](MoonLiveLayout.md)) |
| `line(x1, y1, x2, y2, r, g, b)` | a straight segment on the grid, via the shared `draw::line` |
| `random16(n)` | a value in `[0, n)` |
| `mod(a, b)` | `a % b` — the wrap a cyclic animation needs |
| `beat(bpm, t)` | a `0..65535` sawtooth at `bpm` |
| `beatsin(bpm, t, high)` | a sine `0..high` at `bpm` |
| `scale(value, n)` | a `0..65535` value onto `0..n-1` — lands a wave on an axis |
| `sin(angle)`, `cos(angle)` | the circle; one turn is `0..65535`, result biased to `1..65535` centred at 32768 |
| `turn(n)` | one revolution split `n` ways — the angle step for placing `n` points on a circle |
| `print(v)` | log a value and return it ([what it costs](../../../moonlive/README.md#debugging-print)) |

`sin`/`cos` return an **unsigned** wave, so a coordinate comes from scaling by the full span and not by half of it: `scale(cos(a), radius * 2 + 1)` sweeps a whole axis, where scaling by `radius` alone would only ever reach one side of centre.

`turn(n)` exists because a full revolution is 65536 — one past the largest number a script can write — and the grammar has no division. Without it, placing `n` points evenly on a circle is not expressible.

### The script's own functions

A class may define functions beside its entry point and call them, including calling itself. `effects/crosshair.mlv` is the worked example: a `column()` and a `row()`, both called from `tick()`.

These are real calls, not text pasted in by the compiler: the callee allocates its own frame when it runs, which is what lets one helper call another and what makes recursion work. A function takes no arguments and returns nothing yet, so a helper does a whole job rather than computing a value.

Two rules a script author meets:

- **Declare a helper above the function that calls it.** Only functions already parsed are visible, so a call to one declared further down reports `unknown function`. A function can always call itself.
- **Recursion is bounded.** About 30 calls deep a further call does nothing and returns, because a render task has a fixed stack and the alternative to a limit is a device that resets mid-frame. It is not reported: what you see is the picture being wrong where the recursion stopped, on a device that keeps running.

### Wire contract — control declaration

The controls are **declared by the script** (one per `addUint8` call in its `defineControls()`), then **surfaced in `/api/state`**, the device JSON view the integrator consumes, as regular `uint8` controls alongside `script`. So an integrator sees and writes them exactly like any other control — e.g. `POST /api/control` with `{"module": "ML", "control": "speed", "value": 80}`; they're fully present in the device JSON, just authored in the script rather than fixed in the module. The script's `\n` line breaks are standard JSON string escapes the device decodes, so a multi-line script round-trips through `/api/file`.

## Pieces

- **`MoonLive`** (`src/core/moonlive/MoonLive.h/.cpp`) — the **domain-neutral engine core**. Owns a block of executable memory; `compile(source, table)` runs the front-end against a host builtin table and places the emitted code, `run(buf, nLights, cpl, t)` calls it. Includes only `<cstdint>`, the compiler/emitter seams, and the platform seam — never `EffectBase`, `Buffer`, or any LED type.
- **`MoonLiveBuiltins`** (`src/core/moonlive/MoonLiveBuiltins.h`) — the **neutral host-binding seam**: a `BuiltinTable` of `{name → descriptor}`, where a descriptor is either `Call` (a host C function pointer — a pure helper like `random16`) or `Inline` (a neutral opcode tag the backend emits inline — the hot-path buffer writers, no per-pixel call). The core owns no function names; it resolves a call against whatever the host registered.
- **`MoonLiveCompiler`** (`src/core/moonlive/MoonLiveCompiler.h/.cpp`) — the **platform-independent front-end**: a recursive-descent lexer + expression parser that lowers each statement to the typed IR (`MoonLiveIr.h`). Pure (source + table in, IR out, deterministic). Knows the *language*, never an ISA and never a domain.
- **`MoonLiveBuiltins_light`** (`src/light/moonlive/MoonLiveBuiltins_light.h`) — the **light-domain registration**: the only place the LED vocabulary lives. Registers the whole vocabulary above — Inline ops lowering to stores, and Calls into host helpers — plus the system variables each binding supplies. A different host (display, sensor) writes its own table; the core is unchanged.
- **per-ISA assembler + lowering** (`src/platform/<target>/moonlive_asm_*` + `moonlive_lower_*`): a tiny named-instruction MacroAssembler with label back-patching, and the IR→bytes lowering that drives it. Xtensa for the classic/S3 (`__XTENSA__`), the host ISA on desktop (arm64/x86-64). Adding an ISA is a new assembler + lowering; the front-end and IR are unchanged. (`emitFill`/`emitAnimatedFill` remain as the hand-encoded `fill` references the assembler's output is checked against.) An ISA also brings its own **frame contract**, which the emitter honors before a single instruction matters: on Xtensa the top 32 bytes of every frame belong to the register-window spill hardware, enforced by a `static_assert` tied to the widest call emitted plus the structural codegen test ([why, and how it was found](../../history/lessons.md#lessons-from-the-moonlive-on-xtensa-branch-the-register-window-frame-bug)).
- **`MoonLiveEffect`** (`src/light/moonlive/MoonLiveEffect.h`) — the **thin binding**: a first-class `EffectBase` carrying the `script` control, whose `tick()` delegates to the engine over its own `buffer()`. `compile(source, table, sysvars)` takes both host tables: the shared `lightBuiltins()`, and the system variables THIS binding supplies — `effectSysVars()` here, `modifierSysVars()` for a modifier, `layoutSysVars()` for a layout, which is what decides the names each kind of script can read and cannot declare. The engine is projectMM-agnostic; the binding is the only coupled layer.

## Cross-domain wiring

- **The executable-memory seam** is new platform surface (`src/platform/platform.h`): `allocExec(size)` / `freeExec(ptr,size)` allocate memory the CPU can *fetch* from (ESP32 IRAM via `MALLOC_CAP_EXEC`; an `mmap` `PROT_EXEC` page on desktop, with macOS-arm64 `MAP_JIT` + a write-protect toggle), and `writeExec(dst,src,len)` copies emitted code in safely — on ESP32 that means 32-bit-aligned IRAM stores plus an instruction-cache sync so the core fetches fresh code, not stale cache. All ISA/cache quirks live behind these three functions; the engine stays target-agnostic.
- **The producer buffer**: the emitted routine writes the same `buffer()` + `nrOfLights()*channelsPerLight()` surface a compiled effect writes — the identity-mapping fast path, no intermediate copy. The binding hands the engine `(buffer(), nrOfLights(), channelsPerLight())` each tick.
- A failed compile (no executable memory) leaves the effect `!ok()`: it renders dark and reports the error in its module status — the device keeps running (robustness, no reboot).

## Prior art

MoonLive's native-codegen approach — compile a small C-like language straight to machine code and call it as a function, so a live-authored effect runs at near hand-written speed — was pioneered by **Yves Bazin (hpwit)** in **[ESPLiveScript](https://github.com/hpwit/ESPLiveScript)**: a from-scratch tokenizer, parser, and Xtensa code generator that drives a 12,288-LED panel at ~85 fps where interpreted languages (Lua, Gravity) managed 3–10. That result is what makes "go native, not interpreted" the right call, and ESPLiveScript is the reference MoonLive is built against — studied closely, credited, and written fresh against projectMM's architecture, never copied, per [*Industry standards, our own code*](../../../CLAUDE.md#principles). The live-scripting idea in this ecosystem also descends from **ARTI-FX / ARTI** (the interpreted-effects runtime in WLED MoonModules), which proved the load-a-script-and-run-it-live loop end to end. The host-binding surface (`setRGB`/`setRGBXY`/`setRGBXYZ`) is modelled on the **MoonLight** [effects tutorial](https://moonmodules.org/MoonLight/moonlight/effects-tutorial/).

## Tests

[unit_moonlive_fill](../../../test/unit/core/unit_moonlive_fill.cpp) runs the engine path in-process on the desktop host backend (`compile`/`run`, the animated routine, zero-lights, recompile, `free`, the `allocExec`/`writeExec`/`freeExec` round-trip, the buffer-shape guards). [unit_moonlive_ir](../../../test/unit/core/unit_moonlive_ir.cpp) pins the **behavioral golden** — a compiled `fill` and the hand-encoded reference render an identical buffer — plus setRGB's single-pixel write and the runtime bounds guard. [unit_moonlive_compiler](../../../test/unit/core/unit_moonlive_compiler.cpp) pins the expression grammar (`random16` in any/every argument slot, uint16 bounds), the parser diagnostics (no crash on malformed input), live recompile, and the **domain-neutral** property: with an empty builtin table the core knows *no* functions, and a host can register an arbitrary name against the same machinery.

The grammar + bounds guard are verified live on the S3/Olimex (Xtensa) by saving a script file and naming it — the device compiles the expression on-chip and renders it.

[scenario_MoonLiveEffect_livescript](../../../test/scenarios/light/scenario_MoonLiveEffect_livescript.json) exercises the effect **as a wired MoonModule** — what the unit tests can't reach: add it, live-edit the script file to recolor (recompile), push a broken script (`MoonLive::compile` fails, frees the previous code, `MoonLiveEffect` reports the parse error in the status and renders dark — no crash), recover, resize the grid to 1×1 and back while rendering (the every-grid-size hard rule), then remove and re-add (exec memory re-acquired clean). It runs in-process on the desktop backend each commit, and the same JSON runs live over REST against the device backends. The Xtensa/RISC-V backends are validated by the live S3/P4 runs (a `MoonLiveEffect` on a Layer lights the grid from its script file), which the desktop tests can't reach.

## Source

[MoonLive.md](../core/moxygen/MoonLive.md) · [MoonLiveBuiltins.md](../core/moxygen/MoonLiveBuiltins.md) · [MoonLiveCompiler.md](../core/moxygen/MoonLiveCompiler.md) · [MoonLiveIr.md](../core/moxygen/MoonLiveIr.md) · [MoonLiveEffect.md](moxygen/MoonLiveEffect.md)
