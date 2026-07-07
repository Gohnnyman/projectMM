# 4. Build the map physical→logical so modifiers compose

Status: Accepted

## Context

Modifiers needed to chain (Region, then Multiply, then Rotate). The old interface `mapToPhysical(logicalCoord) → [physical indices]` (a logical→physical fan-out) did not compose: stages emitted flat indices, not coordinates, and chaining would need a product-of-`maxMultiplier` fan-out ceiling, the exact `uint16` overflow that black-screened the high-fan-out MultiplyModifier.

## Decision

Invert the map build to **physical→logical**, adopting MoonLight's proven model written in projectMM's own code. Each modifier is an in-place coordinate fold with three hooks: `modifyLogicalSize`, `modifyLogical` (returns false to reject a coordinate), and `modifyLive` (for per-frame transforms like Rotate). The Layer walks the *physical* lights and folds each through the chain. Because a scatter onto arbitrary logical keys does not fit `setMapping`'s in-order contract, the build is a textbook counting-sort CSR construction (count, prefix-sum, scatter, replay) entirely on the cold path.

## Consequences

- Fan-out becomes free: N physical lights folding onto one logical cell *is* the fan-out, with no fan-out list, no product ceiling, no overflow. `destinationCount ≤ driverCount` is now a hard invariant; the `maxMultiplier`/scratch-buffer machinery was deleted.
- The per-frame read is byte-identical; the hot path is untouched.
- Static folds (mask/tile/crop) happen at build time; only rotation gathers per frame, so a static-only chain pays nothing per frame.
