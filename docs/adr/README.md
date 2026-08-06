# Architecture Decision Records

An [ADR](https://github.com/joelparkerhenderson/architecture-decision-record) captures one significant architectural decision: the context that forced a choice, the option taken, and the consequences that followed. Format is [Michael Nygard's classic](https://cognitect.com/blog/2011/11/15/documenting-architecture-decisions.html): **Title, Status, Context, Decision, Consequences**.

These records are **immutable except the status line**: a decision that changes is not edited in place — a new ADR supersedes it, the old one's status gains a dated pointer to its successor (`Superseded by ADR-NNNN, YYYY-MM-DD`, or a dated `Amended:` note), and both link, so the reasoning trail stays honest while every reader lands on a signpost to current truth. This is the difference from the [lessons log](../history/lessons.md): lessons are debugging war-stories, pruned as they are absorbed; ADRs are decisions, kept as an append-only record. The forward-looking counterpart, what we set out to build, is the [plan archive](../history/plans/README.md).

Agents do not read this directory automatically, only when a decision's rationale is in question (the same rule as `history/` and `backlog/`).

## Index

| # | Decision | Status |
|---|----------|--------|
| [0001](0001-persistence-pod-memcpy-not-json.md) | Persist POD module state with memcpy, not JSON | Accepted |
| [0002](0002-adaptive-memory-degradation-cascade.md) | Adaptive allocation with a degradation cascade | Accepted |
| [0003](0003-layer-buffer-persists-frame-to-frame.md) | The layer buffer persists frame-to-frame | Accepted |
| [0004](0004-composable-modifiers-physical-to-logical.md) | Build the map physical→logical so modifiers compose | Accepted |
| [0005](0005-set-control-primitive-on-scheduler.md) | A generic set-control primitive on the Scheduler | Accepted |
| [0006](0006-device-discovery-udp-mdns-advertise-only.md) | UDP presence for discovery, mDNS advertise-only | Accepted |
| [0007](0007-moonlive-expressions-host-bound-functions.md) | MoonLive is expressions + host-bound functions | Accepted |
| [0008](0008-board-injection-name-only-http-fanout.md) | Board injection: SET_BOARD name-only, controls over HTTP | Accepted |
| [0009](0009-docs-generated-technical-plus-summary.md) | Two doc surfaces: generated technical + hand-written summary | Accepted |
| [0010](0010-integration-identity-stable-hardware-id.md) | Integration identity is a stable hardware id | Accepted |
| [0011](0011-data-exchange-pull-and-prepare-pass-not-pubsub.md) | Inter-module data/events: pull + prepare-pass, not pub/sub | Accepted |
| [0012](0012-ha-discovery-wled-default-mqtt-opt-in.md) | HA discovery: WLED by default, MQTT discovery opt-in | Accepted |
| [0013](0013-no-migration-code-robust-persistence-plus-documented-breaks.md) | No migration code — robust persistence + documented breaks | Accepted |
| [0014](0014-own-i80-dma-driver-below-esp-lcd.md) | Our own i80 DMA driver, one level below esp_lcd | Accepted |
| [0015](0015-library-is-a-tag-not-a-folder.md) | The source tree splits by domain/type; library origin is a tag, not a folder | Accepted |
| [0016](0016-one-parallel-led-driver-runtime-peripheral-strategy.md) | One parallel LED driver with a runtime peripheral strategy, not three CRTP subclasses | Accepted |
