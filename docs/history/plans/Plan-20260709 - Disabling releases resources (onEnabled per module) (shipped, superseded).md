# Plan — Disabling a module releases its resources (onEnabled per module)

> **Status: shipped (cc2e108), then superseded** by [Plan — Unify lifecycle: applyState() is the sole enabled-gate](Plan-20260709%20-%20Unify%20lifecycle%20-%20onBuildState%20is%20the%20sole%20enabled-gate%20(shipped).md) (3e37987). The per-module `onEnabled → setup/teardown` routing below *did* land and was hardware-verified (it fixed the P4 ParlioLed-dark-at-boot bug), but the ~24 scattered `enabled()` guards it required were then lifted into one core primitive: `MoonModule::applyState()` routes an effectively-enabled node to `onBuildState()` (build) and a disabled one to `teardown()` (release). This plan is the first half of that two-phase arc — kept as the design record of what shipped first and why it evolved.

The "hardware half" of release-on-disable (the "disabling a module should release its resources" backlog item — shipped, so this plan is now its record). The "display half" shipped with the pin map (a disabled module's pins already drop from the map, `PinsModule.h` `collect()` gates on `!respectsEnabled() || enabled()`). This makes that display *truthful*: a disabled module actually gives its peripheral/socket back, so the freed GPIO is really reusable — the prerequisite for pins increment #5 (live reassignment). PO-directed.

## Context

Today `setEnabled(false)` only makes the Scheduler skip a module's loop callbacks; the module still **holds** its acquired hardware (AudioService's I²S channel + codec, an LED driver's RMT/Parlio/LCD peripheral + DMA buffer, a driver's UDP socket, IR's RMT-RX channel). So "disabled" means "stops acting" but not "frees" — fine for a quick mute, wrong if disable should let another module claim the pins/peripheral. This wires the existing `onEnabled(bool)` hook so disable releases and enable re-acquires.

## Key facts (Explore, file:line) — this is smaller than the backlog implies

- **The hook is `onEnabled(bool newEnabled)`** ([MoonModule.h:128](../../../src/core/MoonModule.h#L128)), NOT `onEnabledChanged()` (the backlog + a PinsModule comment name it wrong — fix those). Virtual, no-op default, fires only on a real transition (`setEnabled` early-outs if unchanged, [MoonModule.h:220](../../../src/core/MoonModule.h#L220)). **Zero implementers today** — greenfield.
- **Almost all the release/acquire logic already exists.** Every resource-holding module *except IrService* already has a working `teardown()` that fully releases AND an idempotent `setup()`/`reinit()` acquire path (also reached via the live-reconfig `onBuildState()` sweep). So per module the work is *wiring* `onEnabled(false)→teardown-body`, `onEnabled(true)→setup-body`, not writing new logic.
- **`onBuildState()` is the idempotent re-acquire precedent** ([MoonModule.h:193]): the drivers' and AudioService's `onBuildState()` call the same `reinit()` as `setup()`, already exercised live on every control change. Re-enable can reuse it.

### The modules + their existing paths
- **AudioService** ([AudioService.h](../../../src/core/AudioService.h)) — `setup()` = `reinit(); syncReinit()` (+ `active_` mic-election); `teardown()` = `deinit()` (audioMicDeinit + audioCodecDeinit) + socket close (+ `active_` vacate). `onEnabled` must preserve the `active_` bookkeeping exactly as setup/teardown do.
- **RmtLedDriver** — `teardown()` = `deinitAll()` (RMT channels) + `freeSymbols()` + `DriverBase::teardown()`; `setup()` = `parseConfig(); reinit()`.
- **ParallelLedDriver** (CRTP base of Parlio + LCD) — `teardown()`→`deinit()`→`busDeinit()` (`parlioWs2812Deinit` / `lcdWs2812Deinit`); `setup()` = `parseConfig(); reinit()`.
- **NetworkSendDriver** — `teardown()` = `socket_.close()`; `setup()` = `socket_.open()`.
- **NetworkReceiveEffect** — `teardown()` = close artnet/e131/ddp sockets; `setup()` = open+bind each.
- **IrService** ([IrService.h](../../../src/core/IrService.h)) — **the exception.** No `setup()`/`teardown()`, no channel member; the RMT-RX channel is a `static` behind `platform_esp32_ir.cpp` keyed by pin, freed only on a pin change. Needs a **new `platform::irStop()` seam** to release from the module.

## Design

### 1. The base pattern (PO-confirmed: direct call, but REUSE the same body onBuildState/setup use — no duplication)
For each resource-holding module, add:
```cpp
void onEnabled(bool on) override { if (on) <acquire>; else <release>; }
```
where `<acquire>`/`<release>` are the **exact same bodies** the module's `setup()`/`onBuildState()` (acquire) and `teardown()` (release) already run — **not a copy**. For the drivers and AudioService that acquire body is already the shared `reinit()` (`onBuildState()` calls it, `setup()` calls it), and the release body is `deinit()`/`teardown()`; so `onEnabled` just calls those existing functions — zero new logic, zero duplication. Where a module's `setup()`/`teardown()` inline the acquire/release rather than calling a shared helper, factor it into a private `acquire()`/`release()` that `setup()`, `teardown()`, `onBuildState()`, and `onEnabled()` all call, so the enable/disable path and the boot/reconfig path can never diverge. **Re-acquire is a direct call to that shared body, NOT a route through the whole-tree `Scheduler::buildState()` sweep** (PO decision: each module owns its own enable/disable; no coupling to the tree sweep — but it runs the identical `reinit()` code the sweep would, so behaviour matches).

### 2. Per module
- **AudioService**: `onEnabled(true)` → `reinit(); syncReinit()` + re-elect (`if (active_==nullptr) active_=this`); `onEnabled(false)` → `deinit()` + socket close + vacate (`if (active_==this) active_=nullptr`). This is literally the setup/teardown bodies — factor into `acquire()`/`release()` and have all three call them.
- **RmtLedDriver / ParallelLedDriver**: `onEnabled(true)` → `parseConfig(); reinit()`; `onEnabled(false)` → the `teardown()` body (`deinitAll()+freeSymbols()` / `deinit()`). The `reinit()` already deinits-then-rebuilds, so it's re-callable.
- **NetworkSendDriver / NetworkReceiveEffect**: `onEnabled(true)` → open (+bind); `onEnabled(false)` → close.
- **IrService**: add `platform::irStop()` (frees the static RMT-RX channel; a no-op if none) — desktop stub no-op. Then `onEnabled(false)` → `irStop()`; `onEnabled(true)` → nothing (the channel re-acquires lazily on the next `irRead` in `loop()`, which already only runs when enabled).

### 3. Fix the stale names
Rename `onEnabledChanged` → `onEnabled` in the backlog item and the PinsModule comment ([PinsModule.h] the "DISPLAY half / hardware half" note) so the docs match the code.

### 4. Contract decision (per module, state it in each override)
Disable frees the *peripheral* always (the point of the item). Whether it also frees the *large DMA buffer* is the RAM-vs-instant-re-enable tradeoff the backlog flags: the LED drivers' `teardown()` already frees the buffer (`freeSymbols()` / `dmaBuf_=nullptr`), so disable → buffer freed → re-enable rebuilds it. Keep that (frees RAM; re-enable cost is one `reinit`, already the live-reconfig cost). No per-module divergence unless a module shows a reason.

## Files
- **Edit:** `src/core/AudioService.h`, `src/light/drivers/RmtLedDriver.h`, `src/light/drivers/ParallelLedDriver.h`, `src/light/drivers/NetworkSendDriver.h`, `src/light/effects/NetworkReceiveEffect.h`, `src/core/IrService.h` (+ `onEnabled` overrides, factor acquire/release where setup/teardown duplicate). `src/platform/platform.h` + `src/platform/esp32/platform_esp32_ir.cpp` + desktop stub (`irStop()`). `docs/backlog/backlog-core.md` + `src/core/PinsModule.h` comment (name fix). `docs/moonmodules/core/services.md` / light docs if a module's disable behavior is user-visible.
- **Tests:** `test/unit/` per module — enable→disable→enable round-trips the resource without leaking or crashing (the platform layer's test seams already let a host test observe init/deinit calls; AudioService/driver unit tests exist as the model). A scenario: add a driver, disable it (its pins free in the PinsModule map — ties the two halves together), re-enable, still renders.

## Verification
1. Build clean; ctest + scenarios; boundary (the `irStop` seam stays in `src/platform/`); ESP32 build; KPI.
2. Live on hardware: disable AudioService → the mic I²S releases and its pins free in the pin map (truthful now, not just displayed); re-enable → mic works again, no reboot. Disable an LED driver → its RMT/Parlio peripheral + pins free; another driver can claim those GPIOs (no conflict flag); re-enable → renders. Disable IR → the RMT-RX channel frees. Confirm no crash on rapid enable/disable toggling (robustness-to-any-input).

## Scope guard
Wire the EXISTING teardown/acquire bodies into `onEnabled`; do NOT rewrite release logic (it's already there and tested via teardown). The one genuinely new code is `platform::irStop()`. Do NOT build the live-reassignment broker (#5) — this only makes the freed pin *real*; the broker that swaps two drivers' pins is the next increment, unblocked by this. Keep each `onEnabled` a thin call to the module's own acquire/release. Mark `(shipped)` when it lands.
