# Plan — LightPresets: a reusable named-preset library (Drivers submodule)

## Context

Today each driver owns its light preset inline: the curated built-ins (RGB/GRB/BGR/RGBW/GRBW) plus a per-driver Custom channel-role wiring (`customRoles_[kMaxChannels]` on DriverBase). A Custom wiring built on one driver can't be reused on another — you rebuild it by hand. The product owner wants **named, reusable presets** (including Custom wirings) defined once and referenced by many drivers.

The product owner connected this to palettes: Palettes today are N predefined gradients referenced by index; **custom palettes are coming**, and they're the same shape — a library of named definitions (curated + user-added), edited in one place, referenced by many consumers via a select. Light presets are that pattern too. So this builds the **first instance** of a "named-definition library" module; custom palettes will follow the same pattern later (possibly a shared base — out of scope here, *Concrete first*).

**Decisions made (product owner):**
1. **A LightPresets *module*** (not just a shared registry) — it has its own editable card. Justified because custom palettes will also want an editable home, so a library module isn't over-built for one feature.
2. **Placement: a Drivers submodule** via a new `'preset'` child role — presets live with the drivers that use them.
3. **All presets are library entries** — the curated built-ins (RGB/GRB/…) are *seeded as read-only default entries*; users add custom ones alongside. A driver picks any preset by reference. One editing home; no built-in/inline-Custom split.

## Verified current state (file:line)

- **Drivers** (`Drivers.h:74`): `acceptsChildRoles() == "driver"`. A preset submodule needs a new `'preset'` role → `"driver preset"` (space-separated, the existing format).
- **DriverBase** (`DriverBase.h`): owns `lightPreset_` (uint8 index into curated `kLightPresetOptions`), `customChannels_`, `customRoles_[kMaxChannels]` (the fixed bind-target array), `whiteMode_`, `localBrightness_`. `defineCorrectionControls()` renders the preset select + Custom editor. `seedRoles()` fills `roles_` (the ScratchBuffer render array) from preset/Custom, then `correction_.rebuild()` derives offsets. `rebuildCorrection(global)` is the cold-path entry.
- **`Correction` / `fillRolesFromPreset`** (`Correction.h`): a preset name → a `ChannelRole[]` layout. `rebuild(brightness, roles, n)` derives the hot-path offsets. This is already the "a preset is a channel-role layout" model — the module just makes the layouts **named, shared, and user-editable**.
- **`Select` stores a uint8 INDEX** (`Control.h:115`), not a name. A driver referencing a preset by raw index **breaks on reorder/delete** of the library. → the reference must be stable (see Design/References).
- **DevicesModule** (`DevicesModule.h`): the precedent for a persisted dynamic list — `addList` + `ListSource` + a recursive-JSON `restoreList`. A variable set of preset definitions persists the same way.
- **Palettes** (`Palette.h:213`): the shared named-lookup precedent (curated `kBuiltins`, `active()`/`setActive`). The pattern this generalizes.
- **customRoles_ bind-stability lesson** (just fixed): controls bind `&var`, so preset-entry role storage must be stable-address (fixed array), NOT a reallocating ScratchBuffer.

## Design

### The module — `LightPresetsModule` (Drivers submodule, role `'preset'`)

Owns an ordered set of **preset definitions**, each: a **name** + a **channel-role layout** (`ChannelRole roles[channelsPerLight]`). Seeded on first boot with the curated built-ins (RGB/GRB/BGR/RGBW/GRBW) as **read-only** entries (not deletable/renamable); users add custom entries alongside. The Custom per-channel editor (today inline on each driver) **moves here** — you define a named preset once, reference it everywhere.

- **Storage**: the preset set is a persisted List (DevicesModule pattern) — each entry `{name, roles[]}`. Role storage per entry is a **fixed array** (bind-stability lesson), bounded by `kMaxChannels`. Built-in entries flagged read-only.
- **UI**: its own card — a list of presets; selecting one reveals its channel-count + per-channel role pickers (the editor that was inline on drivers). Add/delete/rename custom entries; built-ins are read-only rows.
- **Lifetime**: a boot-wired singleton under Drivers (exactly one), so drivers can resolve it via an `ActiveInstance<LightPresetsModule>`-style seat (the election primitive already in core), the same way HueDriver reaches DevicesModule::active().

### References — a driver points at a preset (stable across reorder/rename)

A driver's `lightPreset` select lists **the library's preset names**. But a `Select` stores an index, and indices shift on reorder/delete. Two-part fix:
- The driver stores a **stable preset id** (a small monotonic id assigned per entry, persisted with the entry), not the list position. The select's *display* is the name; the *stored value* resolves to the id. (Same problem WLED solves for presets — id, not slot.)
- On resolve, the driver looks up the preset by id in the library; **missing id → fall back to a safe built-in (RGB)** and flag a status warning (robustness: a deleted preset never crashes a driver, it degrades).
- Hot path unchanged: the driver still ends up with a `Correction` whose offsets are derived once (cold path) from the resolved preset's roles. Resolution happens in `rebuildCorrection` (cold path); `apply()` is untouched.

### What moves off DriverBase

- The inline Custom editor (`customChannels_`, `customRoles_[]`, the ch* pickers, `ensureCustomRoles`) **moves into the module** (that's where a preset is now defined). DriverBase keeps: `localBrightness_`, `whiteMode_`, and the **preset reference** (the stable id + the select). `seedRoles()` becomes "ask the library for my preset's roles by id."
- `defineCorrectionControls()` shrinks: localBrightness, whiteMode, and a `preset` select (names from the library) — no inline channel editor.

### Migration

- Existing per-driver Custom wirings: on upgrade, a driver with an inline Custom wiring seeds a corresponding named entry in the library (or falls back to its curated preset). Since this is pre-1.0 and the feature shipped days ago, a clean reset is acceptable if simpler — product owner's call in the plan review.

## The editable-list is a CORE PRIMITIVE, not a preset-only editor

The preset library needs an **editable** list (add / delete / rename / reorder rows, each row's fields inline-editable). Today's `ListSource` is DISPLAY-ONLY (DevicesModule pattern: `listRowCount`/`writeListRow`/`restoreList`, no mutation hooks). Rather than build a bespoke preset editor, the product owner's steer ("this is generic functionality we'll reuse") makes this a **core primitive**: an editable-list / CRUD-grid that presets use now and **custom palettes reuse later** (and any future "library of named things"). This clears the higher core-change bar precisely because it's the recognizable, reusable primitive many modules will lean on (an editable data-grid — the UITableView-editing / react-table / QAbstractItemModel-with-edit shape), not a one-off. Build the primitive FIRST, with presets as its first consumer proving it.

**Industry-standard justification:** modern tools (VS Code snippets, DAW/Lightroom presets, WLED presets, QLC+ fixtures) all use an editable list where rows add/delete/rename/reorder and edit in place — not a "pick-from-dropdown-then-edit-fields-below" dialog. Editable rows is the user-friendly, recognizable pattern.

## Increments

0. **EditableList core primitive** — extend the list mechanism from display-only into editable: row **add / delete / rename / reorder** + **per-row editable fields**. Server side: an `EditableListSource` (or extend `ListSource`) with the mutation hooks + the API endpoints (reuse the existing `/api/...` module-list ops shape where possible). Client side (`app.js`): render editable rows with the affordances. First consumer is presets, but the primitive is domain-neutral and lives in core. A small unit/scenario pins the CRUD contract. Held to the core bar: it must read as the textbook editable-list, reusable by the next consumer (custom palettes) with no change.
1. **LightPresetsModule on the primitive** — the module, the `'preset'` child role on Drivers, presets stored as EditableList rows (each: name + channel-role layout), built-ins seeded read-only, the per-row role editor (channel-count + ch* pickers) using the primitive's per-row fields. No driver wiring yet.
2. **Driver references a preset by id** — DriverBase's `lightPreset` becomes a stable-id reference into the library; `seedRoles()` resolves via the library singleton; missing-id → RGB fallback + warning. Remove the inline Custom editor from DriverBase.
3. **Polish** — reorder/delete robustness (drivers degrade, don't crash), rename, the read-only built-in guard, docs.

## Files

- **New (Inc 0, core primitive):** the editable-list surface — extend `src/core/Control.h` `ListSource` (or a new `EditableListSource`) with mutation hooks; the API ops in `src/core/HttpServerModule.cpp`; the editable-row rendering in `src/ui/app.js`; a CRUD-contract test. Domain-neutral, in core.
- **New (Inc 1):** `src/light/drivers/LightPresetsModule.h` (+ `.cpp` if heavy) + `test/unit/light/unit_LightPresets*.cpp`. Register in `main.cpp` (boot-wired under Drivers) + `ModuleFactory`.
- **Modify:** `src/light/drivers/Drivers.h` (`acceptsChildRoles` → `"driver preset"`), `src/light/drivers/DriverBase.h` (preset-reference id replaces inline Custom editor; `seedRoles` resolves via library), `src/light/drivers/Correction.h` (unchanged core; `fillRolesFromPreset` may move/extend into the module).
- **Docs:** `docs/moonmodules/light/drivers.md` (+ a LightPresets section/page) + the editable-list primitive documented where the control system is (coding-standards / core services), `check_specs.py` control-name validation.

## Verification

- Build clean (0-warn); ctest + scenarios green. A driver referencing a preset produces the same `Correction` output as today's inline preset (byte-identical for the curated ones).
- **Robustness scenarios** (the strongpoint): delete a preset a driver references → driver falls back to RGB, no crash; reorder presets → references still resolve (id, not index); two drivers reference one preset → both update when it's edited.
- Hot path unchanged (resolution is cold-path; `apply()` untouched) — KPI flat.

## Hot-path guarantee (NON-NEGOTIABLE — the product owner's hard constraint)

**A preset must apply at full hot-path speed, at least for RGBW.** This is guaranteed BY CONSTRUCTION and the design must not break it:

- The render hot path is `Correction::apply()` (per light, per frame). It reads ONLY the driver's own derived offset cache (`offRed/offGreen/offBlue/offWhite`) + `briLut`. For RGBW that's exactly: 3 LUT reads, 1 `min()` for white, 4 indexed stores — no branch on preset type, no name lookup, no library access, no indirection. Identical cost whether the preset is a curated built-in, an inline custom, or a library reference.
- The preset library is touched **only on the cold path** (`rebuildCorrection`, on a config change): resolve the driver's preset-id → library entry → `roles[]` → derive the offset cache ONCE. Per-frame work is unchanged.
- **Each driver owns its own `Correction` value** (`DriverBase::correction_`) — the derived cache is a local member, cache-hot in `apply()`, NOT a pointer into the shared library. Two drivers referencing one preset each hold their own derived cache; neither reads the library at render time.

**Invariant the implementation MUST hold:** the library reference resolves to a driver-local `Correction` at cold-path rebuild time; `apply()` and everything it calls stays byte-for-byte the current per-light transform. Verification: a driver referencing a library preset must produce byte-identical output AND identical KPI tick time to today's inline preset (RGBW pinned explicitly). If resolving a preset ever appears in the tick path, the design is wrong.

## Scope guard

- **Do NOT** rework palettes in this change (Concrete-first — prove the pattern on presets; custom palettes + a shared base are a later, separate effort, backlogged).
- **Do NOT** reference presets by raw list index (breaks on reorder — the whole point of a stable id).
- Built-ins are seeded read-only, not special-cased in the reference path — a driver resolves a built-in the same way it resolves a custom preset (uniform, per the "all presets are library entries" decision).
- Keep `apply()` and the hot path exactly as-is; this is a cold-path/definition-ownership refactor, not a render-path change.

## Open question for plan review

- **Migration of existing inline Custom wirings**: seed-into-library vs clean-reset (pre-1.0, feature is days old). Leaning clean-reset for simplicity unless the product owner has bench wirings to preserve.
