# MoonLive scripts

Scripts for the [MoonLive](../docs/moonmodules/light/MoonLiveEffect.md) engine, one file per script,
grouped by the module that runs it. Name one in a module's `script` control on a running device and
it compiles to native code on the next tick.

Each script declares a **class**, and the host calls its functions: `tick()` for an effect,
`placeLights()` for a layout, `modifyLogical()` for a modifier. A function is called when it is
present and its moment arrives, so which entry points a class defines is what decides what it does.
The class name is independent of the file name, the way a C file and the functions in it are.

A class may also define functions of its own and **call them**, including calling itself:

```
class CrosshairEffect {
  uint8_t bpm = 30;

  defineControls() { addUint8("bpm", bpm, 1, 240); }

  column() { for (y = 0; y < height; y = y + 1) { setRGB(y * width + scale(beat(bpm, t), width), 255, 40, 0); } }
  tick()   { fill(0, 0, 0); column(); }
}
```

These are real calls, not pasted-in text: the callee gets its own frame when it runs, which is what
lets one helper call another and lets a function recurse. A function takes no arguments and returns
nothing yet, so a helper does a whole job rather than computing a value. `effects/crosshair.mle` is
the worked example.

**A declaration is a MEMBER; `defineControls()` decides what the UI shows.** `uint8_t bpm = 30;` is
state the script owns: visible in every function, surviving every tick. Naming it in
`defineControls()` with `addUint8("bpm", bpm, 1, 240)` also puts it on the UI as a slider, which is
the same call a compiled module makes. A member no `addUint8` names stays private to the script,
which is how a stateful effect holds a value the user should not see.

The default comes from the declaration, the range from the call, and the quoted name is the UI
label, free to differ from the member's name.

**A member can be WRITTEN, which is what makes it state.** `level = level + 10;` assigns, and the
value is still there on the next tick, because a member lives in storage that outlives the call. A
loop variable can be assigned too. A [system variable](../docs/moonmodules/light/MoonLiveEffect.md)
(`width`, `t`, `xPos`) cannot: the engine rewrites it before every call, so the store would vanish.

A control CAN be assigned, and the effect is visible rather than surprising: the value moves under
the slider until the user drags it again. Whether a member is a control is decided by
`defineControls()` at run time, so the language does not distinguish the two here.

**`if` and `else`,** with `<`, `<=`, `>`, `>=`, `==` and `!=`. Both sides are ordinary expressions:

```c
if (heat[i] > 40) { setRGB(i, 255, 90, 0); }
else { setRGB(i, 0, 0, 0); }
```

**Members can be wider than a byte, and can be arrays.** `uint8_t` spans 0..255; `uint16_t` spans
0..65535, which is what a position on a wall wider than 255 needs. An array is declared with a
literal length and starts at zero:

```c
uint16_t phase = 900;      // a value a byte cannot hold
uint8_t  heat[16];         // sixteen elements, all zero to begin with
```

An index is an arbitrary expression (`heat[i * 2 + 1]`), and an index outside the array is
**clamped to the last element** rather than refused or allowed through: a script computes indices
from live control values, so out of range is a normal run-time state, and the fixture shows a
repeated last light instead of crashing.

All of a class's members share a small fixed budget (`kCtrlBytes`), so a class that declares more
than fits is a compile error naming the arena, not a failed allocation while a fixture runs.

`effects/ember.mle` is the worked example: a heat array that decays and re-ignites, so what it
draws this frame depends on the last one. That is the line between an effect that evaluates a
formula and one that runs a simulation, and it is the reason arrays exist. `plasma.mle` would look
identical if every frame started from scratch; `ember.mle` would go dark.

**Declare a helper above the function that calls it.** Only functions already parsed are visible, so
a call to one declared further down reports `unknown function`. A function can always call itself.

**Recursion is bounded.** About 30 calls deep, a further call does nothing and returns. A render
task has a fixed stack, so the alternative to a limit is a device that resets mid-frame. What you
see if you hit it is the picture being wrong where the recursion stopped, on a device that keeps
running. Nothing is reported; the exact depth is `kMaxCallDepth`.

**A script's ROLE is its file extension**: `.mle` an effect, `.mll` a layout, `.mlm` a modifier. One
language, three names, the way GLSL uses `.vert`/`.frag` for one shading language. It is what a card
filters its picker on, so an effect card offers effects.

Stated in the name rather than worked out from the file's contents, and deliberately: the entry
point a class defines (`tick`, `placeLights`, `modifyLogical`) already tells the ENGINE which moment
to call, but reusing that as the role would tie a UI filter to a language feature. The day a modifier
wants a per-frame `tick()`, every modifier would start appearing in effect pickers with nothing
changed. The engine stays role-blind either way: it runs whichever moment the binding asks for, so a
class defining several is still legal.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLiveLayout](../docs/moonmodules/light/MoonLiveLayout.md) | where the lights physically are — `addLight(x, y, z)` |
| `effects/` | [MoonLiveEffect](../docs/moonmodules/light/MoonLiveEffect.md) | a colour per light: `setRGB(index, r, g, b)`, or a whole shape at once with `line(x1, y1, x2, y2, r, g, b)` |
| `modifiers/` | [MoonLiveModifier](../docs/moonmodules/light/MoonLiveModifier.md) | where one light lands: `setXYZ(xPos, yPos, zPos)` |

Each module ships one of these as its default, so the folder doubles as the reference for what a
working script looks like.

`unit_MoonLiveScripts` compiles every file here, so a script that stops parsing when the language
changes fails the build rather than waiting to be pasted into a device.

## Debugging: print

`print(v)` writes a value to the serial log and returns it, so it wraps any part of an expression
without changing the result — `addLight(print(xx), yy, 0)` places the same light and tells you what
`xx` was.

**Take it out again when the script works.** A serial write blocks, and a script runs on the render
tick, so a print costs frame time every frame it survives. Each compile grants a short burst and then
goes quiet, which bounds the damage and gives every edit a fresh window; it does not make a print
free. No script in this folder ships with one.
