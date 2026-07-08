#pragma once

#include <cstdint>

namespace mm {

/// A small plain-data summary of the light pipeline's output, produced by the light domain
/// (the `Drivers` container) and consumed by domain-neutral core consumers — the WLED `/json`
/// shim and MQTT — so they can report the real device shape without including any light-domain
/// class or type. This is the shared-struct pull pattern (see architecture.md § Data exchange,
/// the same shape as `AudioFrame`): the producer owns one POD overwritten in place on each
/// rebuild, the consumer holds a `const LightSummary*` and reads it; no allocation, no event bus.
///
/// Domain-neutral on purpose: plain `uint32_t`/`uint8_t`, no light typedefs, so it stays in core
/// with the consumers. `lightCount` is `uint32_t` to hold any count on any board (the light-side
/// `nrOfLightsType` is `uint16_t` without PSRAM, `uint32_t` with; `uint32_t` here covers both).
///
/// **Extendable by design.** Add a field (estimated power, segment count, RGBW flag) and the
/// producer fills it in one place; every consumer that wants it reads it, no new seam. Keep it a
/// flat POD of small integers.
struct LightSummary {
    uint32_t lightCount = 0;              ///< total physical lights driven (Layer::physicalLightCount()).
    uint8_t  channelsPerLight = 3;        ///< 3 = RGB, 4 = RGBW, more = multi-channel DMX fixtures.
};

} // namespace mm
