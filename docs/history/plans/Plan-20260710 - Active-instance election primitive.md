# Plan — A core primitive for the "one active instance" election

## Context

Product-owner remark (during the hook-rename work): `if (active_ == this) active_ = nullptr;` reads like orchestration a module shouldn't hand-maintain — the core should own it.

The pattern is a **singleton election**: several instances of a module can exist (two mics, or the device-registry), but exactly *one* is "the active one" that a consumer reaches through a `static active()` accessor. Each such module hand-writes the same four moves:

- **claim if empty** — `if (active_ == nullptr) active_ = this;` (first live instance wins)
- **vacate if mine** — `if (active_ == this) active_ = nullptr;` (on teardown/disable)
- **vacate on destruct** — `~Module() { if (active_ == this) active_ = nullptr; }` (the dangling-static guard)
- **self-elect if empty** — in `tick()`: `if (active_ == nullptr) active_ = this;` (a survivor takes the empty seat so the consumer never reads a dead pointer)

This is duplicated today across:
- **`AudioService`** ([AudioService.h](../../../src/core/AudioService.h)) — `active_` is the live mic; consumers read `AudioService::latestFrame()`.
- **`DevicesModule`** ([DevicesModule.h](../../../src/core/DevicesModule.h)) — `active_` is the boot device-registry; a light-domain Hue driver reaches it via `DevicesModule::active()`.

It's the same *[Complexity lives in core](../../../CLAUDE.md#principles)* smell the [ScratchBuffer plan](Plan-20260710%20-%20Scratch%20buffer%20helper%20for%20memory-holding%20effects.md) catches, one level up: non-trivial election bookkeeping repeated per module, each copy a chance to get the dangling-static or the self-elect wrong (both were real bugs the disable-releases-resources work fixed). A core primitive removes the four moves from the module — it just declares "I participate in this election" and reads the winner.

## Design sketch (resolve details in review)

A small header primitive — one static seat per participating type, claimed/vacated by RAII, self-healing:

```cpp
// core/ActiveInstance.h
//
// Per-TYPE singleton seat. Declare one as a member; it claims the seat when the
// owner becomes live and vacates it (and self-heals to a surviving instance)
// automatically. Consumers read Seat<T>::active().
template <class T>
class ActiveInstance {
public:
    explicit ActiveInstance(T& self) : self_(self) {}
    ~ActiveInstance() { if (seat_ == &self_) seat_ = nullptr; }   // dangling-static guard, for free

    void claim()  { if (!seat_) seat_ = &self_; }   // first live wins; a 2nd is captured but not seated
    void vacate() { if (seat_ == &self_) seat_ = nullptr; }
    bool seated() const { return seat_ == &self_; }

    static T* active() { return seat_; }            // the consumer accessor
private:
    T&   self_;
    static inline T* seat_ = nullptr;
};
```

An effect/service then holds `ActiveInstance<AudioService> seat_{*this};` and calls `seat_.claim()` in its build hook and `seat_.vacate()` in teardown — or, cleaner, the base wires those two into the lifecycle so the module writes nothing (the same "owner tie does it" mechanic the ScratchBuffer plan uses). The `latestFrame()`/`active()` accessors become `ActiveInstance<T>::active()`.

**Open decisions:**
1. **How much does the module still write?** Ideal is zero — the base claims on build (when effectively-enabled) and vacates on teardown, mirroring how `applyState()` already routes those. The self-elect-in-tick (a survivor grabbing an empty seat) is the one bit that may need to stay a one-liner in the module, or move into the base tick. Resolve by trying AudioService first.
2. **Naming under the hook-rename.** This lands after (or with) the [prepare/tick/release rename](Plan-20260710%20-%20Rename%20module%20hooks%20to%20prepare-tick-release%20(decided).md) — so "claim in `onBuildState`" becomes "claim in `prepare`", "vacate in `teardown`" becomes "vacate in `release`". Author against the new names if the rename ships first.
3. **Coordinate with ScratchBuffer's owner tie.** Both primitives want a `MoonModule&`/owner reference to hook the lifecycle. Consider whether they share one mechanism (a module holds a small list of "lifecycle participants" the base drives) or stay two independent RAII members. Lean: two independent members — simpler, and they're genuinely different concerns; only unify if a third participant appears.

## Files

- **New:** `src/core/ActiveInstance.h` + `test/unit/core/unit_ActiveInstance.cpp` (claim-first-wins, vacate-only-if-mine, self-heal, destructor vacates — the dangling-static regression).
- **Migrate:** `AudioService` (mic seat) and `DevicesModule` (registry seat) — remove the four hand-written moves + the static `active_` + the destructor guard, replace with the member.
- **Docs:** the guide doesn't teach this (it's a core-services concern, not an effect-author one), so no guide change — but drop a line in `coding-standards.md` if a third participant makes it a general pattern.

## Verification

1. Build + ctest + scenarios green; the existing `unit_AudioService` two-mic election test and `unit_DevicesModule` active-seat test must pass **unchanged** (they assert the *behaviour* — first wins, survivor re-elects, destruct vacates — which the primitive must preserve).
2. ASAN clean across construct/claim/vacate/destruct in any order (the dangling-static guard is the whole point).
3. Grep confirms the four `active_ == this` / `active_ == nullptr` moves are gone from the modules.

## Scope guard

One small primitive + two module migrations. Behaviour-preserving. Don't over-generalize to a "registry of named singletons" — there are exactly two participants; a per-type static seat is the minimal correct shape. Fold into the lifecycle-primitive family (sibling of ScratchBuffer) but keep it a separate header — different concern.
