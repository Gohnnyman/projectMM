# Plan — Release 4 scope: effect breadth + the rename runway

## Context

Release 3 is being cut now. This plan captures the **Release 4** candidates — the next strategic thread after R3 — so the direction is recorded before the work starts. The product owner's steer: the items below are R4, not R3.

The backlog has one dominant strategic thread that most other items orbit: the **projectMM → MoonLight rename** ([backlog rename plan](../../backlog/rename-to-moonlight.md)). Its gate is *"the effect library must not feel thin next to the predecessor's 60+ effects."* Two in-flight plans feed that gate, and R4 is where they land. The shape of R4 is therefore **"the effects release + the rename runway"**: grow visible feature breadth while moving the single most important strategic gate (rename readiness), and leave the hardware-verification-bound driver work to its own dedicated push.

This is a roadmap/scope plan, not a single-feature `/plan`. Each item below gets its own `/plan` + commit when reached; this document is the *map* and the *why*.

## The spine — effect-breadth parity (headline)

**MoonLight migration, Stage 1 + the next effect batch.** ([Plan-20260630 - MoonLight migration (multi-stage)](Plan-20260630%20-%20MoonLight%20migration%20(multi-stage).md).)

This is the biggest lever and the explicit *"execution vehicle for the effect-breadth parity gate."* ~21 of the predecessor's 60+ effects are ported. Stage 1's prerequisites are the highest-value core work available, because every future effect leans on them:

- **Shared palette** — hard prerequisite; many effects color via `ColorFromPalette`. Generalise the pattern `PlasmaPaletteEffect` hard-codes today.
- **The shared primitive library** — FastLED-named, our own implementation, hot-path-tuned integer-only: `beatsin8`, `inoise8`, `qadd8`, `nscale8`, `random8`/`random16`, `ColorFromPalette`, and the dimension-agnostic draw set. Extends the existing `color.h` (`scale8`, `sin8`).
- **Tag/emoji legend** — settle before batch-migrating so every module is consistent from batch one.
- **Per-library doc model** — `effects_<library>.md` compact table rows (per [ADR 0015](../../adr/0015-library-is-a-tag-not-a-folder.md)); changes the `check_specs.py` contract.

Then the next migration batch on top. This is the R4 headline: it unblocks the rename *and* is pure user-visible feature growth.

## Two quick wins — scoped and ready

- **Active-instance election primitive.** ([Plan-20260710 - Active-instance election primitive](Plan-20260710%20-%20Active-instance%20election%20primitive.md).) A core `ActiveInstance<T>` that removes duplicated singleton-election bookkeeping from `AudioService` + `DevicesModule` (both had real dangling-static bugs). Textbook *Complexity-lives-in-core* subtraction; small; in flight.
- **CodeRabbit #29 boundary findings (4).** ([backlog-core § MoonLive core/platform layering](../../backlog/backlog-core.md#moonlive-coreplatform-layering--jit-sdkconfig-scoping-coderabbit-29-4-findings).) MoonLive core-includes-platform + compiled-into-`mm_core`, W^X disabled in the board default, a scenario riding timing + network. Real, already scoped; good hygiene to close before a named release.

## The RS-485 / DMX-512 opportunity (candidate, larger)

The [P4-shield RS-485/DMX hardware is now well documented](../../reference/mhc-wled-esp32-p4-shield.md) (the builder's schematics landed 2026-07-16). The **RS-485 / DMX-512 wired-output driver** + its **`platform::` UART-RS485 seam** ([backlog-light](../../backlog/backlog-light.md#rs-485-dmx-512-wired-output-future-the-physical-dmx-driver)) is demand-driven and self-contained. It is a meaty new capability — a flagship candidate if R4 wants a headline new-hardware feature alongside the effects work, but it is larger than the two quick wins and should be its own `/plan`.

## High-light-count driver work (in R4 — hardware-verified)

The streaming-ring / lane-driver work is **in R4**. It is hardware-verification-heavy — each item needs the expander wall (and the relevant board) to prove, so these land with bench sign-off, not blind:

- **Classic-ESP32 shift-register ring on raw I2S** ([backlog-light "WANTED"](../../backlog/backlog-light.md#drivers)) — the high-light-count classic driver.
- **P4 Parlio streaming ring** ([backlog-light "WANTED"](../../backlog/backlog-light.md#drivers)) — lift the P4 Parlio ceiling past ~21K to light-count-independent.
- **Shared lane-driver scaffolding** — extract when the 3rd parallel backend lands (deferred until then, but that 3rd backend is one of the two above).
- **MoonI80 prime-only ring stall backstop** ([backlog-core](../../backlog/backlog-core.md#mooni80-prime-only-ring-no-stall-backstop-sibling-path-gap)) + the whole-frame late-EOF serialization hardening — the sibling-path recovery gaps; verify on the expander wall.

## Success shape

R4 ships when: the migration Stage-1 primitives + the next effect batch have landed (moving the rename's breadth gate forward), the `ActiveInstance` primitive and the CodeRabbit #29 boundary fixes are in, the RS-485/DMX driver reaches a verified first output, and the high-light-count driver work above is bench-verified. The rename itself is a *separate* cutover (its own plan); R4 is the runway that makes the name not a downgrade, not the switch.
