# 16. One parallel LED driver with a runtime peripheral strategy, not three CRTP subclasses

Date: 2026-07-23

## Status

Accepted

Builds on [ADR-0014](0014-own-i80-dma-driver-below-esp-lcd.md) (the own-DMA MoonI80 backend), which this consolidates alongside the esp_lcd i80 and Parlio backends.

## Context

The parallel-LED output was **four classes**: a CRTP base `ParallelLedDriver<Derived>` holding all shared logic (slicing, the fused correct+transpose encode, the async double-buffer, the loopback self-test, the dead-frame guard) and three concrete CRTP subclasses, each a full registered `MoonModule`: `MoonLedDriver` (own-GDMA LCD_CAM + streaming ring + 74HCT595 expander), `MultiPinLedDriver` (esp_lcd i80 on S3/P4 LCD_CAM, I2S on classic), `ParlioLedDriver` (P4 Parlio). Because each was separately factory-registered, the UI add-module picker offered all three on *every* board, including chips that cannot run them (`lanesAvailable() == 0`).

The CRTP base existed for exactly one reason: to reach the peripheral via compile-time dispatch. Every `derived()->` call is a peripheral operation; there is no non-peripheral use of CRTP. And crucially, every such call is **per-frame or per-reinit, never per-light** — the per-light encode operates on the raw `uint8_t*` the peripheral hands back, and never calls into the peripheral. CRTP's guarantee ("no runtime indirection") therefore protected calls that don't exist on the hot path.

The product owner wanted one user-facing "Parallel LED" module with a `peripheral` dropdown that surfaces the shared controls plus the selected peripheral's unique controls, allocating only the selected backend.

## Decision

Collapse the four classes into **one registered `ParallelLedDriver`** (a plain `MoonModule`) that holds a **`LedPeripheral*` runtime strategy**, chosen by a `peripheral` Select. The three ex-subclasses become `LedPeripheral` implementations (`I80Peripheral`, `MoonI80Peripheral`, `ParlioPeripheral`), each self-registering its factory + label with a static registry, gated by its chip's `CONFIG_SOC_*` so a board links only its usable backends. The Select is board-filtered to `lanesAvailable() > 0` and uses stable string labels (not indices) so a catalog config is portable across chips. `RmtLedDriver` stays a separate module (a different shape: N independent per-pin RMT channels, not one lockstep DMA bus).

Because CRTP protected only per-frame calls, replacing it with one vtable dispatch per frame is free (one vcall against thousands of microseconds of frame work). The base's shared body did not change; only the *dispatch to the peripheral* moved from compile-time to runtime.

Two capabilities fall out of the single-object design and are included:
- **A peripheral-block claim guard**: the chip has one of each hardware block (one LCD_CAM, one Parlio, one I2S), so two live drivers on the same block corrupt each other. A driver reports its block via an RTTI-free `hwBlock()` virtual (ESP32 is `-fno-rtti`), gated on `inited_` so only a driver actually holding the bus claims it; a sibling wanting the same block idles with a clear status. Different blocks (RMT + Parlio + i80 on a P4) coexist.
- **`pinExpander` auto-clear**: a peripheral that cannot host the 74HCT595 (Parlio, classic i80) silently degrades an enabled expander back to direct mode rather than idling on an unfixable error.

Core stays domain-neutral: the backend registry and the peripheral interface live in `src/light/drivers/`; the only core touch is a string-label apply path in `Control.cpp` (a Select value may be an option label, not just an index).

The alternatives weighed and rejected: **keep CRTP + one registered wrapper** (still three code paths, still the wrong-chip picker problem, no runtime switch); **fold RmtLed in behind the same interface** (a leaky abstraction carrying single-DMA-bus ops half the implementers cannot honor — an expansion, not a reduction).

## Consequences

**Net subtraction plus a feature.** Four classes become one module + one interface + three stripped backends; one control set, one lifecycle, one registry entry, one UI card. The backends shrink (they lose the `MoonModule`/control/lifecycle scaffolding). The add-module picker offers one "Parallel LED" card on every board, and the `peripheral` dropdown shows only what the chip supports; switching it live re-surfaces that peripheral's controls and re-inits the bus with no reflash.

**One new hot-path fact, and it is free:** one virtual dispatch per frame to reach the peripheral. This is the *only* runtime indirection added, and it is per-frame, not per-light.

**The runtime backend became a swappable object, which the persistence and structural-mutation paths had to learn about** — two robustness bugs this branch also fixes and records in [lessons.md](../history/lessons.md): a control whose backing variable lives on the (swappable) backend was lost on reload unless persistence re-binds the backend first; and stopping the encode worker before a structural mutation had to reach the worker's owner (`Drivers`) regardless of which subtree was mutated. Both are core-level fixes with regression tests. The lesson embedded in the ADR: moving a compile-time type choice to a runtime object makes every path that assumed a fixed object (persistence overlay order, per-parent worker quiesce) a place to check.

The migration cost is documented, not coded (per [ADR-0013](0013-no-migration-code-robust-persistence-plus-documented-breaks.md)): a field device's persisted `MoonLedDriver`/`MultiPinLedDriver`/`ParlioLedDriver` type no longer resolves, so the module drops on boot and the user re-adds a Parallel LED driver and picks the peripheral — a `MIGRATING.md` entry covers it, and the web-installer catalog names the new type so a fresh install is correct.

The design intent and staged plan are the [consolidation plan](../history/plans/); this ADR is the decision record.
