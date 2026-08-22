# Plan — List-row detail on demand (the real fix for the 1 Hz stutter)

## Context — and a corrected diagnosis

LEDs stutter ~1 Hz, only while a browser (WS client) is connected. A first fix moved the WS state *send* off the render tick (chunked resumable drain + `JsonSink::detach`, see the sibling plan). **It did not help — slightly worse.** Measurement on the live classic-Olimex (192.168.1.208) shows why:

- Full `/api/state` = **43.7 KB**. The `presets` control alone = **21 KB** (50%). Each of the 13 preset rows serialises a `detail` object whose per-channel `select` **repeats the full 14-string role-option array**; the 32-channel BeTopper row is 5.2 KB by itself.
- `buildStateJson()` serialises all 43.7 KB (incl. the 21 KB of preset detail) **every second on the render thread** (`tick1s` runs inline in the Scheduler loop). **The SERIALIZE is the spike, not the socket write.** The send-side plan addressed the wrong half and added `detach`/alloc work, so it netted slightly worse.

Dropping list detail from the push cuts state to **22.2 KB** (half). The remaining 22 KB is the rest of the tree.

**Chosen fix (PO): don't serialise list-row detail in the periodic push — the UI fetches a row's detail on demand when the user expands it.** Row detail (the channel-role editors) is only *needed* for an expanded row; sending all 13 rows' editors every second is pure waste. This is the product owner's long-standing "don't send what nobody's watching" instinct, now genuinely applicable (detail *is* per-row and mostly unwatched, unlike the whole-tree case where it wasn't).

## Interaction with the send-side change

The send-side change (chunked drain, `detach`) is **complementary, not redundant, but re-evaluate after measuring**: once detail is dropped the push is ~22 KB (still serialised each second, but the *send* of 22 KB still benefits from being off the render tick). Plan: land this detail-on-demand fix, reflash the Olimex, **measure**. If the stutter is gone, keep the send-side change (correct + cheap). If 22 KB serialised-per-second still stutters, the follow-up is skip-when-unchanged (cache last state, rebuild only on a tree-change signal) — a separate change, not built pre-emptively.

## Verified current state (file:line)

- **`writeControlValue` List branch** (`src/core/Control.cpp:187-202`): emits `,"detail":[ <writeListRowDetail for every row> ]`. This is the 21 KB. Called from the state push (`writeControls` → `writeControlValue`) AND any other `/api/state` consumer.
- **`writeListRowDetail`** (`Control.h:171`, overridden in `LightPresetsModule.h`): for a preset, writes `{fields:[{name,type,value,options...}]}` — one `select` per channel, each with the full `kChannelRoleOptions`. This is the richness that explodes for a 32-channel fixture.
- **`/api/list/<module>/<control>[/<id>]`** (`HttpServerModule.cpp:280` POST, `:328` PATCH, `:337` DELETE): no **GET** yet. This is where the on-demand detail endpoint slots in.
- **UI list render** (`app.js` `buildListEntries` / `fillEditableListDetail`): builds a row's detail panel from `ctrl.detail[i].fields`. Today that data arrives inline in `/api/state`; it must instead be fetched on expand.
- **The `list.dataset.sig` dirty-check** already skips a DOM rebuild when rows+detail are unchanged — but the *device* still serialises + sends detail regardless; that's what this fixes at the source.

## Design

### Device: stop emitting list detail in the bulk push; serve it per-row on demand

1. **`Control.cpp` `writeControlValue` List branch**: emit only the `value` (summary rows) + `editable` flag. **Drop the `detail[]` array.** The summary row (`writeListRow`: id, name, channelCount, locked) is all the collapsed list needs — ~1 KB for 13 presets vs. 21 KB.
2. **New `GET /api/list/<module>/<control>/<id>`** (`HttpServerModule.cpp`, beside the POST/PATCH/DELETE handlers): resolve the list + row by id (reuse `resolveEditableList`), write that ONE row's detail via `writeListRowDetail` into a small JSON response. One row's detail (even a 32-ch fixture, 5 KB) served only when asked, off the 1 Hz path.
   - This also means the on-demand fetch is a **cold, occasional** request, not a per-second cost — no render-tick impact.

### UI: fetch a row's detail when it expands

3. **`app.js`**: when a row's detail panel is opened (the expand toggle), if its detail isn't loaded yet, `GET /api/list/<module>/<control>/<id>`, then render the editors from the response. Cache it on the row element so re-expanding doesn't refetch (invalidate the cache when a field edit or the row's summary changes — the summary carries `channels`, so a channel-count change re-fetches).
   - Collapsed rows never fetch detail → the common case is zero detail traffic.
   - The existing `fillEditableListDetail` renders from a `fields[]` object; feed it the fetched object unchanged.

### What stays

- Non-editable lists (Devices discovery) already have small detail; dropping it from the bulk push and fetching on expand is the same mechanism, uniformly. (Verify the Devices card still shows its detail on expand via the new GET.)
- The send-side resumable drain + `detach` stay (pending the post-measure re-eval above).

## Files

- **`src/core/Control.cpp`**: List branch of `writeControlValue` — drop `detail[]`.
- **`src/core/HttpServerModule.cpp` / `.h`**: add the `GET /api/list/.../<id>` handler (route in the GET dispatch near `:280`), reuse `resolveEditableList` + `writeListRowDetail`.
- **`src/ui/app.js`**: fetch-on-expand for a row's detail + per-row cache + invalidation on summary/field change.
- **Tests**:
  - `unit_Control_list.cpp` (or the HttpServer apply test): `writeControlValue` for a List no longer emits `detail` (payload shrinks; the summary rows remain).
  - A handler test for `GET /api/list/.../<id>` returning one row's detail (and 404 for a bad id).
  - JS: a small test that expanding a row triggers the fetch and renders fields (if the JS harness can mock fetch; else manual + the C++ endpoint test).
- **Docs**: `HttpServerModule` REST list — document the new GET verb alongside POST/PATCH/DELETE. `coding-standards`/architecture note on "list detail is fetched on demand, not pushed" if it reads as a general rule.

## Verification

1. `cmake --build build` clean; `ctest` + scenarios; JS tests green.
2. `/api/state` size drops ~50% (measure on-device: ~43.7 KB → ~22 KB). `presets` control ~1 KB (summary only).
3. **On the classic Olimex (192.168.1.208), browser open**: the 1 Hz stutter is GONE (the 21 KB per-second serialize is eliminated). Expanding a preset row fetches + shows its channel editors; collapsing + re-expanding uses the cache; editing a channel still works (PATCH unchanged) and reflects live.
4. If stutter persists at 22 KB/s serialize → escalate to skip-when-unchanged (separate change).
5. KPI: render tick no longer shows the 1 Hz spike with a client connected.

## Scope guard

Move list-row detail from the periodic bulk push to an on-demand GET; drop it from `writeControlValue`. Do NOT also hoist shared options or build skip-when-unchanged in this change (both are separate levers; measure after this one). Keep the send-side resumable drain unless the measure shows it's now pointless. The summary rows (id/name/channels/locked) stay in the push — the collapsed list must still render without a fetch.
