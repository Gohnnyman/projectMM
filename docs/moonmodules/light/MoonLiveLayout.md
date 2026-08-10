# MoonLiveLayout

A **layout written as a live script**: where the lights physically are, authored as text on a running device instead of compiled in as a C++ class. Same [MoonLive](MoonLiveEffect.md) engine as a scripted effect or [modifier](MoonLiveModifier.md), pointed at the third job.

A [layout](layouts.md) is the one part of the pipeline that differs for every physical build — a ring, a spiral staircase, a car grille, a costume sewn last night. Each one has meant writing a C++ class, rebuilding and reflashing. A script means the person who hung the lights can describe where they went, on the device, and see it immediately.

<img src="../../assets/light/MoonLiveLayout.png" width="300" alt="MoonLiveLayout">

## Writing one

The script places every light itself, with a loop. That is the difference from a scripted modifier: the Layer calls a modifier once per light, so its script transforms a single coordinate — a layout has no such per-light call to ride on.

```c
uint8_t cols = 16;  // @control 1..64
uint8_t rows = 16;  // @control 1..64

for (y = 0; y < rows; y = y + 1) {
  for (x = 0; x < cols; x = x + 1) {
    addLight(x, y, 0);
  }
}
```

That is the default: a plain grid, one light per cell. `addLight(x, y, z)` places the next light along the strand — no index, because the order the script calls it in *is* the strand order.

The `cols` and `rows` lines are the script's own controls, not something the module hands it. A layout is never told how big it is: the pipeline works out the bounding box from the coordinates the layouts actually place, so a size passed in from outside would be a second answer that could disagree with the first.

They are named `cols`/`rows` because `width`, `height` and `depth` are [system variables](MoonLiveEffect.md#system-variables--what-the-engine-hands-a-script) — the logical grid the Layer hands an effect or a modifier. A layout is upstream of that grid, so it names its own controls.

A few shapes that are one line here and a new class otherwise:

```c
// a strand that runs right to left
for (i = 0; i < cols; i = i + 1) { addLight(cols - 1 - i, 0, 0); }

// a diagonal
for (i = 0; i < cols; i = i + 1) { addLight(i, i, 0); }

// two rows, stacked
for (i = 0; i < cols; i = i + 1) { addLight(i, 0, 0); addLight(i, 1, 0); }

// a circle: lights and grid cells are not the same number
uint8_t count = 24;   // @control 3..255
uint8_t radius = 5;   // @control 1..127
for (i = 0; i < count; i = i + 1) {
  addLight(scale(cos(i * turn(count)), radius * 2 + 1),
           scale(sin(i * turn(count)), radius * 2 + 1), 0);
}
```

### What a script can read

A script reads whatever it declares. `uint8_t cols = 16; // @control 1..64` becomes a real slider in the UI, and the loop reads it — which is how a panel gets resized without editing code.

`t` is the one [system variable](MoonLiveEffect.md#system-variables--what-the-engine-hands-a-script) a layout is given, and it is always **0** here: the script runs twice per rebuild (once to count, once to place) and must agree with itself, so it is handed a fixed clock rather than a live one — a moving `t` would let the two passes disagree on how many lights there are. `width`/`height`/`depth` name the grid a layout is *defining*, so asking for one is a compile error rather than a silent zero; `x` and `y` are free to use as loop counters.

### Seeing inside a script

`print(v)` logs a value and returns it, so it wraps any part of an expression: `addLight(print(x), y, 0)`.
It is for debugging and comes back out again — [what print costs](../../../moonlive/README.md#debugging-print).

## How the count is known

A layout has to answer **how many lights** before it produces a single coordinate — the Layer sizes its buffer from that number and only then asks where each light is. A script cannot be asked "how many?" without running it.

So it runs twice. On the first pass `addLight` counts; on the second it emits each position to whoever asked. Same script, same arithmetic, so as long as the script is deterministic the two answers cannot drift apart — which is exactly what the compiled layouts do (`SphereLayout` walks its shell twice for the same reason). A script that calls `random16` breaks that condition. The two passes disagree on the COUNT only when the random value decides a loop bound or how many times `addLight` runs; a random COORDINATE keeps the count right and simply places the lights somewhere else on the second pass, so the fixture is the size it claims but not the shape. See [Limits](#limits).

**Nothing is stored between the passes.** Staging 16,384 coordinates would cost 48 KB, which a classic ESP32 driving that many lights does not have spare. Running the script again is cheaper than remembering what it said, and it means a scripted layout costs the same as a compiled one: the JIT'd program, and nothing that grows with the light count.

## Limits

**The grammar is arithmetic, calls and `for`** — `+`, `-`, `*`, parentheses, nested loops. Division, `%` and `if` are not in the language yet, so a serpentine over an arbitrary number of rows (every other row reversed) is not expressible today. A fixed few rows can be written out as one loop per direction — `two-rows.mlv` does exactly that — but each row costs its own loop, so it does not scale to a panel.

**A script runs twice per rebuild**, once to count and once to place, so it has to be deterministic. With `random16` in a loop bound or around an `addLight` call, the two passes disagree on the count; with `random16` in a coordinate, the count holds and only the positions move.

## Controls

| control | what it does |
|---|---|
| `source` | the script; editing it recompiles and re-places the lights live |

Plus one control per `@control` the script declares.

Editing any of them rebuilds the pipeline, because every one can change where the lights are. A script that fails to compile leaves a fixture with no lights, shows the parse error on the module, and the device keeps running.

Detail: [technical](moxygen/MoonLiveLayout.md)
