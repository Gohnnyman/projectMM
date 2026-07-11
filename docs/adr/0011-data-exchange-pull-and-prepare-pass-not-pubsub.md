# 11. Inter-module data and events: pull + prepare-pass, not pub/sub

Status: Accepted

## Context

Modules need two things from each other: one module reads another's data on the hot path (an effect reading a sensor frame, a driver reading a layer buffer), and a change in one module triggers derived-state rebuilds in others (a control edit resizing buffers, a tree mutation re-resolving links). The obvious general-purpose answer is a publish/subscribe event bus with a registry and listener lifecycles.

## Decision

Do not use pub/sub. There is one producer per data kind and the consumer explicitly wants that specific data, so the registry overhead and listener-lifecycle complexity buy nothing. Use three narrower mechanisms instead:

- **Shared-struct pull** for hot-path data: the producer owns a small POD struct overwritten in place each tick; the consumer holds a `const Foo*` (set at wiring time) and reads it per frame. Lock-free for a small POD (a half-updated read self-corrects next tick); a large frame buffer uses the two-core double-buffer swap instead, not this pull.
- **Push to a domain-neutral sink** when a producer hands bytes to a generic core service: the core defines a narrow interface (`BinaryBroadcaster`), the producer pushes, the sink knows nothing about the payload.
- **A framework-driven prepare-pass** for derived-state rebuilds: a three-tier split (`onControlChanged` per-control, a `affectsPrepare` gate, `prepare` rebuild) where the coordinator walks every module, gated by per-module metadata. This is the recognised layout/prepare-pass pattern (JUCE `prepareToPlay`, UIKit `layoutSubviews`, WPF `AffectsMeasure`), not an event bus: the publisher tells the coordinator to run the pass, the coordinator walks every module.

Direct method calls cover the one remaining case (a producer notifying one known consumer): the producer holds a pointer set at wiring time and calls it.

## Consequences

- No registry, no subscription, no listener lifecycles to leak; each mechanism costs only what its case needs.
- Every change costs proportionally: an in-place tweak is tier-1 only; a shape change ripples through the tree-wide sweep; a structural mutation always rebuilds.
- The mechanism is core and domain-neutral; the light pipeline consumes it (mapping-LUT rebuild, buffer pull) without the core knowing about lights.
- Pub/sub becomes the right pattern only if multiple unknown subscribers per event ever appear; projectMM has none. The mechanisms are described as current behaviour in [architecture.md § Data exchange](../architecture.md#data-exchange-between-modules) and [§ Event triggering](../architecture.md#event-triggering-between-modules).
