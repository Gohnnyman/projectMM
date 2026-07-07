# 2. Adaptive allocation with a degradation cascade

Status: Accepted

## Context

The light pipeline runs on devices from a no-PSRAM ESP32 (~180 KB free internal heap) to a PSRAM-rich P4. A fixed buffer scheme either wastes memory on small installs or fails to fit large ones. The pipeline has intermediate buffers (mapping LUT, driver output buffer) that a 1:1 unshuffled layout does not need at all.

## Decision

Allocate intermediate buffers on demand, only when the pipeline actually needs them, and degrade under memory pressure rather than fail:

- The **mapping LUT** is built only when modifiers exist and the layout is not a plain grid and heap remains after reserving `HEAP_RESERVE` (32 KB) for stack/HTTP/WiFi.
- The **driver output buffer** is built only when a LUT is actually allocated.
- When memory is insufficient, degrade in order: full pipeline → skip the output buffer (map inline) → skip the LUT (forced 1:1) → reduce layer dimensions (halve to a floor of 8×8).

Each level is observable (`degraded()`, `lutSkipped()`, `outputBufferSkipped()`). Every allocation is predict-then-measure: predict from dimensions + channels + modifiers, compare the heap delta, and flag >5% variance as a leak.

## Consequences

- A 1:1 unshuffled layout allocates zero intermediate buffers; ArtNet reads the layer buffer directly. Maximum LED count at minimum memory.
- The device stays running under pressure (degraded is acceptable, crashed is not), instead of failing an allocation outright.
- The mechanism and buffer-type detail live in [architecture.md § Memory strategy](../architecture.md#memory-strategy); this ADR records the decision to make allocation adaptive rather than fixed.
