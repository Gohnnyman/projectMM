# 7. MoonLive is expressions + host-bound functions, not statement shapes

Status: Accepted

## Context

The MoonLive compiler was first built around a fixed *statement shape*, `setRGB(idx, r, g, b)`, with per-slot parser rules (the index could be `random16`, colors were literal-only) and an RGB-specific `Store` op baked into the core. Three product-owner remarks exposed one root flaw: `random16` worked only in the index slot; `random16(255)` capped at a byte (validators conflated ranges); and the core compiler was light-domain-specific.

## Decision

Adopt the ESPLiveScript / ARTI model: the core knows only **expressions plus a generic call mechanism**; the host registers its functions in a builtin table. Every argument parses as an expression (a literal or a nested call), so `setRGB(random16(256), random16(256), 30, 0)` works and a number is a `uint16`. The LED names and RGB meaning live only in the light-domain registration (`MoonLiveBuiltins_light.h`); the core sees a neutral `BuiltinTable` of `{name → Call(fn ptr) | Inline(opcode tag)}`, where a buffer writer is `Inline` (the hot-path fast path) and a pure helper is `Call`.

## Consequences

- A capability the language "can't express in slot Y" is fixed by real expressions, not a per-slot special case.
- Domain-neutrality is testable: a test asserts the core with an empty builtin table knows no functions.
- Two codegen contracts fell out and generalise to any per-ISA backend: a value live across a call must survive it (save/restore the caller-saved set), and register budget is real on the MCU, so a tree-walk register stack with a free-list allocator keeps N calls at a handful of registers, not 2N (this surfaced on the P4 RISC-V backend, the first target where a 4-call statement exhausted the pool).
