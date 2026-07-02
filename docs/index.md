# projectMM

High-performance LED &amp; DMX lighting control for ESP32 and beyond.

[:material-flash: Flash an ESP32 from your browser](/install/){ .md-button .md-button--primary }
&nbsp;
[:material-github: GitHub](https://github.com/MoonModules/projectMM){ .md-button }

!!! tip "New here?"
    The [Getting started](gettingstarted.md) guide walks you from a blank ESP32 to your first running light show, step by step — no build tools required.

## What it is

projectMM drives large LED installations and DMX fixtures. You build a light show by stacking simple blocks — a **layout** (how the LEDs are arranged), one or more **effects** (what they animate), **modifiers** (mirror, rotate, mask…), and a **driver** (how the pixels reach the hardware). Every setting takes effect live; there is no reboot to apply a change.

It runs on ESP32 (the primary target), and also on Teensy, macOS, Windows, Linux, and Raspberry Pi.

## Find your way

<div class="grid cards" markdown>

-   :material-download: **Get running**

    Flash a board from your browser and light your first pixels.

    [Getting started](gettingstarted.md) · [Web installer](/install/)

-   :material-palette: **Build a show**

    Browse the effects, layouts, modifiers, and drivers you compose into a show.

    [Effects](moonmodules/light/effects/effects.md) · [Layouts](moonmodules/light/layouts/layouts.md) · [Modifiers](moonmodules/light/modifiers/modifiers.md)

-   :material-check-decagram: **See what's verified**

    Every behaviour is pinned by a test. When a bug is fixed, a test proves it.

    [Unit tests](tests/unit-tests.md) · [Scenario tests](tests/scenario-tests.md)

-   :material-code-braces: **Go deeper**

    System design, the module model, and the per-module reference.

    [Architecture](architecture.md) · [Core modules](moonmodules/core/MoonModule.md) · [Light pipeline](moonmodules/light/EffectBase.md)

</div>

The web installer works in Chrome &amp; Edge (Web Serial) — no download required.
