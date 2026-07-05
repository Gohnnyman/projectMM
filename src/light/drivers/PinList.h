#pragma once

#include "light/light_types.h"  // nrOfLightsType

#include <cstdint>
#include <cstdlib>  // std::strtol

namespace mm {

// Shared pin/count list parsing for multi-output LED drivers — RmtLedDriver
// (one RMT channel per pin) and LcdLedDriver (one LCD_CAM lane per pin) both
// drive consecutive slices of the source buffer from the same two text
// controls, so the parsers live once here (extracted when the second user
// landed, per the increment-1 plan's "concrete first" rule). Both return
// nullptr on success or a static error literal the caller feeds straight into
// setStatus(); both are strtol-based like parseDottedQuad (Control.h) and
// fully host-testable (unit_RmtLedDriver_pins.cpp pins them for both drivers).

// Parse "18,17,16" into out[0..maxPins). Spaces around tokens are fine
// (strtol skips them). Rejects empty input, bad tokens, trailing commas,
// duplicates, and more than maxPins entries (the chip's channel/lane cap).
inline const char* parsePinList(const char* s, uint16_t* out, uint8_t maxPins,
                                uint8_t& nOut) {
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

// The per-pin light ceiling for a WS2812-class 1-wire protocol (RMT / LCD_CAM / Parlio
// all shift the same NRZ wire timing): one line clocks a fixed ~30 us/LED (24 bits ×
// 1.25 us), so 2048 LEDs ≈ 61 ms ≈ 16 FPS — the floor before output turns to a slideshow.
// The WS2812 drivers pass this as assignCounts' maxPerPin. Clocked 2-wire types
// (APA102/SK9822 over SPI) have no such fixed-rate limit — they pass a far higher cap (or 0).
inline constexpr nrOfLightsType kMaxWs2812LedsPerPin = 2048;

// Fill counts[0..nPins) from "100,100,50" (may be empty or shorter than the
// pin list; extra entries beyond nPins are ignored — a stale longer list
// after pins shrank is not an error). Explicit counts are clamped so the
// running sum never exceeds totalLights; the unassigned remainder splits
// evenly over the unlisted pins, last pin takes the rounding remainder.
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
    nrOfLightsType remaining = totalLights;
    uint8_t nExplicit = 0;
    const char* p = s;
    while (p && *p && nExplicit < nPins) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0) return "invalid ledsPerPin list";
        while (*end == ' ') end++;
        const nrOfLightsType c =
            (v > static_cast<long>(remaining)) ? remaining
                                               : static_cast<nrOfLightsType>(v);
        counts[nExplicit++] = c;
        remaining = static_cast<nrOfLightsType>(remaining - c);
        if (*end == '\0') break;
        if (*end != ',') return "invalid ledsPerPin list";
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

} // namespace mm
