# Plan — Move the WS state push off the render tick (resumable text frame)

## Context

LEDs stutter on a ~1 s cadence, visible only while a browser has the UI open (a WS client connected). Root cause, traced in code: `HttpServerModule::tick1s()` → `pushStateToWebSockets()` → `buildStateJson()` serializes the **whole module tree** (measured **20.4 KB** on the live device with the LightPresets library) and `sendWsTextFrame()` does a **single blocking `conn.write()` of the full 20 KB per client** — all inline on the render thread (`tick1s` runs in the same Scheduler loop as `tick()`). On a backpressured client that write blocks the render loop for the frame, producing the 1 Hz stutter. No WS clients → `pushStateToWebSockets` early-returns → no stutter (matches the report).

The **preview stream** (a *larger* frame) does NOT stutter because it was already moved off the hot path: `sendBufferedFrame()` sets up a resumable send and `drainPreviewSend()` (called each `tick20ms`) walks a per-client cursor a **chunk at a time**, so no single step does a large blocking write. The state push never got this treatment.

**Chosen fix (PO):** extend the existing resumable sender to carry the state frame too — *one* core transport mechanism for both large frames, not a second parallel drain loop (Complexity-lives-in-core: extend the mechanism to the sibling path, don't re-implement it). Payload-size optimisation (diffing, per-module gating) is explicitly NOT the fix: the stutter is *where* the 20 KB runs (one blocking write between two render frames), not its size — shrinking it still spikes the render tick. Per-module "only send the watched module" is rejected: the UI renders the whole tree at once (no single selected module), it needs per-client server-side selection state (more complexity, not less), and it doesn't remove the on-render-thread blocking write.

## Verified current state (file:line)

- **`tick1s()`** (`HttpServerModule.cpp:72`) → `pushStateToWebSockets()` (`:1995`): builds JSON into a `JsonSink` heap buffer via `buildStateJson()` (`:802`), then per client `sendWsTextFrame(ws, sink.data(), sink.size())` (`:2009`). Also `pushWledStateToWebSockets()` (`:2025`) — a *small* `{state,info}` frame, not the problem, leave on tick1s.
- **`sendWsTextFrame()`** (`:2093`): builds a 2/4-byte text-opcode (0x81) WS header, then `conn.write(header)` + `conn.write(data, len)` — **blocking full-payload write**. This is the spike.
- **The resumable sender** (`:2172` `sendBufferedFrame`, drained by `drainPreviewSend` on `tick20ms` `:62`): `previewSend_` holds `{hdr[16], hdrLen, const uint8_t* body /*BORROWED*/, bodyLen, sent[MAX_WS_CLIENTS], active}`. One frame in flight (`if (previewSend_.active) return false` — drop-new backpressure). WS header built as **binary opcode 0x82** (`:2184`). Cursor per client walks `[hdr ++ body]`; a client that can't keep up is closed.
- **Lifetime mismatch (the crux):** preview *borrows* `body` (PreviewDriver keeps the pixel/coord buffer alive across the drain). The state JSON is built fresh each second into a local `JsonSink` that would be destroyed at the end of `tick1s` — so the resumable path must **own** the state body for its lifetime, or it dangles mid-drain.
- **`tick20ms()`** (`:53`) already the transport-poll cadence (drains preview, polls WLED input, accepts one HTTP conn). The state drain belongs here too.

## Design

### 1. Let the resumable sender optionally OWN its body + carry a text opcode

Extend `previewSend_` (rename-neutral: it's now the general "buffered WS frame" state, but keep the field name to avoid a churn diff unless it reads wrong — decide at implementation; a `bufferedSend_` rename is optional polish, not required):

- Add `bool ownsBody` and `uint8_t opcode` to the struct.
- `sendBufferedFrame(...)` gains an `opcode` (default 0x82 binary, preserving preview callers byte-for-byte) and an `ownedBody` variant: when the caller hands ownership, the struct stores the heap pointer and frees it when the drain completes (all clients sent, or the send is torn down in `release()` / on abort).
- The WS header build at `:2184` uses the passed `opcode` instead of the hardcoded `0x82`.
- `drainPreviewSend()` on completion: if `ownsBody`, `free()` the body and clear the pointer. Same on `release()` (`:47` already sets `active=false` — add the conditional free).

This is the minimal generalisation: preview stays a borrowed-body binary frame (unchanged behaviour); state becomes an owned-body text frame through the same cursor/drain machinery.

### 2. State push: serialize on tick1s, hand off, drain on tick20ms

- **`tick1s()`**: keep `pushWledStateToWebSockets()` (small, fine inline). Replace the big push: if there are WS clients AND no buffered send is already active, serialize `buildStateJson()` into a **heap buffer this function allocates** (not a stack `JsonSink` — the body must outlive the call), then `sendBufferedFrame(nullptr, 0, ownedBuf, len, /*opcode=*/0x81, /*ownsBody=*/true)`. If a buffered send is still active (previous frame not fully drained, or a preview frame in flight), **skip this second's state push** — drop-new, the next second's state is fresher anyway. (Same drop-new logic the preview producer already relies on.)
- **`tick20ms()`**: unchanged — `drainPreviewSend()` already runs here and now also drains the state frame (it's the same `previewSend_` cursor). The 20 KB streams out a chunk per 20 ms across ~N steps, never one blocking write on a render frame.
- **Delete** the blocking-full-write path from `pushStateToWebSockets` (the `sendWsTextFrame` per-client loop for the *big* state frame). `sendWsTextFrame` itself stays if any small-frame caller remains; check callers — if the WLED small-state push uses it and is fine inline, keep it for that; the big state frame stops using it.

### Interaction: state vs preview share one in-flight slot

Both now contend for the single `previewSend_` slot (one frame in flight). That's acceptable and even desirable: on a memory-tight board only one large frame drains at a time. Priority is drop-new/first-come: whichever started drltns first finishes; the other's producer sees `active` and sheds that frame (preview drops a frame → lower preview fps momentarily; state drops a second → next second's state covers it). Neither blocks the render tick. If contention ever proves visible (preview starving state or vice-versa) the follow-up is a 2-slot sender — backlogged, not built now (YAGNI; one slot matches today's single-large-frame-at-a-time reality).

## Files

- **`src/core/HttpServerModule.h`**: `previewSend_` struct gains `ownsBody` + `opcode`; `sendBufferedFrame` signature gains `opcode` + ownership (or an overload).
- **`src/core/HttpServerModule.cpp`**: generalise `sendBufferedFrame` (`:2172`) + `drainPreviewSend` (free-on-complete) + `release()` (`:47`, free-if-owned); rewrite `pushStateToWebSockets` big-frame path to serialize-into-owned-heap + hand off; `tick1s` skip-if-active. WS header opcode parameterised.
- **Tests** (`test/unit/core/` — an HttpServer/transport test if one exists, else a focused new one): a buffered TEXT frame with an owned body drains to completion across multiple `tick20ms` calls and frees its buffer (no leak — ASAN); a second `sendBufferedFrame` while one is active returns false (drop-new); `release()` mid-drain frees an owned body. Pin that the render `tick()` path no longer calls the blocking state write.
- **Docs**: `architecture.md § Parallelism` / the render-transport seam note already describes the preview drain; add one clause that the state frame shares it. No new module doc.

## Verification

1. `cmake --build build` clean (0-warn); `ctest` + scenarios green; JS unchanged.
2. **ASAN**: owned-body frame frees exactly once (drain-complete OR release-mid-drain), never double-frees, never leaks. This is the one new lifetime path — exercise it hard.
3. **On hardware (192.168.x P4)**: open the UI (WS client active), watch the LEDs — the 1 Hz stutter is gone. Confirm the browser still gets a full state update ~1/s (values live, cards update). Confirm preview still streams. Confirm both-at-once (preview + state) doesn't stall the render tick (KPI tick time flat vs. before).
4. **KPI**: render tick time no longer shows a 1 Hz spike; per-tick cost of `tick20ms` rises slightly (a state chunk) but bounded and off the render `tick()`.

## Scope guard

Transport-only: move the *existing* 20 KB state frame off the render tick by reusing the *existing* resumable sender, generalised for owned-body + text opcode. Do NOT diff/shrink the payload (separate bandwidth concern, not the stutter), do NOT add per-client module selection, do NOT build a second drain loop, do NOT add a `tick1sUI` hook (the fix needs no new per-module surface — it's entirely in HttpServerModule's transport). One in-flight slot stays; a 2-slot sender is backlogged only if contention proves visible.
