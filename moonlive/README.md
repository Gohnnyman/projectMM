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
  uint8_t bpm = 30;   // @control 1..240

  column() { for (y = 0; y < height; y = y + 1) { setRGB(y * width + scale(beat(bpm, t), width), 255, 40, 0); } }
  tick()   { fill(0, 0, 0); column(); }
}
```

These are real calls, not pasted-in text: the callee gets its own frame when it runs, which is what
lets one helper call another and lets a function recurse. A function takes no arguments and returns
nothing yet, so a helper does a whole job rather than computing a value. `effects/crosshair.mlv` is
the worked example.

**Declare a helper above the function that calls it.** Only functions already parsed are visible, so
a call to one declared further down reports `unknown function`. A function can always call itself.

**Recursion is bounded.** About 30 calls deep, a further call does nothing and returns. A render
task has a fixed stack, so the alternative to a limit is a device that resets mid-frame. What you
see if you hit it is the picture being wrong where the recursion stopped, on a device that keeps
running. Nothing is reported; the exact depth is `kMaxCallDepth`.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLiveLayout](../docs/moonmodules/light/MoonLiveLayout.md) | where the lights physically are — `addLight(x, y, z)` |
| `effects/` | [MoonLiveEffect](../docs/moonmodules/light/MoonLiveEffect.md) | a colour per light: `setRGB(index, r, g, b)`, or a whole shape at once with `line(x1, y1, x2, y2, r, g, b)` |
| `modifiers/` | [MoonLiveModifier](../docs/moonmodules/light/MoonLiveModifier.md) | where one light lands: `setXYZ(0, xPos, yPos, zPos)` |

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
