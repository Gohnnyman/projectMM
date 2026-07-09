# Plan — Pins increment #3: conflict soft-flag

Pins top-down [increment #3 / §4](../../backlog/pins-analysis-top-down.md#4-the-conflict-authority-phase-2): flag a GPIO claimed by two or more controls. Builds on #2 (per-claim severity, shipped). PO-directed; **soft-flag** chosen (robustness over reject-on-add).

## Context

Today two modules can claim the same GPIO and nothing surfaces it beyond the map listing both rows (`RmtLed.pins="18"` twice, or a mic `wsPin` colliding with an LED lane — the exact GPIO-21 collision the S3 map already showed). The top-down §4 resolves the conflict UX to **soft-flag**: the claim lands (never wedge a running device or a live pin-swap — *robustness to any input*), but the map makes the conflict loud. Reject-on-add is explicitly deferred (installer/catalog path only); output-suppression and the shared-pin opt-out are later.

This is deliberately the *smallest* useful conflict step: **surface**, don't enforce. It reuses #2's `severity` field and the map's existing "list both owners of a shared GPIO" behavior — so there is no new HTTP hook, no persistence, no `Scheduler::setControl` change. The "authority both consult" (validators calling a `pinConflicts()` query) is a *later* increment; forcing it now would turn a read-only map into a policy engine (the scope-guard the top-down draws).

## Verified seams (from #1/#2, already in PinsModule.h)

- `refresh()` collects all claims into `claims_[]` then GPIO-sorts them ([PinsModule.h](../../../src/core/PinsModule.h)). After the sort, equal-GPIO claims are adjacent — a single linear pass marks conflicts.
- `Claim::severity` (added in #2) is the exact field the UI already colours (`rowSeverityClass` → red/yellow row). A conflict = `severity="error"`.
- `writeListRowDetail` already lists every claim on a GPIO (the double-claim is already *visible* in the detail); #3 adds the *summary-level* red flag so it's loud without expanding.
- The generic UI (app.js `rowSeverityClass`, shipped in #2) needs **no change** — a conflict just sets the same `severity` field.

## Design

**One pass in `refresh()`, after the GPIO sort.** Walk the sorted `claims_`; for any run of ≥2 claims sharing a `gpio`, set each run member's `severity` to `"error"` (a conflict is at least as severe as a strap-warn; a conflict on a strap is still "error"). Precedence: a reserved/invalid pin is already `"error"` from #2 — a conflict doesn't downgrade it; and a conflict promotes a `"warn"` (strap) or none to `"error"`. So the rule is simply: **conflict ⇒ severity = "error"** (the max).

Because the strings are static literals and severity is already a `const char*`, this is a couple of lines — no new field, no storage. The detail already shows the co-owners, so a user seeing the red row expands it and reads "GPIO 21: I2cScan · I²C SDA / Audio · data" — the conflict named.

Optional (small, decide while building): add a `"conflict"` boolean or reason to the row so the UI *could* later distinguish "unsafe pin" from "double-claimed" — but per subtraction, only if it earns it. Default: reuse `severity="error"`; the detail already disambiguates. **Lean: no new field.**

## Files

- **Edit:** `src/core/PinsModule.h` — the post-sort conflict pass in `refresh()` (+ a `///` line). `docs/moonmodules/core/system.md` — the Pins card notes a double-claim now flags red at the summary.
- **Tests:** `unit_PinsModule.cpp` — the existing "double-claim stays visible in the detail" test extends to assert BOTH conflicting rows now carry `severity="error"`; a single-claim safe pin still carries none; a conflict-on-a-strap is "error" not "warn".

## Verification

1. Build clean; ctest (the extended double-claim test); spec; boundary; scenarios; ESP32 build; JS (no app.js change, but run the gate).
2. Live on S3/P4: the pre-existing GPIO-21 collision (I2cScan I²C SDA vs Audio data) now shows a **red** row at the summary, not just in the detail. Set two LED lanes to the same GPIO → red; separate them → clears. No wedge, no reboot.

## Scope guard

Surface only. **NO** reject-on-add, **NO** output suppression, **NO** `Scheduler`/HTTP validator hook (that's the "authority both consult" — a later increment), **NO** shared-pin opt-out (later). The map flags; it does not enforce or persist. Reuse #2's `severity` — do not add a parallel conflict-colour path in the UI. Mark `(shipped)` when it lands.
