# Plan: a file-editor control, and a filesystem change seam

## Context

Editing a MoonLive script means leaving the module: find the file in the File Manager, open the
modal editor, save, then go back and re-name the script on the card to force a recompile. The thing
a user wants is to type on the module's own card and watch the fixture change.

Two things block that, and only one is about the editor.

**The editor exists but is not reachable from a card.** `openFileEditor` ([app.js:4510](../../../src/ui/app.js))
is a working text editor over `/api/file` with a truncation guard, a binary guard and a prettify
hook. It is welded to a `<dialog>` opened from a File Manager tree row.

**Saving a file's CONTENTS notifies nothing.** `requestPrepareTree()` is reached only from a control
write (Scheduler.cpp:270), so nothing recompiles when a path stays the same and the bytes change.
The three MoonLive bindings paper over this three different ways, and two are wrong:

- `MoonLiveEffect` has no content hash. `affectsPrepare` (:57) fires only on the script NAME, so a
  content change never re-enters `prepare()`. Its comment at :63 claims a source edit recompiles;
  the code does not do it.
- `MoonLiveLayout` caches `compiledHash_` (:205), cleared only on a name change (:99, :106).
- `MoonLiveModifier` re-hashes every prepare and compares (:96). Correct, and the comparison is
  REQUIRED: the comment at :90-95 records that an unconditional `needsRebuild_` makes `prepare()`
  and the Layer's `applyState()` call each other forever, and the fixture renders nothing.

The outcome is one capability (a file editor control) plus one core rule extended to its next path.

## Design

### 1. Core: a filesystem change seam

`handleWriteFile` has a single success branch (HttpServerModule.cpp:711). On success it calls a hook
that asks the scheduler to re-derive, the same construct `Scheduler` already takes for
`loadAllHook_` and `noteDirtyHook_` (Scheduler.h:56-62):

```cpp
// A successful write to `path` changed persistent state that a module may have derived from.
// Core's existing rule (a control write re-derives the tree) extended to the file path.
using FileChangedFn = void (*)(const char* path);
```

`main.cpp` wires it to `Scheduler::requestPrepareTree()`. Every module's `prepare()` then re-runs
and each decides for itself whether anything actually changed, which is what the shared
`MoonLiveScript` hash comparison answers in 4 bytes without holding the source.

**Why the seam rather than a client-side nudge.** The alternative was to have the browser re-POST
the unchanged path to `/api/control` after saving, which re-enters the existing notification path
without any core change. Rejected on [CLAUDE.md](../../../CLAUDE.md) Principle 3: when core enforces
a rule on one path, extend core to the next path, never paste the check into the caller. The rule is
"a change to persistent state re-derives what depends on it", core already enforces it for a control
write, and a file write is the next path. A client-side nudge also only works for whoever remembers
it, so `curl`, MoonDeck and the File Manager's own modal would each leave a stale program running.

Deliberately a whole-tree request rather than a path-to-module registry: a registry needs an
association nothing else in the system keeps, and `prepare()` is already the cold path that exists
to be re-entered. If profiling ever shows the sweep is too broad, the hook has the path in hand and
can narrow later without changing its callers.

**Coalescing is already there, so nothing needs building.** `requestPrepareTree()` sets an atomic
flag that `tick()` consumes with `exchange(false)` (Scheduler.h:82, Scheduler.cpp:85), so a burst of
writes inside one tick (the File Manager's multi-file upload) already collapses to a single sweep.
It is also the call the hook must use rather than `prepareTree()`: the doc at Scheduler.h:74-77
warns that the immediate walk runs a scripted layout's JIT'd code on the CALLING task's stack, and a
file-write hook runs on the small web-server task.

### 2. Core: `ControlType::FilePath`, wire name `filepath`

A path-valued control: storage identical to `Text` (a module-owned `char[]`), but the UI renders a
picker plus an inline editor over `/api/file`.

A separate type rather than a flag on `TextArea`, because they store opposite things. TextArea's
value IS the body; FilePath's value is a ~40-byte reference, and the body cannot enter
`/api/control` at all (413 above the request buffer, HttpServerModule.cpp:181-197). Every existing
flag (`hidden`, `readonly`, `advanced`, `numberField`, `fader`, `encoder`) leaves the value's
meaning untouched, so a flag that changes what the value MEANS is not a rendering hint.

`aux` carries a `const char* const[2]` of `{directory, extension}`, the same shape `Select` already
uses for its options array (Control.h:442). No new descriptor field, so the positional-initializer
hazard noted at Control.h:295 does not apply. `writeControlMetadata` emits `dir` and `ext`, so the
UI needs no hardcoded knowledge of MoonLive.

```cpp
addFilePath(name, buf, bufSize, dirAndExt)
```

### 3. UI: one editor, two hosts

Split `openFileEditor` into a body-owning core that the modal wraps, so the modal keeps its exact
behavior and the card mounts the same code inline:

- `fmLoadInto(textarea, relPath, expectedSize)`: load, truncation guard, binary guard, prettify.
- `fmSaveFrom(textarea, relPath)`: POST the body.
- `fmMountEditor(host, relPath, {expectedSize, onSaved})`: the pane (textarea, status, Save, dirty
  dot) wired for blur-save, Ctrl/Cmd+S and the Save button.
- `fmCreateFile(dir, name)`: shared by the File Manager toolbar and the card's create button.

`openFileEditor` keeps its signature and becomes a dialog shell around `fmMountEditor`. Reuse the
`.fm-editor-*` class names so style.css:1629-1655 serves both. Lift the existing `textareaSizes` and
`ResizeObserver` height persistence (app.js:1737-1751) into `fmMountEditor`, so the `textarea` case
and the new case share one copy instead of two.

**Save on blur, on Ctrl/Cmd+S, and on the Save button**, with a dirty dot and a `beforeunload` guard.
Explicitly not per-keystroke autosave: a save writes to flash and re-derives whatever depends on the
file, so autosaving would do both on every keystroke, against a file that is half-typed and
therefore usually invalid. Blur and an explicit key are what every editor a user already knows do.

**The card row**: a native `<select>` of the directory's files (filtered by `ext`), a create button,
a delete button behind `armPressTwice` (the double-confirm the File Manager already uses at :4258),
then the editor pane.

### 4. Light domain: `MoonLiveScript`, the shared helper

New `src/light/moonlive/MoonLiveScript.h`, owning what the three bindings each keep a divergent copy
of: the name buffer, the engine, the status buffer, the compiled hash and the failed-name latch.

```cpp
/// Re-read and recompile IFF the file's content hash moved. Returns true when a NEW program
/// was installed: the signal a binding needs to ask its Layer for a mapping rebuild.
/// Idempotent, and that is load-bearing rather than an optimization: returning false on an
/// unchanged file is what stops prepare() and the Layer's applyState() calling each other
/// forever (see MoonLiveModifier's cycle).
bool sync(const SysVarTable& sysvars, MoonModule& owner);
```

`MoonLiveScriptFile.h` gains `scriptFileHash(name, uint32_t&)` (read and hash, no compile), so the
unchanged path costs one file read instead of a recompile. `compileScriptFile`'s `hashOut` already
exists (:44-46) and only the modifier passes it, so the convergence is subtractive.

**The rule is the same for all three, and it is one line: if the file changed, recompile.** That is
what `sync()` is. The three bindings look different today only because each grew its own bookkeeping
around that one rule, and the convergence deletes the bookkeeping rather than reconciling it.

What genuinely differs is not the rule but **how many places ask**. A layout is asked from
`lightCount()` and `placeLights()` as well as `prepare()` (MoonLiveLayout.h:64, :74, :83), because a
container computes its bounding box by walking its children before those children have prepared. An
effect and a modifier are asked from `prepare()` only. Same question, same answer, more callers.

And the modifier's `needsRebuild_` is not a different trigger either: it is what the modifier does
AFTER a recompile (tell the Layer to redo its mapping). That is why `sync()` returning "a new program
was installed" serves it with no special case.

| binding | today | after |
|---|---|---|
| Effect | no hash; recompiles on every prepare | `script_.sync(...)`, so an unchanged script stops re-JITting |
| Layout | lazy `compile()`, early-out, failed-name latch | `script_.sync(...)`. The `const_cast` and the three extra call sites stay in MoonLiveLayout.h, because parent-first `applyState()` ordering is a layout fact, not a MoonLive one |
| Modifier | re-hashes every prepare, compares | `needsRebuild_ \|= script_.sync(...)`, so the cycle-break is enforced by the return value rather than by a comment |

The three ~45-line `prepare()` bodies become one-liners over one shared body. The `script` control on
all three cards becomes `addFilePath` with MoonLive's own `kScriptPick[2] = {kScriptDir, ".mlv"}`, so
core knows nothing about scripts and the light domain supplies the directory. Scripts stay inside
`kScriptDir`: MoonLiveScriptFile.h:66-71 refuses a path separator so a control value cannot reach
`../.config/NetworkModule.json`.

## Files

- `src/core/HttpServerModule.{h,cpp}`: the hook, fired at the one success branch (:711)
- `src/core/Scheduler.{h,cpp}`: unchanged. `requestPrepareTree()` already does the job, coalescing
  included
- `src/main.cpp`: wire the hook to `requestPrepareTree`
- `src/core/Control.{h,cpp}`: the type and its registration points (enum, `controlTypeName`,
  `writeControlValue`, `writeControlMetadata`, `applyControlValue`, `addFilePath`)
- `src/ui/app.js`: the four extracted functions, `openFileEditor` shrunk onto them, and the FIVE
  control registration points: `createControl` (:1494), `updateModuleControls` (:3213),
  `EDITABLE_CONTROL_TYPES` (:55), `controlValuesEqual` (:3394). `controlRendersGenerically` needs no
  change, being type-agnostic. Also fix the `.ml` to `.mlv` comment at :4436.
- `src/ui/style.css`: a `.control-fileedit` wrapper and the dirty dot; reuse `.fm-editor-*`
- `src/light/moonlive/MoonLiveScript.h`: new
- `src/light/moonlive/MoonLiveScriptFile.h`: `kScriptPick`, `scriptFileHash`
- `src/light/moonlive/MoonLive{Effect,Layout,Modifier}.h`: hold the helper, shrink
- `docs/moonmodules/core/ui.md`, `docs/moonmodules/light/MoonLiveEffect.md`: the control type row
  and the save-recompile loop
- `docs/backlog/backlog-light.md`: delete the "editing a script's CONTENTS does not recompile it"
  entry, which this closes

## Verification

**Unit (C++)**, each pinning a behavior a user could state:

- "a file write asks the tree to re-derive": the hook fires on success, and NOT on a rejected write
  (bad path, 413, 507).
- "a burst of writes costs one prepare sweep": the coalescing `requestPrepareTree()` already gives,
  pinned so a later change to the hook cannot turn one sweep per tick into one per file.
- "editing a script's file recompiles it, without renaming the file": the feature's core guarantee,
  and nothing pins it today.
- "syncing an unchanged script recompiles nothing": the modifier's cycle-breaker, testable directly
  for the first time instead of only through the Layer.
- "a modifier whose script did not change does not ask the Layer to rebuild": the blank-fixture
  infinite loop, as a regression test rather than a comment.
- "a script that fails to compile is tried once, not on every sweep": the failed-name latch, and a
  DIFFERENT name is still tried (the bug MoonLiveLayout.h:129-133 documents).
- "a file-path control stores a name, never a file body": a 5 KB value into a 41-byte buffer
  truncates and stays NUL-terminated.

Not written: that `controlTypeName` returns `filepath`. Trivial, and it does not earn its place.

**Scenario**: extend `scenario_MoonLiveEffect_livescript.json`, which already live-edits a script
file, with a same-path rewrite that must recompile.

**By hand (desktop)**, in two passes, because the control is generic and only its plumbing is not.

The CONTROL, judged without reference to any consumer: the picker lists only files matching the
declared extension and nothing else; typing raises the dirty dot; blurring saves and clears it;
Cmd/Ctrl+S does the same; create makes a file and focuses it; delete needs two clicks and disarms
if the pointer leaves; a binary file is read-only; the same file open in both the modal and a card
does not clobber; a file deleted underneath the control leaves the OWNING MODULE to report its own
error, and nothing crashes.

The PLUMBING, on a MoonLive card: saving changes the fixture, and deleting the pointed-at script
leaves the module's own "script not found" status. This is the only pass that mentions MoonLive,
and it is testing the binding rather than the control.

Unchanged-behavior check on the shared code: the File Manager modal still pretty-prints a `.json`
and still refuses a binary file, exactly as before the extraction.

**Hardware (product owner, the final guardrail)**: type in the card, blur, and the fixture changes.
Confirm the save stall is a visible blink and not a watchdog reset at the 16 KB `kScriptFileMax`, on
a large grid at full frame rate. Free heap flat across twenty save-edit cycles (the exec block
reallocates per compile, so a leak shows here and nowhere else). A CRLF file and a UTF-8 BOM file
compile or report a diagnostic, and never crash.

## Steps

Three steps, each stopping at a point the product owner can review and judge. Commit timing is the
product owner's call and is not part of this plan.

1. The filesystem seam and the `MoonLiveScript` convergence. Verifiable on its own without any UI
   change: saving through the File Manager that exists today already recompiles. This is the step
   with hardware risk, because it changes when `prepare()` runs.
2. `ControlType::FilePath` and the JS editor extraction, with the File Manager unchanged. The
   extraction is verifiable by the File Manager still behaving exactly as it did before.
3. The card control replacing `addText("script", ...)` in the three bindings: the loop the product
   owner asked for, where typing runs the script.
