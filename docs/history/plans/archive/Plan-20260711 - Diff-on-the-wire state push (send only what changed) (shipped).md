# Plan — Diff-on-the-wire state push (send only what changed)

## Context

LEDs stutter at ~1 Hz, only with a browser (WS client) connected. Traced to `HttpServerModule::tick1s()` → `buildStateJson()` serialising the **whole module tree every second on the render thread** (tick1s runs inline in the Scheduler loop). Two fixes already landed and helped but did not eliminate it:
1. **Options hoist** (`optionSets`/`optionsRef`): the repeated 14-role option array now emitted once per list, not per channel per row. 44 KB → 34.6 KB. Stutter "better but not gone."
2. **Resumable chunked send** (`JsonSink::detach` + drain on tick20ms): moved the *send* off the render tick. Helped the write, but the **serialize** (not the write) is the spike.

**Measured residual:** state is 34.6 KB; between two consecutive pushes only **~67 leaf fields change** (mostly telemetry — `tickTimeUs` ×15, FreeRTOS task `stack`/`prio`/`state` ×~30, `ageSec` ×5, a few live `value`s). So **>99% of the 34.6 KB is identical second-to-second** and re-serialised for nothing. Preset detail alone is ~10.5 KB of near-static channel descriptors.

**The industry-standard answer (PO):** stop sending snapshots; send **diffs**. Full state once on connect, then a small patch of only-what-changed. The recognised pattern: **JSON Patch (RFC 6902)** over the WebSocket (the same model React/Redux, Firestore, git-on-the-wire use — full model, delta transport). ~34.6 KB/s → ~1–2 KB/s, and — critically — the **serialize** shrinks to O(changes), which is what actually removes the render-thread spike.

## The decisive design constraint (why value-compare, not per-write flags or JSON snapshot-compare)

Three ways to compute the diff:
- **JSON snapshot-compare**: rebuild the full 34.6 KB JSON each second, diff against the last-sent string, send the delta. **Still serialises the full tree every second** — the serialize IS the stutter, so this does NOT fix it. Rejected.
- **Per-write dirty flags**: each changed leaf marks itself dirty on write. Problem: values change via TWO paths — `setControl` (user/API, easy to hook) AND the device mutating its own storage (telemetry `tickTimeUs`, status strings, driver state — most of the churn). Path 2 bypasses `setControl`, so per-write flags would need every device-side write instrumented. Fragile.
- **Per-leaf value-compare, serialize-only-changed** (CHOSEN): store a `lastSentValue` per control; at push time walk the tree (only **92 controls** — cheap int/string compares, microseconds) and **serialise only the controls whose value differs** from lastSent, then update lastSent. The expensive JSON *serialisation* runs for ~5–67 changed leaves, NOT 92. This catches BOTH change paths uniformly (it reads the live value however it changed — no per-write instrumentation) AND avoids the full-tree serialize (the actual spike). The cheap O(tree) *compare* is not the cost; the O(tree) *serialize* was — and that's now O(changes). This is the leaf-level refinement of "snapshot-compare" that the naive JSON-string version got wrong: compare VALUES (cheap), serialise only diffs (the win).

The repo already has the UI scaffolding this needs: a no-rebuild per-control patch path (`updateModuleControls`, the `list.dataset.sig` skip) that already updates a single control's DOM in place. The device side is new (the value-hash cache + `buildStatePatch`) but self-contained in HttpServerModule — no change to the control/module classes for detection, since the compare reads live values through the existing `writeControlValue`. (`MoonModule::markDirty()` stays what it is — a persistence-subtree flag, unrelated to this WS patch.)

## Design

### Cache scope: global, full-resync on any connect (simplest; upgradeable)

One **global** last-sent value cache (not per-client). When ANY client connects, resync EVERYONE with a full state and reset the cache; steady state sends a shared patch to all clients. No per-client "synced" bookkeeping at all — connects are rare on a LED controller, so an occasional full push when a 2nd browser opens is fine. This is *Concrete first, abstract later*: a clean upgrade path to a per-client synced-flag (Option 1) exists if multi-browser efficiency ever matters, but it isn't built now.

### Wire protocol: full-state-on-connect, then patches

- **On WS connect**: send the current **full** `/api/state` once (the UI's initial render needs the whole tree) and reset the global cache so the next patch is a clean baseline for all clients.
- **Each tick1s** (with synced clients): build a **patch** — a JSON array of changed leaves — and send it. Frame type distinguishes it (`{"patch":[...]}` vs the initial `{"modules":[...]}`), or a WS sub-protocol tag.
- **Patch entry**: `{"path":"<module>/<control>","value":<v>}` — a slim custom shape addressed by the UI's existing (module-name, control-name) keying, OR strict RFC 6902 (`{"op":"replace","path":"/...","value":...}`). Decide in review: RFC 6902 is the recognised spec (interop, libraries) but its JSON-Pointer paths are verbose for our tree; a slim `{mid,key,value}` matches how `updateModuleControls` already looks controls up. Leaning slim-custom keyed the same way the UI already patches, with a one-line comment citing RFC 6902 as the prior art the shape follows.

### Cache storage: hash per control, not the value string

The last-sent cache stores a **hash** (FNV-1a `uint32`) of each control's serialized value, keyed by a hash of its `<module>/<control>` path — NOT the full value string. So the cache is a fixed **8 bytes per control** (path-hash → value-hash) regardless of value size (a 1.3 KB list summary caches as 4 bytes). At push time: serialize the control's value to a small scratch `JsonSink`, hash it, compare to the stored value-hash; on mismatch emit `{path, value}` and update the hash. ~92 entries → <1 KB cache, flat and uniform. Hash-collision risk (two different values, same 32-bit hash → a missed update) is negligible for 92 controls and self-heals on the next real change; a 64-bit hash is the trivial upgrade if ever a concern. This is the standard "cache a cheap digest, not the payload" move (ETag/rsync-style).

### Device: per-leaf value-compare (hash)

- **NO per-write dirty flags** (rejected above — they miss device-side writes). Change is detected by value-compare: `buildStatePatch` walks the 92 controls, serialises each control's VALUE to a small scratch sink, hashes it, and compares to the global cache. This reads the live value however it changed (setControl OR a device write), so telemetry, status, and driver updates are all caught without instrumenting any write site.
- **`buildStatePatch(sink)`**: for each control, serialise value → hash → compare to cache; on mismatch emit `{path,value}` and update the cache hash. The cheap O(tree) part is the value-serialise+hash (~3.7 KB of values total, microseconds); the win is that the 30.8 KB of METADATA (options, list detail, field descriptors) is never touched — it rides only the initial full state.
- **Telemetry** (`tickTimeUs`, task list, `ageSec`) changes every tick by nature — its value-hash differs each second, so it rides every patch (~3 KB). Acceptable (it genuinely changes). Follow-up (NOT this change): move telemetry to a slower cadence / opt-in stream so an idle device sends a near-empty patch. Measure first; throttle only if ~3 KB/s still hitches.
- **Structural changes** (add/delete/move module, list row add/delete/move) can't be a value patch — they reshape the tree. On a structural change, **re-sync** the affected client(s): send a fresh full state (or a structural patch). Reuse the existing `afterListMutation`/module-mutation hooks to trigger a full re-sync. Simple and rare.

### UI: apply patches instead of replacing state

- On the initial full-state frame: render as today (`renderCards`).
- On a patch frame: for each entry, update the one control's value via the **existing** `updateModuleControls` per-control patch logic (it already updates a single control's DOM without a rebuild, suppressing the field being edited). No full re-render.
- The `list.dataset.sig` / open-row / scroll-preservation machinery stays — a list value patch updates just that control.
- Keep a client-side full-state model so a structural re-sync replaces cleanly.

### What this subsumes / makes moot

- The **options hoist** stays (correct + cheap), but the near-static preset detail now simply **never appears in a patch** after the initial sync — so the 10.5 KB is a one-time cost, not per-second. **Detail-on-demand and the compact-roles reshape become unnecessary** (the plan explicitly drops them — don't build them).
- The **resumable chunked send** stays for the ONE big frame that remains: the initial full-state on connect (and a structural re-sync). Patches are tiny and send inline.

## Files

- **`src/core/HttpServerModule.cpp/.h`**: the global value-hash cache + `buildStatePatch` (walk controls, value-hash-compare, emit `{path,value}` for mismatches). No changes to `MoonModule`/`ControlList` for change-detection — the compare reads live values through the existing `writeControlValue`.
- **`src/core/HttpServerModule.cpp/.h`**: per-client "synced" flag; `tick1s` sends full state to un-synced clients, a patch to synced ones; structural hooks trigger re-sync; `buildStatePatch`.
- **`src/ui/app.js`**: WS `onmessage` distinguishes full-state vs patch; patch path reuses `updateModuleControls` per-control update; keep a full-state model for re-sync.
- **Tests**: `buildStatePatch` emits only controls whose value changed since the last build (change one control → it appears once; build again with nothing changed → empty patch); a structural change forces a full re-sync; the UI patch-apply updates one control without rebuilding (JS test if mockable). Pin that an unchanged tree produces an EMPTY patch (the core guarantee), and a single value change produces a one-entry patch.
- **Docs**: `HttpServerModule` WS protocol — document the connect-full-then-patch contract + the patch shape (cite RFC 6902 as prior art). `architecture.md § Parallelism`/state-sync note.

## Verification

1. Build clean; ctest + scenarios + JS green.
2. **Measure on the classic Olimex (192.168.1.208), browser open**: a steady-state patch (nobody touching controls) is ~1–3 KB (telemetry only), not 34.6 KB. The **serialize cost per tick1s drops to O(changes)** — the render-tick 1 Hz spike is gone in the KPI timing. **This is the pass/fail: LEDs no longer stutter with the UI open.**
3. Editing a control still updates live (its leaf patches through); adding/deleting a module or preset row re-syncs the UI correctly (full state resent); the collapsed/expanded list state + scroll survive a patch.
4. Initial page load still renders the full tree (the one-time full state on connect, chunk-drained).

## Scope guard

Diff-on-the-wire via per-leaf value-hash compare (NO per-write dirty flags) + connect-full-then-patch. Do NOT keep a snapshot-compare (it leaves the serialize spike). Do NOT build detail-on-demand or compact-roles (subsumed — near-static detail leaves the per-second path for free). Telemetry throttling is a NAMED follow-up, only if ~3 KB/s telemetry patch still hitches after this lands. Structural changes re-sync via a full state (rare) rather than inventing structural patches. Keep the options hoist and the resumable sender (for the initial/re-sync full frame).
