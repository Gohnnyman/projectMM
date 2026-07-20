# 13. No migration code — robust persistence + documented breaks

Status: Accepted

## Context

Persisted config and UI state carry keys/values that drift as the schema evolves (a control renamed, a config file renamed, a wire key re-spelled). The reflex fix is *migration code*: a fallback that reads the old key when the new one is absent, or a one-time cleanup that deletes files a renamed type wrote. Two such migrations had accumulated (a `FilesystemModule::migrateRenamedConfigs()` deleting `LayoutGroup.json`/`DriverGroup.json`, and a `lsRead(key, legacyKey, default)` localStorage fallback), and a review proposed a third (fall back to a legacy `colour` key after the US-spelling rename).

Migration code is **absence-narration in code form**: it exists only to describe a past state a present-tense reader never sees, which fights CLAUDE.md's *Present tense only* principle. It also accretes — each rename tempts one more fallback, and they become permanent dead weight because no one remembers to remove them. A *patching framework* (version-stamp the persisted file, run an ordered chain of patch functions) was considered as the disciplined alternative, but it is still migration code — just centralized — and building the framework before there are several real migrations to hold is premature abstraction (*Concrete first, abstract later*; *Core grows slower than the domain*).

The realization that settles it: **the persistence layer is already robust to schema drift**, so migrations are rarely needed at all. On load, an **absent key** keeps the control's `defineControls()` default (the `hasKey` guard); a **stale/out-of-range value** snaps to the new bounds via `ApplyPolicy::Clamp`; an **unknown key** is ignored. The only genuinely lossy case is a renamed *value* key, and pre-1.0 a lost setting simply re-populates on next use (a cached device re-confirms on its next live packet; a UI selection resets to the first module).

## Decision

**No migration code in the system.** Persistence stays robust-by-default (absent → default, stale → clamp, unknown → ignore), which absorbs almost all schema drift with zero migration-specific code. A breaking format change (a persisted key, config filename, wire key, or localStorage key that a *released* version wrote and a new version reads differently) is **documented, not migrated** — recorded in [MIGRATING.md](../MIGRATING.md), one entry per break with the action it costs the user, so the record is present-tense prose rather than past-narrating code.

A **patching framework is explicitly deferred**, not rejected forever: it becomes the right tool post-1.0 *if* breaking format changes become frequent enough that ad-hoc losses pile up (a rough bar: >5 across a few releases) *and* users hold persisted state too valuable to re-derive. At that point build the recognizable version-stamp + ordered-patch-chain pattern (SQLite `user_version`, Rails/Room migrations), not a bespoke one. Until then, the cost of not migrating is a re-populated setting, which the robust-by-default layer already makes cheap.

## Consequences

- The two existing migrations were removed: `FilesystemModule::migrateRenamedConfigs()` (and its call + declaration) and the `lsRead` legacy-key parameter. The proposed `colour`-fallback was not added.
- **Robust-by-default is the contract**: a schema change must degrade gracefully through absent-default / clamp / ignore, never crash or wedge (the *Robust to any input* principle already guards this in tests). A change that would lose data silently is documented here instead of papered over with a fallback.
- **Known breaking changes are logged in [MIGRATING.md](../MIGRATING.md)** — one entry per break, newest first, each with the action it costs the user (usually none: the value re-populates on next use). That log is the home for this decision's output; it lives outside the ADR because it *grows* with every break, and an ADR is immutable (superseded, never edited).
- The lesson that generalizes: a robust reader is worth more than a migration — build the loader to tolerate drift, and most migrations never need to exist. Reach for a patching framework only when real, frequent, high-value breaks prove it earns its complexity.
