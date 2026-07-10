# Build your own MoonModules

A hands-on guide to writing your own light **effects** (and later layouts, modifiers, and drivers) for projectMM. It's written for developers new to the codebase — including as a practical class in a school or workshop. If you can write a `for` loop in C++, you can write an effect.

By the end you'll understand the one idea that makes modules easy here: **you write *what* your module does; the core decides *when* to run it.** You fill in a few functions; the engine handles lifecycle, threading, and memory timing, and calls your functions at the right moment.

## Everything is a MoonModule

The building block of the whole system is the **MoonModule**. An effect is a MoonModule. So is a layout, a modifier, a driver, the WiFi manager, the file browser — *everything*. A MoonModule is just a small C++ class with a few **hook functions** the core knows how to call, plus some **controls** (the sliders and toggles the user sees). That's the entire contract. Learn it once and you can build any kind of module.

The four light-domain kinds you'll write are all MoonModules with a tiny bit extra:

- an **effect** derives from `EffectBase` (a MoonModule that gets a pixel buffer to draw into),
- a **layout** derives from `LayoutBase` (a MoonModule that reports where each LED sits in space),
- a **modifier** derives from `ModifierBase` (a MoonModule that bends/masks the image),
- a **driver** derives from `DriverBase` (a MoonModule that pushes the image to hardware).

Each base just pre-fills the hooks specific to that job, so you fill in even less. Under all of them is the same `MoonModule` with the same lifecycle — which is why, once you've written an effect, every other kind feels familiar.

## The big picture in one minute

A projectMM light show is a small tree of MoonModules:

```
Layouts   →  where the LEDs are in space (a Grid, a sphere, a strip)
Layers    →  a stack of images being drawn
  Layer   →  one image, built by…
    Effect     →  draws colour into the image (the fun part)
    Modifier   →  bends/masks/repeats the image
Drivers   →  send the finished image to the physical LEDs
```

An **effect** is the most creative and the easiest to write, so we start there. An effect's whole job is: **every frame, write colours into a buffer.** The buffer is just an array of bytes — 3 per LED (red, green, blue) — and the framework hands it to you already sized to the grid. You loop over the pixels and set colours. That's it.

Everything else — allocating that buffer, figuring out where each pixel is, pushing the finished frame to real LEDs, stopping cleanly when the user turns your effect off — is done *for* you by the core.

## The mental model: core orchestrates, you fill in the blanks

This is the most important section. Get this and everything else is detail.

Your module is a class with a handful of **hook functions**. You override the ones you need and leave the rest alone. For an effect there are only three that matter, and a simple effect uses just two of them:

| Hook | When the core calls it | What you put here |
|---|---|---|
| `onBuildControls()` | At startup, and whenever the control set changes | Declare your sliders/toggles (e.g. a `speed` control) |
| `onBuildState()` | At boot, and whenever the grid size / config changes | *(only if you need memory)* build a scratch buffer sized to the grid |
| `loop()` | **Every render tick** | Draw your colours. This is the heart of an effect. |
| `teardown()` | When your module is removed or **switched off** | *(only if you allocated memory)* free what `onBuildState()` built |

**The core calls these — you only fill them in.** It decides when to build, when to run, and when to release. A single traffic-cop method — `applyState()` — visits every module in the tree and either **builds it** (calls `onBuildState()`) when it's enabled or **releases it** (calls `teardown()`) when it's disabled, then runs `loop()` on the enabled ones each render tick.

> **A word on speed.** `loop()` runs on the **render tick** — as fast as the hardware can go: a few hundred frames per second on an ESP32, thousands on a desktop. This is the **hot path**, the one place where speed and memory really matter. The rule of thumb: keep `loop()` to the pixel maths, and put anything expensive-but-occasional — allocating a buffer, opening a file, parsing a config — in `onBuildState()`, which runs only when something changes. A lean `loop()` flies on the smallest chip.

Two consequences that make your life easy:

- **Your code assumes it's always live.** When the user switches your effect off, the core stops calling `loop()` and calls `teardown()` for you — so your `loop()` runs only when it should, and just draws. The "am I on?" question is the core's job, so it stays out of your code.
- **Switching a parent off switches the children off too.** Turn off the whole Layer and every effect inside it releases automatically — the core cascades it down the tree for you.

> **Why it's built this way.** Keeping the "am I on?" decision in *one* core place makes each module short and honest: it does its job and trusts the core to call it at the right time. That's the difference between a codebase you can learn in an afternoon and one that grows a thousand special cases.

## Your first effect: a moving rainbow

Here is a complete, real effect — a diagonal rainbow that scrolls over time. Read it once, then we'll walk through it.

```cpp
#pragma once

#include "light/effects/Effect.h"   // one include: the base + draw / palette / maths helpers

namespace mm {

/// Palette-cycling diagonal rainbow.
class RainbowEffect : public EffectBase {
public:
    // A control the user can drag in the UI. Plain member variable.
    uint8_t speed = 20;   // in BPM — one hue cycle every 3 seconds

    // Declare the control so it shows up in the UI. Runs at startup.
    void onBuildControls() override {
        controls_.addUint8("speed", speed, 1, 255);   // name, variable, min, max
    }

    // Called every render tick. Draw the picture for "now".
    void loop() override {
        Buffer&   buf  = layer()->buffer();   // the pixel buffer, already sized for you
        Coord3D   dims = {width(), height(), depth()};   // read the grid size every frame
        uint8_t   w    = width(), h = height();

        // beat8(speed, elapsed()) is a 0..255 value that rotates once per beat —
        // the friendly wrapper over the time maths. elapsed() is milliseconds since
        // the show started, so animation runs at the same speed on every device.
        uint8_t phase = beat8(speed, elapsed());

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // Hue = position (x + y) + time (phase) → a diagonal that scrolls.
                uint8_t hue = (x + y) * 256 / (w + h) + phase;
                RGB     c   = colorFromPalette(*Palettes::active(), hue);
                draw::pixel(buf, dims, {x, y, 0}, c);   // set one pixel by coordinate
            }
        }
    }
};

} // namespace mm
```

That's the *entire* effect — just `onBuildControls()` and `loop()`. A simple effect that reads the shared buffer needs only those two hooks. Let's unpack the important bits.

### One include gets you everything

`#include "light/effects/Effect.h"` is the umbrella header for effects: it pulls in the base class, the render-context accessors (`buffer()`, `width()`, …), and the common helpers (`draw::*`, the palette system, the `math8` animation helpers, `RGB`). Include that one file and start writing. The other kinds have the same convention — `light/layouts/Layout.h`, `light/modifiers/Modifier.h`, `light/drivers/Driver.h`. If you reach for a helper it doesn't bundle (audio input, say), add that one extra `#include`; nothing forces the set.

### Reading the grid every frame

Notice we call `width()` and `height()` **inside** `loop()`, and loop exactly `0..w` and `0..h`. **Read the real size every frame** — the same effect runs on an 8×8 grid, a 300-pixel strip, or a 64×64×64 cube, and the user can resize the grid live. Iterate exactly the dimensions the framework reports, and your effect "just works" everywhere.

> **Rule of thumb:** read `width()`/`height()`/`depth()` each frame and iterate exactly those bounds. The buffer is sized to exactly `width × height × depth × channels`, so staying within the reported dimensions keeps every write in range.

### Animating off *time*, not frames

We build the moving `phase` from `elapsed()` (milliseconds since the show started), so motion tracks real time. A desktop renders thousands of frames per second and an ESP32 a few hundred; driving `phase` from `elapsed()` keeps the speed identical on both. (Driving it from a frame counter instead would tie the speed to the frame rate — fast on the desktop, slow on the chip.) `beat8(speed, elapsed())` is the ready-made helper: it returns a 0–255 value that rotates once per beat and handles the time maths and overflow internally, so you write one line. There's a small family — `beatsin8` for a value that eases in and out like a wave, `sin8`/`cos8` for integer trig — all in `core/math8.h`.

### Writing pixels by coordinate

`draw::pixel(buf, dims, {x, y, z}, colour)` sets one pixel at a coordinate. It works out the byte offset into the buffer for you and safely ignores a coordinate that's off the grid — so you think in `(x, y, z)`, not array indices. There are matching helpers for common jobs: `draw::fill` (whole buffer one colour), `draw::line`, `draw::fade` (dim the buffer for trails), `draw::blur`. Reach for these before hand-rolling buffer arithmetic; they're in `light/draw.h`.

### Controls are just member variables

`speed` is an ordinary `uint8_t`. In `onBuildControls()` you tell the UI about it with one line, and from then on the framework keeps `speed` in sync with the on-screen slider. You read it in `loop()` like any variable. There are helpers for every common control:

```cpp
controls_.addUint8("brightness", brightness, 0, 255);   // a 0–255 slider
controls_.addBool("mirror", mirror);                    // a checkbox
controls_.addSelect("mode", mode, kModeOptions, kModeCount);   // a dropdown
```

### Registering it so the UI can add it

One line in `src/main.cpp` makes your effect appear in the "add effect" list:

```cpp
mm::ModuleFactory::registerType<mm::RainbowEffect>(
    "RainbowEffect", "light/effects.md#rainbow");
```

The second argument is the doc anchor for the in-UI help link — point it at your effect's entry in the effects catalog page.

### Comments: `///` for docs, `//` for notes

Two comment styles, two jobs:

- **`///` — documentation.** A `///` comment above the class, and above each control and method, feeds the auto-generated technical page (via Doxygen/moxygen) *and* shows up on IDE hover. Put the "what this effect is" one-liner and each control's meaning here, so a reader gets your docs without opening the source.
- **`//` — an implementation note.** A plain `//` comment explains a tricky line *to whoever is reading the source*. It stays in the code and is invisible to the generated docs — perfect for "why this maths" notes that would clutter the public page.

```cpp
/// Palette-cycling diagonal rainbow.       ← shows on the docs page + on hover
class RainbowEffect : public EffectBase {
public:
    uint8_t speed = 20;   /// One hue cycle per `speed` beats.   ← documents the control

    void loop() override {
        // 64-bit widen so the phase can't overflow.   ← a note for the code reader only
        …
    }
};
```

Rule of thumb: if a future *user* of your module should read it, use `///`; if only a future *editor* of the code needs it, use `//`.

## When you need memory: `onBuildState()` and `teardown()`

The rainbow only wrote into the buffer the framework already gave it. Some effects need their **own** scratch memory — for example a fire effect keeps a "heat" value per pixel between frames, or a game-of-life keeps the cell grid.

This is where two more hooks come in, and where the core-orchestrates model pays off. You allocate in `onBuildState()` and free in `teardown()` — and you **do not** worry about *when* those run. The core calls `onBuildState()` when the grid is (re)sized and `teardown()` when your effect is switched off or removed. You just make the two match.

```cpp
class SparkleEffect : public EffectBase {
public:
    void onBuildState() override {
        // Called at boot and whenever the grid size changes.
        // Size our scratch buffer to the current grid, reallocating only if the count changed.
        nrOfLightsType n = nrOfLights();
        if (n != heatCount_) {
            release();                                       // free the old one first
            heat_ = static_cast<uint8_t*>(platform::alloc(n));   // one byte of "heat" per light
            heatCount_ = heat_ ? n : 0;
        }
        setDynamicBytes(heatCount_);   // tell the UI how much heap we use (for the card readout)
    }

    void teardown() override {
        release();                     // give the memory back
        setDynamicBytes(0);
    }

    ~SparkleEffect() override { release(); }   // and free on destruction, just in case

    void loop() override {
        if (!heat_) return;            // buffer not there (e.g. the grid is 0×0, or memory was tight) — skip
        // …use heat_[] to render sparkles…
    }

private:
    void release() { platform::free(heat_); heat_ = nullptr; heatCount_ = 0; }

    uint8_t*       heat_ = nullptr;
    nrOfLightsType heatCount_ = 0;
};
```

(`platform::alloc` returns a raw `void*`, so the one `static_cast<uint8_t*>` names the type you want — the same line every memory-holding effect writes. The planned scratch-buffer helper below removes even this: `heat_.resize(n)`, no cast.)

Read the hooks together and the pattern is clear:

- `onBuildState()` — **acquire**: build your memory for the current grid. The core calls this only while you're enabled, so it just allocates.
- `teardown()` — **release**: give it all back. The core calls this the moment you're switched off, so a disabled effect holds *zero* memory.
- destructor — a final safety net for a bare delete.

> **The golden rule:** whatever you allocate in `onBuildState()`, free in `teardown()`. Match the two halves and the memory balances. The core guarantees `teardown()` runs on disable (and cascades it to the whole subtree if a *parent* is disabled), so implementing those two halves is the whole job — the core handles the release timing.

The one line that reads like housekeeping — `if (!heat_) return;` at the top of `loop()` — checks that the buffer exists before using it. It covers the rare cases where it might be absent: a 0×0 grid, or an allocation that ran short on a low-memory board. It asks *"is my buffer here?"* — the core already answers *"is my effect enabled?"* by choosing when to call `loop()`, so those two concerns stay separate. It's cheap insurance so a degenerate grid renders safely.

`platform::alloc` / `platform::free` are the portable allocator — they work the same on a desktop, an ESP32, or a Raspberry Pi. Use them (not raw `new`) so your effect compiles and runs on every target.

> **Heads-up — this boilerplate is on its way out.** The allocate / free / destructor / `setDynamicBytes` / null-guard pattern repeats across many memory-holding effects, so a small core helper — a scratch buffer that sizes itself, frees itself, reports its own memory, and needs no cast — is planned. With it, the whole example above collapses to *declare a buffer and use it*: `ScratchBuffer<uint8_t> heat_;` plus `heat_.resize(nrOfLights())` in `onBuildState()`, and nothing else — no `release()`, no `teardown()`, no destructor, no `setDynamicBytes`, no `static_cast`. For now, the explicit version above is the idiom.

## Test your module

Every module ships with a **unit test** — a short file that pins what it does, so a later change that breaks it fails the build instead of the show. It's a house rule (`ctest` must pass before work is done), and it's less work than it sounds: for an effect, a test builds a small grid, runs a frame, and checks the buffer got painted. The real `RainbowEffect` test is about ten lines of substance:

```cpp
TEST_CASE("RainbowEffect writes non-zero RGB data to buffer") {
    // …wire a 4×4 Layer with the effect (a few lines of setup)…
    layer.applyState();
    layer.loop();                       // render one frame
    bool lit = /* any byte in the buffer non-zero */;
    CHECK(lit);                         // the rainbow painted something
}
```

That's the whole shape: **set up a small grid, run `loop()`, assert the output is what you expect.** Good things to pin for a new effect: it paints *something* on a normal grid, it survives a 1×1 and 0×0 grid without crashing (the robustness rule), and — if it holds memory — its `dynamicBytes` drops to zero when it's disabled. When you find a bug, the fix isn't done until a test reproduces it: that's how a crash becomes a test that stops it ever coming back.

Tests live in `test/unit/light/unit_<YourEffect>.cpp` and run with `ctest`. The full strategy — the unit vs. scenario tiers, how to pick one, the live-device tier — is a topic of its own: see [docs/testing.md](../testing.md). For your first effect, one small "it paints and it doesn't crash" test is plenty.

## A tour of the other module kinds

Effects are the on-ramp. The same "fill in the hooks, let the core orchestrate" model applies to every module — only the *hook you fill in* changes.

### Layouts — where the pixels are

A **layout** answers one question: *for light number N, where is it in 3D space?* You override `forEachCoord`, which walks every light and reports its `(x, y, z)`. A grid is the classic example:

```cpp
class GridLayout : public LayoutBase {
public:
    lengthType width  = 16;
    lengthType height = 16;

    void onBuildControls() override {
        controls_.addInt16("width",  width,  1, 512);
        controls_.addInt16("height", height, 1, 512);
    }

    nrOfLightsType lightCount() const override { return width * height; }

    void forEachCoord(CoordCallback cb, void* ctx) const override {
        nrOfLightsType idx = 0;
        for (lengthType y = 0; y < height; y++)
            for (lengthType x = 0; x < width; x++)
                cb(ctx, idx++, x, y, 0);   // this light's index and its position
    }
};
```

The framework uses your coordinates to build the mapping that turns an effect's clean grid drawing into the physical LED order — so an effect works in clean grid coordinates while the framework maps them to the real strip order, zig-zag and all. Write a new layout (a circle, a sphere, a spiral staircase) and every existing effect immediately works on it.

### Modifiers — bend the picture

A **modifier** sits between the effect and the LEDs and reshapes space: mirror it, mask a region, tile it, rotate it. Instead of drawing colours, a modifier transforms *coordinates*. Its main hook, `modifyLogical`, takes a light's position and folds it into a new one (or rejects it). Several modifiers can stack, and the framework collapses the whole chain into a single fast lookup so the per-frame cost stays tiny.

Modifiers are the most abstract of the four, so treat them as a step up from effects — but the shape is the same class with the same lifecycle: declare controls, implement your transform hook, let the core run it.

### Drivers — talk to real hardware (advanced)

A **driver** takes the finished image and pushes it out to physical LEDs — over a wire (WS2812 on a GPIO pin) or over the network (Art-Net, DDP). Drivers are the **advanced topic**: they touch real hardware, timing, and DMA, and they hold a scarce resource (a GPIO peripheral or a socket). That's exactly where the lifecycle model matters most:

- `onBuildState()` **acquires** the peripheral / opens the socket for the current pin config.
- `teardown()` **releases** it — so when you switch a driver off, its GPIO is genuinely freed and another driver can use that pin. No reboot.

You get all of that "release the pin on disable" behaviour by implementing the same two hooks you already know from the memory example. The core's traffic cop does the rest. Writing a driver means learning the specific hardware peripheral (the RMT unit, the Parlio bus, a UDP socket), but the *module shape* is identical to your rainbow — which is the whole point.

## A suggested classroom path

1. **Copy the rainbow.** Change the maths in `loop()` — make it pulse, or swap the palette lookup for a fixed colour. Rebuild and add it in the UI. See it move.
2. **Add a control.** Give it a `brightness` slider (`addUint8`) and multiply your colours by it. Watch the UI wire itself up.
3. **Add memory.** Write an effect that keeps a per-pixel value between frames (a fading trail, a bouncing dot). Now you need `onBuildState()` + `teardown()` — practice matching them.
4. **Make it robust.** Resize the grid live to 1×1, then 0×0. Your effect must not crash. (Reading `width()`/`height()` each frame is what saves you.)
5. **Write a test.** Add `test/unit/light/unit_MyEffect.cpp` with one case: build a small grid, run a frame, `CHECK` the buffer got painted (and doesn't crash on 0×0). Run `ctest`. That's the habit — a module and its test travel together.
6. **Write a layout.** Arrange the pixels in a circle instead of a grid, and watch your existing effects render on the new shape.
7. **(Advanced) Read a driver.** Open `RmtLedDriver` and find its `onBuildState()` (acquire) and `teardown()` (release). You'll recognise the exact pattern from step 3 — just holding a GPIO instead of a heap buffer.

## What to read next

- **The effects catalog:** [docs/moonmodules/light/effects.md](../moonmodules/light/effects.md) — every shipped effect, with screenshots and controls. The best source of copy-and-tweak starting points.
- **The architecture doc:** [docs/architecture.md](../architecture.md) — the render pipeline (Layouts → Layers → Effects/Modifiers → Drivers) and the hot-path rules (why we avoid heap and floats inside `loop()`).
- **Coding standards:** [docs/coding-standards.md](../coding-standards.md) — the house style (header-only light modules, `constexpr`, naming) so your module reads like the rest.
- **The real modules:** the smallest ones make the best teachers — `RainbowEffect` (a clean loop), `GameOfLifeEffect` (the memory lifecycle), `GridLayout` (`forEachCoord`).

The recurring lesson across all of them: **keep your module about what it does.** Declare your controls, draw or transform in the hook, allocate-in-`onBuildState`/free-in-`teardown` if you hold memory — and let the core decide when any of it runs. That discipline is what keeps a large, multi-platform light engine understandable one small module at a time.
