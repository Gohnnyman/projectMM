# MoonLive scripts

Scripts for the [MoonLive](../docs/moonmodules/light/MoonLiveEffect.md) engine, one file per script,
grouped by the module that runs it. Paste one into a module's `source` control on a running device
and it compiles to native code on the next tick.

| folder | run by | a script writes |
|---|---|---|
| `layouts/` | [MoonLiveLayout](../docs/moonmodules/light/MoonLiveLayout.md) | where the lights physically are — `addLight(x, y, z)` |
| `effects/` | [MoonLiveEffect](../docs/moonmodules/light/MoonLiveEffect.md) | a colour per light — `setRGB(index, r, g, b)` |
| `modifiers/` | [MoonLiveModifier](../docs/moonmodules/light/MoonLiveModifier.md) | where one light lands — `setXYZ(0, x, y, z)` |
| `drivers/` | — | nothing yet; a scripted driver has no binding |

Each module ships one of these as its default, so the folder doubles as the reference for what a
working script looks like.

`unit_MoonLiveScripts` compiles every file here, so a script that stops parsing when the language
changes fails the build rather than waiting to be pasted into a device.

## How big a script can be

A script's size is bounded by the device's free memory, not by a fixed budget: the compiler sizes
its working buffers to the source it is given. The `source` control holds 4 KB of text, which is a
few hundred statements — well past what a layout or effect needs. A script that outgrows the device
fails to compile with a diagnostic; it never truncates silently.

That 4 KB is held per scripted module whether the script fills it or not, which is a few percent of a
classic ESP32's free RAM per module. Shrinking it is backlogged.

## Debugging: print

`print(v)` writes a value to the serial log and returns it, so it wraps any part of an expression
without changing the result — `addLight(print(xx), yy, 0)` places the same light and tells you what
`xx` was.

**Take it out again when the script works.** A serial write blocks, and a script runs on the render
tick, so a print costs frame time every frame it survives. Each compile grants a short burst and then
goes quiet, which bounds the damage and gives every edit a fresh window; it does not make a print
free. No script in this folder ships with one.
