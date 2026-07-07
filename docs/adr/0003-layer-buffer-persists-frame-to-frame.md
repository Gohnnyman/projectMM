# 3. The layer buffer persists frame-to-frame

Status: Accepted

## Context

An early design cleared the render buffer before every effect frame, on the reasoning "the buffer is the effect's to fill, every time." This silently broke every persistence effect: a scroll reads the prior column and shifts it (reading a wiped buffer, only the fresh pixel survived); a trail calls `fadeToBlackBy` to decay the previous frame (fading zeroes never forms a trail); Game-of-Life reads its prior cell state (gone). The symptom that surfaced it: FreqMatrix lit only one row, and ~13 effects' `fade` controls did nothing.

## Decision

The render buffer is **not** cleared each frame. `Layer::loop()` leaves the previous frame's pixels in place, zeroed once on allocation/resize, then persistent, matching the FastLED / WLED / MoonLight convention (their `leds[]` / segment / VirtualLayer buffers all persist). Each effect owns its background inside its own `loop()`: a full-grid effect overwrites every pixel, a trail calls `fadeToBlackBy`, a sparse effect that wants a clean frame calls `draw::fill` itself. There is no per-effect "persist" flag (a flag would be bespoke). `fadeToBlackBy` is a Layer operation collected once per frame: effects register an amount, the Layer keeps the MIN across them and applies one pass at the next frame's start.

## Consequences

- Persistence effects (scroll, trail, Game-of-Life) work; multiple effects on one layer deliberately interact through the shared persistent buffer.
- N fading effects cost one buffer pass, not N, and never fade each other's fresh pixels.
- A state-advancing effect (Game-of-Life) separates when the simulation steps (gated on `bpm`) from when it paints (frame rate); its state lives in the persistent buffer. `unit_Layer_persistence` and `unit_GameOfLifeEffect` pin it.
