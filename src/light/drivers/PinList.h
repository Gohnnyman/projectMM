#pragma once

#include "core/PinList.h"        // parsePinList — the domain-neutral GPIO-CSV parser (core primitive)
#include "light/light_types.h"  // nrOfLightsType

#include <cstdint>
#include <cstdlib>  // std::strtol

namespace mm {

/// @defgroup PinList Pin + light-count list parsing
/// @{
///
/// The LIGHT-DOMAIN half of pin/count list parsing for multi-output LED drivers — RmtLedDriver (one RMT
/// channel per pin) and MultiPinLedDriver (one i80 data lane per pin) drive consecutive slices of the source
/// buffer from two text controls. The GPIO-CSV parser (`parsePinList`) is a domain-neutral core primitive
/// (core/PinList.h); the LED-count distribution below (`assignCounts`, which speaks nrOfLightsType) stays
/// here in the light layer. Both return nullptr on success or a static error literal for setStatus();
/// host-tested by unit_RmtLedDriver_pins.cpp.

// The per-pin light ceiling for a WS2812-class 1-wire protocol (RMT / LCD_CAM / Parlio
// all shift the same NRZ wire timing): one line clocks a fixed ~30 us/LED (24 bits ×
// 1.25 us), so 2048 LEDs ≈ 61 ms ≈ 16 FPS — the floor before output turns to a slideshow.
// The WS2812 drivers pass this as assignCounts' maxPerPin. Clocked 2-wire types
// (APA102/SK9822 over SPI) have no such fixed-rate limit — they pass a far higher cap (or 0).
inline constexpr nrOfLightsType kMaxWs2812LedsPerPin = 2048;

// Fill counts[0..nPins) from the `ledsPerPin` text. Three cases — the broadcasting
// idiom (cf. NumPy/CSS: a scalar applies to all, a list maps element-wise), with an
// "auto-fit" empty case:
//   • empty ""      → even split: totalLights / nPins per pin (last takes the remainder).
//   • single "N"    → N per pin, on EVERY pin (broadcast). "one number = that many each."
//   • list "3,4,5"  → map per pin: pin0=3, pin1=4, pin2=5; a list SHORTER than nPins
//                     even-splits the remaining lights over the unlisted pins.
// A list LONGER than nPins ignores the extras (a stale list after pins shrank is not an
// error). Explicit counts are clamped so the running sum never exceeds totalLights.
//
// `maxPerPin` is the driver's protocol ceiling on lights-per-data-line: a pin whose count
// exceeds it is clamped to it, so the driver drives the first maxPerPin (output stays lit)
// instead of choking on the rest. Per-protocol, so the driver passes its own — a WS2812-class
// 1-wire line (RMT / LCD_CAM / Parlio) clocks a fixed ~30 us/LED, so ~2048/pin is already
// ~16 FPS (kMaxWs2812LedsPerPin); a clocked 2-wire SPI type (APA102/SK9822) runs tens of MHz
// and does 10k+/pin, so it passes a far higher cap. Pass 0 for no ceiling. The intended way
// to output fewer lights is the driver's start/count window, not this safety cap.
//
// On a clamp, `*warn` (if given) is set to kClampedWarning — a Warning the caller shows while
// still running, distinct from the return value (which stays nullptr: clamping is not an
// error that idles the driver). Left null when nothing was clamped.
//
// Offsets accumulate from the CLAMPED counts, so clamping pin i also shifts every later pin's
// source slice down by the trimmed amount. For the headline case — a whole grid funneled onto
// ONE pin — this is exactly right (that one pin drives the first maxPerPin, nothing after it).
// For the pathological multi-pin-all-over-ceiling case (e.g. 3000 on each of 3 pins) the later
// strips then show a shifted window, not just a truncated one. Accepted, not fixed: it degrades
// rather than crashes (architecture.md Robustness), the misconfig is not one anyone wires on
// purpose, and preserving alignment would need a parallel unclamped-counts array — more state
// for a case the warning already flags. The window (start/count) is the real way to send fewer.
inline constexpr const char* kClampedWarning =
    "some LEDs not driven: over per-pin max; add pins or use start/count";

inline const char* assignCounts(const char* s, uint8_t nPins,
                                nrOfLightsType totalLights, nrOfLightsType* counts,
                                nrOfLightsType maxPerPin = 0, const char** warn = nullptr) {
    if (warn) *warn = nullptr;
    for (uint8_t i = 0; i < nPins; i++) counts[i] = 0;

    // Single value (no comma) → broadcast: that many on EVERY pin, clamped so no
    // pin exceeds the buffer's remaining lights. "one number = that many each."
    {
        const char* p = s;
        while (*p == ' ') p++;
        if (*p) {
            char* end = nullptr;
            const long v = std::strtol(p, &end, 10);
            if (end == p || v < 0) return "invalid count list";
            while (*end == ' ') end++;
            if (*end == '\0') {   // exactly one number → broadcast it
                nrOfLightsType remaining = totalLights;
                for (uint8_t i = 0; i < nPins; i++) {
                    const nrOfLightsType c =
                        (v > static_cast<long>(remaining)) ? remaining
                                                           : static_cast<nrOfLightsType>(v);
                    counts[i] = c;
                    remaining = static_cast<nrOfLightsType>(remaining - c);
                }
                if (maxPerPin > 0)
                    for (uint8_t i = 0; i < nPins; i++)
                        if (counts[i] > maxPerPin) { counts[i] = maxPerPin; if (warn) *warn = kClampedWarning; }
                return nullptr;
            }
        }
    }

    // Empty → even split; a list → map per pin (a short list even-splits the rest).
    nrOfLightsType remaining = totalLights;
    uint8_t nExplicit = 0;
    const char* p = s;
    while (p && *p && nExplicit < nPins) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0) return "invalid count list";
        while (*end == ' ') end++;
        const nrOfLightsType c =
            (v > static_cast<long>(remaining)) ? remaining
                                               : static_cast<nrOfLightsType>(v);
        counts[nExplicit++] = c;
        remaining = static_cast<nrOfLightsType>(remaining - c);
        if (*end == '\0') break;
        if (*end != ',') return "invalid count list";
        p = end + 1;
    }
    const uint8_t nRemaining = static_cast<uint8_t>(nPins - nExplicit);
    if (nRemaining > 0) {
        const nrOfLightsType per = static_cast<nrOfLightsType>(remaining / nRemaining);
        for (uint8_t i = nExplicit; i < nPins; i++) counts[i] = per;
        counts[nPins - 1] = static_cast<nrOfLightsType>(
            counts[nPins - 1] + (remaining - per * nRemaining));
    }
    if (maxPerPin > 0)
        for (uint8_t i = 0; i < nPins; i++)
            if (counts[i] > maxPerPin) {
                counts[i] = maxPerPin;
                if (warn) *warn = kClampedWarning;
            }
    return nullptr;
}

/// @}

} // namespace mm
