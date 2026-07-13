#pragma once

#include <cstdint>
#include <cstdlib>  // std::strtol
#include "core/Control.h"  // MM_MAX_GPIO — the build-injected per-chip GPIO ceiling

namespace mm {

// Domain-neutral core primitive: parse a comma-separated GPIO list ("18,17,16"). Lives in core (not
// the light layer) because both a light driver's `pins` control AND the core PinsModule's pin-ownership
// map read the same CSV — so the dependency runs domain→core, never core→light. strtol-based like
// parseDottedQuad (Control.h). Returns nullptr on success or a static error literal the caller feeds
// straight into setStatus(). Host-tested by unit_RmtLedDriver_pins.cpp + unit_PinsModule.cpp.

// Parse "18,17,16" into out[0..maxPins). Spaces around tokens are fine (strtol skips them). Rejects
// empty input, bad tokens, trailing commas, duplicates, out-of-range pins (> the chip's MM_MAX_GPIO
// ceiling), and more than maxPins entries (the chip's channel/lane cap). The range check is the
// crash guard: an in-CSV pin like 999 parses as a valid integer but is not a GPIO — handing it to
// IDF's gpio_func_sel() faults ("GPIO number error" → reset, WROVER bench 2026-07-13). Rejecting it
// here — at the one parse boundary every driver (RMT/Parlio/i80) shares — keeps a garbage pin from
// ever reaching hardware, so no driver has to re-guard it.
inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    nOut = 0;
    if (!s || !*s) return "invalid pin list";
    const char* p = s;
    while (true) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0 || v > 0xFFFF) return "invalid pin list";
        if (v > MM_MAX_GPIO) return "pin out of range for this chip";
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
