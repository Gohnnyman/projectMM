# Plan — PinsModule (ownership map, read-only)

Pins top-down [increment #1](../../backlog/pins-analysis-top-down.md#8-increments-each-its-own-plan-when-picked): the phase-1, read-only GPIO ownership map. PO-approved this session ("Approving pins top down, implement the first step of it").

## Context

Today nothing shows which GPIO each module claims. A driver's `pins="18,19"`, a mic's `sckPin/wsPin/sdPin`, an Ethernet PHY's `ethMdcGpio` — each lives in its owning module's controls, invisibly. There is **no cross-tree pin enumerator and no conflict check** (Explore confirmed: `ControlType::Pin` appears only in `addPin` + serialization; the per-driver `pins` CSV parse in [PinList.h](../../../src/light/drivers/PinList.h) is scoped to one driver). The GPIO-46 loopback corruption — an output role driven onto a strap — is exactly the class of bug a visible pin map catches.

The [top-down study](../../backlog/pins-analysis-top-down.md) designs two axes: **ownership** (who claims each GPIO, for what role — read-only, phase 1) and **live state** (what each pin is doing now — later, testing-focused). This plan is *only* the phase-1 ownership map. Per the study's own scope guard: "resist making it an allocation subsystem (the controls are already the registry)."

## Scope cut (the leanest first increment)

§8 defines increment #1 as: "enumerate `Pin` controls → GPIO-keyed `ListSource` map with **owner + name-derived role**." That is the whole of this plan. Two things §3 lists as columns are **deliberately deferred**, each for a concrete reason:

- **Capability flags** (valid / output-capable / RTC / I2C — the ✅💡⏰🔌 column). These need a new `platform::gpio*` capability seam (`GPIO_IS_VALID_GPIO` etc. behind the platform boundary — none exists today). Adding a platform seam is real core work; folding it in would make increment #1 stop being the "small, safe first" the study promises. It's a clean fast-follow *within* phase 1 once the module exists and reads live. **Deferred to increment #1b.**
- **Reserved/strap + claimed-twice flags** (§4/§5). The claimed-twice flag is the conflict authority (explicitly phase 2/3 — "don't wire the conflict authority into phase 1"). Strap flags depend on `gpio-usage.md` data not yet in a consumable form. **Deferred.**

What ships: **`{gpio, owningModule, controlName → role}`**, keyed by physical GPIO, read live off the tree, refreshed on `loop1s()`. Nothing written, nothing enforced.

## Verified seams (Explore, file:line)

- **Pin value:** `*static_cast<int8_t*>(desc.ptr)`; `-1` = unused (skip). Type check `desc.type == ControlType::Pin`. `ControlDescriptor` at [Control.h:184](../../../src/core/Control.h#L184) (`ptr`, `name`, `type`, `min`, `max`, `hidden`, `readonly`). Iterate a module's controls via `MoonModule::controls()` → `ControlList::count()` / `operator[]` ([Control.h:385-386](../../../src/core/Control.h#L385)).
- **Text-pin CSV:** name-convention only — Text controls named `"pins"` hold `"18,19,20"` ([RmtLedDriver.h:109](../../../src/light/drivers/RmtLedDriver.h#L109), [ParallelLedDriver.h:88](../../../src/light/drivers/ParallelLedDriver.h#L88)). No type-level flag. Parse with `parsePinList` in [PinList.h](../../../src/light/drivers/PinList.h) (`uint16_t out[]`, dedup, returns error literal or nullptr).
- **Tree walk:** no generic helper. Recurse `Scheduler::instance()->module(i)` for roots ([Scheduler.h:73-74](../../../src/core/Scheduler.h#L73), `moduleCount()`/`module(i)`) + `child(i)`/`childCount()` ([MoonModule.h:390-391](../../../src/core/MoonModule.h#L390)). Pattern: `printModuleMetrics` at [main.cpp:241](../../../src/main.cpp#L241).
- **List shape:** copy [TasksModule](../../../src/core/TasksModule.h)'s nested `ListSource` member. `ListSource` at [Control.h:160](../../../src/core/Control.h#L160) (`listRowCount` / `writeListRow` / `writeListRowDetail` default = summary / `restoreList` default = no-op — a read-only source overrides only the first two). Bind with `controls_.addList("pins", pins_)`.
- **Owner column:** `MoonModule::name()` ([MoonModule.h:201](../../../src/core/MoonModule.h#L201)) for display; `typeName()` ([MoonModule.h:216](../../../src/core/MoonModule.h#L216)) to disambiguate same-type instances.
- **Wiring:** three-line create/`markWiredByCode()`/`systemModule->addChild()` after [main.cpp:295](../../../src/main.cpp#L295); include near :102; `registerType<PinsModule>("PinsModule", "core/system.md#pins")` near :220. Base `Generic` role → not user-addable; `markWiredByCode()` exempts from persistence trim.
- **GPIO ceiling:** `MM_MAX_GPIO` ([Control.h:13](../../../src/core/Control.h#L13), build-injected `CONFIG_SOC_GPIO_PIN_COUNT-1`, fallback 63). Used only to size the per-GPIO aggregation buffer — not for capability (that's 1b).

**Caveat (Explore):** a Pin control can be `hidden` (e.g. `loopbackTxPin` when test mode is off) and `onBuildControls` re-runs. Read live on each serialize; **include hidden pins** (a hidden-but-set pin still claims the GPIO — that's precisely what a map must show). Never cache.

## Design

### `src/core/PinsModule.h` (new, header-only)

Same shape and rationale as [TasksModule.h](../../../src/core/TasksModule.h) — a small read-only System diagnostic, header-only (the header-only-core exception the sibling modules already carry). `///` doc: what it shows (GPIO → owner · role), the read-live-off-the-tree design (no state), the fixed-System-module wiring, **Prior art:** MoonLight ModuleIO's per-pin report (the reusable field set — owner/usage), inverted here (each module owns its pins; this only observes), plus the bespoke-reason line per *Common patterns first*. `@card` deferred until there's a screenshot.

**One list: `pins`.** A nested `PinListSource : ListSource` member (`pins_`), TasksModule's nested-member shape (the map is derived, not the module's identity — so not the DevicesModule "module *is* the source" variant).

**Enumeration (`refresh()`, called on `loop1s`).** Walk the tree once, aggregate claims by GPIO into a fixed buffer:

```
struct Claim { uint8_t gpio; const char* owner; const char* role; };   // char* point into live module/control storage — refreshed each pass
Claim claims_[kMaxClaims];  uint8_t count_ = 0;
```

- Recurse roots (`Scheduler::instance()->module(i)`) + children. At each module, scan `controls()`:
  - `type == Pin` and value `>= 0` → one claim `{value, module->name(), roleFor(control.name)}`.
  - `type == Text` and `strcmp(name,"pins")==0` → `parsePinList` the CSV; one claim per parsed GPIO, role `"LED lane N"` (index into the list).
- `kMaxClaims` a fixed cap (e.g. 64) — no allocation, hot-path-clean even though this runs off it. Overflow: stop adding, the list is a diagnostic (log once if exceeded).

**Row = a GPIO.** `writeListRow` emits `{"gpio":N,"owner":"…","role":"…"}`. If a GPIO has multiple claims (the future-conflict case), the summary shows the first and `writeListRowDetail` lists all claims on that GPIO — so a double-claim is *visible* (read-only surfacing) without phase-1 *enforcing* anything. Rows sorted by GPIO number (Device-Manager keying) via the existing [Sort.h](../../../src/core/Sort.h) if a sort helper fits, else insertion order (refine later).

**`roleFor(const char* controlName)`** — a small static name→role map, projectMM's name-derived equivalent of ModuleIO's `usage` enum (§3): `sckPin`→`BCLK`, `wsPin`→`WS`, `sdPin`→`data`, `mclkPin`→`MCLK`, `pins`→`LED lane`, `loopbackTxPin`→`loopback Tx`, `loopbackRxPin`→`loopback Rx`, `ethMdcGpio`→`MDC`, `ethMdioGpio`→`MDIO`, `sda`→`I²C SDA`, `scl`→`I²C SCL`; fallback = the control name verbatim. A flat `{suffix, role}` table + `strcmp`/`strstr`, no allocation. Kept deliberately small — it's a display convenience, not a vocabulary authority (that's phase 2's job if it ever needs one).

### `src/main.cpp`

- `#include "core/PinsModule.h"` near :102.
- `registerType<mm::PinsModule>("PinsModule", "core/system.md#pins")` near :220.
- After the I2cScan wiring (~:295): `create` → `markWiredByCode()` → `systemModule->addChild(pinsModule)`.

### `docs/moonmodules/core/system.md`

Add a `### Pins` summary card (System-modules group, beside Tasks / I2C scan): what it shows (GPIO → owner · role), read-only, always present. Add the matching `#pins` anchor the `registerType` docPath resolves to (`check_specs.py` validates this — the gate that catches a missed anchor).

## Files

- **New:** `src/core/PinsModule.h`; `test/unit/core/unit_PinsModule.cpp`.
- **Edit:** `src/main.cpp` (include + registerType + wiring); `docs/moonmodules/core/system.md` (Pins card + `#pins` anchor).
- **Maybe:** a scenario JSON asserting the pins list is present + populated after a driver with pins is added (the full-pipeline coverage the Hard Rule wants).

## Tests

- **Unit (`unit_PinsModule.cpp`, host):** build a tiny tree (a module with two `Pin` controls set to 13/14, one at `-1`; a driver-like module with `pins="18,19"`). Assert: the source lists GPIO 13/14/18/19; skips `-1`; role derives correctly (`sckPin`→BCLK, `pins`→LED lane 0/1); a GPIO claimed twice appears once in summary with both owners in detail; `listRowCount` matches distinct claimed GPIOs. Read-only: no control mutates the tree.
- **Scenario:** add a driver with `pins`, tick, assert the System/Pins list serializes with the expected GPIO rows (drift-tracked).

## Verification

1. `cmake --build build` clean (`-Wall -Wextra -Werror`); `ctest` + scenarios green; **`check_specs.py` green (the `#pins` docPath resolves)**; `check_platform_boundary.py` (PinsModule touches no platform API — pure tree read); ESP32 build.
2. Live on a board: **System → Pins** lists the LED driver's GPIOs by role; a mic module's SCK/WS/SD show; setting a pin to a used GPIO shows both owners in that GPIO's detail (visible, not enforced). No reboot — the map reflects a live pin change on the next `loop1s`.

## Scope guard

Read-only enumeration only. **NO** capability flags (needs a new `platform::gpio*` seam → increment #1b), **NO** conflict enforcement / pin-uniqueness authority (phase 2), **NO** live-state columns (Level/DriveCap/ADC — the §6 axis). Keep `roleFor` a small display map, not a vocabulary. If the module starts accreting arbitration or a central pin table, it's drifted out of phase 1 — the controls are already the registry; this only reads them. Mark this plan `(shipped)` when it lands.
