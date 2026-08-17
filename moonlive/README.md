# MoonLive scripts

Scripts for the [MoonLive](../docs/moonmodules/light/MoonLiveEffect.md) engine, one file per script,
grouped by the module that runs it. Name one in a module's `script` control on a running device and
it compiles to native code on the next tick.

Each script declares a **class**, and the host calls its functions: `tick()` today, with the
per-role entry points (`placeLights` for a layout, `modifyLogical` for a modifier) to come. The
class name is independent of the file name, the way a C file and the functions in it are.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLiveLayout](../docs/moonmodules/light/MoonLiveLayout.md) | where the lights physically are — `addLight(x, y, z)` |
| `effects/` | [MoonLiveEffect](../docs/moonmodules/light/MoonLiveEffect.md) | a colour per light: `setRGB(index, r, g, b)`, or a whole shape at once with `line(x1, y1, x2, y2, r, g, b)` |
| `modifiers/` | [MoonLiveModifier](../docs/moonmodules/light/MoonLiveModifier.md) | where one light lands — `setXYZ(0, xPos, yPos, zPos)` |

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
