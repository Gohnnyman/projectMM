# Plan: ControlModule and presets

## Context

There is no way to save a device's configuration and bring it back. Every change edits the live tree,
and the only persistence is the automatic one that restores exactly what was there at reboot. A user
who finds a look they like cannot keep it, and cannot switch between looks.

MoonLight solved this inside `ModuleLightsControl`, and the mechanism is the one to copy: **a preset
is a JSON file, saving is copying a file, selecting is reading one back**. MoonLight's presets cover
only effects and modifiers. We make it generic, and put it in **core** rather than the light domain,
so a preset can carry any part of the tree.

`ControlModule` is also where external control belongs later (MIDI surfaces, IR, a hardware panel):
one place that says "put the device in this state", whatever asked for it. Presets are its first
capability, not its only one.

**Naming.** `LightPresetsModule` already exists and is a different thing: named channel-role wirings
per fixture. It keeps its name here; the collision is noted in the module comment on both sides so a
reader is not misled. If the two prove confusable in use, renaming that one to a fixture profile is a
separate, PO-called change.

## Decisions taken

- **A preset captures a SELECTABLE set of top-level subtrees**, recorded in the file. A `Layers`-only
  preset is hardware-portable; adding `Drivers` makes it a device snapshot that carries pin maps.
  The file says which, so applying one is never a surprise.
- **Named files**: `/.config/presets/<name>.json`. Delete is a file delete; a preset uploaded through
  the File Manager just appears. This is the PO's stated principle, taken literally.
- **Playlists are NOT in this branch.** The cycling hook is designed in and left unbuilt; multiple
  named playlists get their own plan, informed by real presets to cycle.

## Design

### The file

```json
{
  "captures": ["Layers", "Layouts"],
  "Layers":  { "enabled": true, "0.type": "Layer", "0.0.type": "NoiseEffect", "0.0.speed": 128 },
  "Layouts": { "enabled": true, "0.type": "GridLayout", "0.width": 128 }
}
```

Each captured subtree is **exactly the bytes `FilesystemModule` already writes** for that module
(`writeNode`, `FilesystemModule.cpp:348`): a flat map of dotted positional keys, with `<idx>.type`
per child. Reusing that format means save and restore reuse the engine that already reconciles a tree
against JSON, rather than a second serializer that could drift from it.

### What has to be added to core

`FilesystemModule` can already do both halves, but neither is reachable at runtime:

- **`saveSubtreeTo(MoonModule*, JsonSink&)`** — factor the body of `saveSubtree`
  (`FilesystemModule.cpp:319`) so it can write into a caller's sink instead of straight to
  `/.config/<TypeName>.json`. The existing method becomes a thin caller of it.
- **`applySubtree(MoonModule*, const char* json, const char* prefix)`** — a public wrapper over the
  private `applyNode` (`FilesystemModule.cpp:191`), which already creates, replaces and destroys
  children by type and tolerates unknown types. **It must also drive the lifecycle `applyNode`
  leaves undone**: `applyNode` calls only `defineControls()` on a created child, because at boot the
  Scheduler's phases 3 and 4 follow. At runtime the caller must do what `applyAddModule` does
  (`HttpServerModule.cpp:1591`): `setup()` then `applyState()`, then one `prepareTree()`.

Both go on `FilesystemModule` because that is where the format and the reconciliation live. No new
serializer, no second copy of the tree-walking rules.

### ControlModule

A top-level module, peer of Layouts/Layers/Drivers, registered in `main.cpp` alongside them. Not
under `Services`: it reaches *across* the top-level modules, so it cannot be a child of one.

Controls:

| control | what it does |
|---|---|
| `presets` | An editable `List` (`ListSource`, `Control.h:190`) — one row per file, with the captured subtrees shown per row. |
| `name` | Text: the name to save under. |
| `capture` | Which subtrees a save includes. One `addBool` per top-level module, so the set is explicit. |
| `save` | Button: write `/.config/presets/<name>.json`. |
| `status` | ReadOnly: what happened, and which preset is currently applied. |

Applying a row uses the list's existing per-row edit path (`setListRowField`), which reaches the
source with an arbitrary field name, so a row gets an "apply" affordance with no new UI primitive.
A row also carries delete and rename through the CRUD the list already provides.

**Save** flushes pending writes first (`FilesystemModule::flushPending()`, `.cpp:100`) so the file
captures the live state rather than a stale debounce, then walks the selected top-level modules and
writes one object per capture.

**Apply** reads the file, and for each key in `captures` that resolves to a live top-level module,
calls `applySubtree`. A capture naming a module this build does not have is skipped with a status
line: the same degrade-never-crash rule `applyNode` already follows for unknown child types.

### The hot path

Applying a preset rebuilds modules, and every structural mutator already quiesces the render worker
(`MoonModule::quiesceForMutation`, `MoonModule.h:510`). But mutations run inline on the render tick,
so a large restore stalls rendering for its duration. **Batch it**: mutate every captured subtree,
then one `prepareTree()` and one `requestFullResync()` at the end, rather than per subtree as the
existing add path does. `tick()` is untouched, since presets are a cold-path feature.

## Files

- `src/core/ControlModule.h` — new. The module, its controls, the preset `ListSource`.
- `src/core/FilesystemModule.h` / `.cpp` — `saveSubtreeTo` + `applySubtree`; `saveSubtree` refactored
  to call the former.
- `src/main.cpp` — register the type, create it, `scheduler.addModule` it.
- `src/ui/app.js` — the row-apply affordance, **in both render paths** (`renderCards` and
  `updateModuleControls`; a rule added to one only is invisible on a WebSocket update).
- `docs/moonmodules/core/control.md` + the catalog card.
- `test/unit/core/unit_ControlModule.cpp` — new.

## Verification

1. **Unit**: a preset round-trips (save a tree, mutate it, apply, the tree matches); a capture naming
   an absent module is skipped without throwing; a corrupt file degrades to a status rather than a
   crash; an unknown child type inside a capture is skipped and the rest still applies.
2. **Scenario**: save a preset, change effects and layout live, apply the preset, assert the pipeline
   still renders non-zero, which is the wired-pipeline gate the other scenarios use.
3. **`check_footprint --module ControlModule`**: zero static RAM when not used.
4. **`clang-hotpath`**: no new blocking call on the render path.
5. **Bench, and the gate that matters**: on a real board, save a look, change it, bring it back, and
   confirm the panels show what they showed before. **PO judgement.**
6. Hardware portability, deliberately: a `Layers`-only preset saved on one board applies on a board
   with different pins and drives its own hardware.

## Deliberately not in this plan

- **Playlists**, per the decision above. The apply path is the hook they will need.
- **Apply-on-boot.** MoonLight explicitly does not (its preset branch is guarded against firing at
  boot); WLED does. Worth deciding once presets exist and the behaviour can be felt.
- **Renaming `LightPresetsModule`.** Noted as a collision, not acted on: it is a PO call and a
  separate change.
- **External control (MIDI, hardware surfaces).** This is what `ControlModule` exists to host, but
  the first capability is presets; adding a control surface has its own plan.
