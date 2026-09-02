# Plan — Fix UI resync collapse by PERSISTING view-only state, not by reconciling live DOM

## Context

The UI's full-state resync (fired by enable/disable, add/delete/move a module, or a real schema change)
rebuilds `#main` via `renderCards()` (`main.innerHTML = ""` + rebuild), which destroys transient UI state and
causes the recurring symptoms: enable/disable reverts, the controls `<details>` expander collapses, sliders
feel laggy, the tab greyness double-flips.

A first attempt reconciled the live DOM with a keyed diff (React-style). It was too fragile — it produced a
duplicate-tabs bug — and it tried to preserve state (focus/caret, mid-drag) that isn't cleanly preservable
anyway. **The correct, simpler split (per the product owner):**
- **Backend is the source of truth** for control values, module structure, and any *module control* (the type
  picker's value is a module control — it lives in the backend). A rebuild reads these fresh and correct.
- **localStorage holds the small view-only state the backend knows nothing about:** the selected tab (already
  persisted as `LS_TABS`) and **whether a module's "controls" expander is open/closed** (not persisted today).

So the fix is: **persist the expander open/closed state to localStorage (like the selected tab already is),
keep the simple `renderCards()` rebuild, and let the rebuild RESTORE that state from localStorage.** No live-DOM
diffing. Simpler, less code, and more robust — it survives a page reload too, not just a resync.

## Design

1. **New `LS_EXPANDED` localStorage key** — a JSON object/set of module names whose controls `<details>` is
   open. Loaded at startup next to `selectedTabs` (~line 61), same `lsRead` helper.
2. **The `<details class="card-controls-collapse">` (createCard, ~793):**
   - On build, set `d.open = expandedSet.has(mod.name)` so a rebuild restores the last open/closed state.
   - On `toggle`, write `mod.name` into/out of `LS_EXPANDED` (`localStorage.setItem`), mirroring exactly how a
     tab click writes `LS_TABS` (~624).
3. **Leave `renderCards()` as the full rebuild.** It already reconstructs the selected tab from `LS_TABS`; it
   now also reconstructs the expander from `LS_EXPANDED`. Everything else (values, structure, picker value)
   comes from the backend `state`, which is correct after any resync. No reconcile machinery.
4. **Tab greyness (the product owner's earlier request), the simple way:** a disabled child's tab gets a
   `tab--disabled` class, derived purely from `child.enabled` at tab build time (renderChildTabs) and in the
   patch-path twin (`updateTabDot`). Also toggled INSTANTLY in `setEnabledUi` on the on/off click (beside the
   card's `card--disabled`), so the tab greys immediately instead of a beat later on the server round-trip.
   Idempotent. Add the `.tab--disabled` CSS (greyed title, works whether or not the tab is the active one).

5. **Backend enabled-resync (discovered during implementation, shipped as part of the fix).** The
   enable/disable revert had a deeper cause than the client rebuild: `enabled` rides only the FULL websocket
   state, never the per-second value patch, yet the enabled branch of `Scheduler::setControl` never requested
   a full resync (unlike add/delete), so the client's cached state kept the old value and reverted the toggle
   about a second later. Fix: a new `MoonModule::notifySchemaChanged()` (fires the existing schema-changed
   hook that `HttpServerModule` wires to `requestFullResync`), called from the enabled branch. Reuses the
   established hook, no new mechanism. This is the C++ side; the persist/greyness work is the client side.

## Non-goals
- No keyed-DOM reconcile. The rebuild stays; correctness comes from backend plus two localStorage keys.
- No new persisted state beyond the expander. The picker value is a backend control; focus/caret and
  mid-drag are inherently transient and out of scope (a resync mid-type or mid-drag is rare and acceptable).
- Structural changes (add/delete/move) still full-rebuild, which loses an in-progress edit or mid-drag on
  those infrequent events. Accepted for now.

## Code grounding (all `src/ui/app.js` + `src/ui/style.css`)
- `LS_EXPANDED` const + load (~53–61), an `expandedSet` in memory.
- The `<details>` build + toggle handler (~793) — set `.open` from the set, write the set on toggle.
- `renderChildTabs` (~605) + `updateTabDot` (~594) — add the `tab--disabled` class from `child.enabled`.
- `style.css` — `.tab--disabled` rule (greyed, `:not` guard NOT needed — grey even when active, keep the
  active underline).

## Verification (PO eyes on desktop `build/macos/projectMM`, http://localhost:8080)
- Open a module's controls `<details>`, then enable/disable ANOTHER module (forces a full resync): the
  expander STAYS open. Reload the page: it's still open (bonus from persistence).
- Disable a module: stays disabled, no revert; its tab greys once.
- Slider drag: no snap-back (a value change no longer forces a full frame — verified this session; only a
  structural change does, and those are infrequent + don't touch an unrelated open expander now).
- Add/delete/reorder a module: tree updates; unrelated expanders elsewhere stay as the localStorage set says.
- Tabs render correctly (exactly the server's children — NO duplicates; the rebuild is authoritative).
- `ctest` green (no C++ change); JS host suite green.
