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

// Parse "18,17,16" into out[0..maxPins). A token may be a single pin ("17") or an inclusive RANGE
// ("20-23" → 20,21,22,23), and the two mix freely ("20-22,35,38-40") — the same range idiom as the
// IP-destination list (parseIpList), so a human types consecutive pins once. Spaces around tokens are
// fine (strtol skips them). Rejects empty input, bad tokens, trailing commas, duplicates, ranges that
// run backwards (hi < lo), out-of-range pins (> the chip's MM_MAX_GPIO ceiling), and more than maxPins
// entries (the chip's channel/lane cap). The ceiling check is the crash guard: an in-CSV pin like 999
// parses as a valid integer but is not a GPIO — handing it to IDF's gpio_func_sel() faults ("GPIO number
// error" → reset, WROVER bench 2026-07-13). Rejecting it here — at the one parse boundary every driver
// (RMT/Parlio/i80) shares — keeps a garbage pin from ever reaching hardware, so no driver has to re-guard it.
//
// Append one pin, enforcing the chip ceiling, the duplicate check, and the lane cap. Returns an error
// literal or nullptr; used for both a single token and each step of an expanded range.
inline const char* appendPin(long v, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    if (v < 0 || v > 0xFFFF) return "invalid pin list";
    if (v > MM_MAX_GPIO) return "pin out of range for this chip";
    if (nOut >= maxPins) return "too many pins for this chip";
    for (uint8_t i = 0; i < nOut; i++)
        if (out[i] == static_cast<uint16_t>(v)) return "duplicate pin";
    out[nOut++] = static_cast<uint16_t>(v);
    return nullptr;
}

inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins, uint8_t& nOut) {
    nOut = 0;
    if (!s || !*s) return "invalid pin list";
    const char* p = s;
    while (true) {
        char* end = nullptr;
        const long lo = std::strtol(p, &end, 10);
        if (end == p) return "invalid pin list";
        while (*end == ' ') end++;
        if (*end == '-') {                          // a RANGE: expand lo..hi inclusive
            const char* after = end + 1;
            char* e2 = nullptr;
            const long hi = std::strtol(after, &e2, 10);
            if (e2 == after) return "invalid pin range";
            if (hi < lo) return "pin range runs backwards";
            for (long v = lo; v <= hi; v++)
                if (const char* err = appendPin(v, out, maxPins, nOut)) return err;
            end = e2;
        } else {
            if (const char* err = appendPin(lo, out, maxPins, nOut)) return err;
        }
        while (*end == ' ') end++;
        if (*end == '\0') return nullptr;
        if (*end != ',') return "invalid pin list";
        p = end + 1;
    }
}

} // namespace mm
