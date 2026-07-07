# 5. A generic set-control primitive on the Scheduler

Status: Accepted

## Context

Adding IR remote control raised: how does one module drive another module's control? The wrong answer is a bespoke seam per target (`Drivers::adjustBrightness`, then `setPalette`, and N more one-offs). The right primitive already existed half-hidden: the WLED-app bridge set brightness via `applySetControl(module, control, value)`, the generic find → validate → apply → `onUpdate` → persist → conditional-`buildState` path, but it was private on HttpServerModule.

## Decision

Lift `setControl` onto the `Scheduler` (which owns the module tree and the persistence hook), and make HttpServer's `applySetControl` a thin result→HTTP-status mapper. Every input (IR, HTTP, Improv, the WLED bridge) composes against one control-agnostic primitive. A factory-created module reaches it through `Scheduler::instance()` (the `FilesystemModule::instance_` pattern), so no per-module inject is needed.

## Consequences

- Adding a new input transport never adds control plumbing; HttpServer shrank ~54 lines.
- The rule that generalised: when a second caller wants a capability a transport implemented privately, the capability belongs in core (the tree owner), and the transport keeps only its status-mapping.
