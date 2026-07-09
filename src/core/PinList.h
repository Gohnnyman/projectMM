#pragma once

#include <cstdint>
#include <cstdlib>  // std::strtol

namespace mm {

// Domain-neutral core primitive: parse a comma-separated GPIO list ("18,17,16"). Lives in core (not
// the light layer) because both a light driver's `pins` control AND the core PinsModule's pin-ownership
// map read the same CSV — so the dependency runs domain→core, never core→light. strtol-based like
// parseDottedQuad (Control.h). Returns nullptr on success or a static error literal the caller feeds
// straight into setStatus(). Host-tested by unit_RmtLedDriver_pins.cpp + unit_PinsModule.cpp.

// Parse "18,17,16" into out[0..maxPins). Spaces around tokens are fine (strtol skips them). Rejects
// empty input, bad tokens, trailing commas, duplicates, and more than maxPins entries (the chip's
// channel/lane cap).
inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    nOut = 0;
    if (!s || !*s) return "invalid pin list";
    const char* p = s;
    while (true) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 0xFFFF) return "invalid pin list";
        while (*end == ' ') end++;
        if (nOut >= maxPins) return "too many pins for this chip";
        for (uint8_t i = 0; i < nOut; i++)
            if (out[i] == static_cast<uint16_t>(v)) return "duplicate pin";
        out[nOut++] = static_cast<uint16_t>(v);
        if (*end == '\0') return nullptr;
        if (*end != ',') return "invalid pin list";
        p = end + 1;
    }
}

} // namespace mm
