#pragma once

// Where a fixture's non-color channels live inside one light.
//
// A light is `channelsPerLight` bytes wide, and that width is a runtime value: 3 for RGB, 4 for
// RGBW, 11 for a mini moving head, 25 for a bigger one. The pipeline does not care which
// (architecture.md § Drivers): a wider light simply has more bytes, and a driver that cannot use
// them ignores them. This header says which of those bytes are pan, tilt, zoom and so on, so an
// effect can drive a moving head through the SAME buffer it paints pixels into.
//
// Color has no entry here because it is always at offset 0: every effect and every draw primitive
// writes RGB(W) at the start of a light, and `Correction` places those bytes at the fixture's own
// color channels on the way out. The offsets below are the channels an effect could not otherwise
// reach at all.
//
// `kAbsent` means "this fixture has no such channel", and every setter is a no-op then. That is
// what keeps a moving-head effect harmless on an LED strip: it calls setPan(), the strip carries
// no pan channel, nothing is written. The same effect on a moving head steers it. (MoonLight's
// LightsHeader is the same idea, credited in docs/history/.)
//
// These are offsets into the LAYER's light, which is not the same layout as the fixture's. A
// layer light always begins with RGB(W) (every draw primitive writes there), so motion is packed
// AFTER the color, in a fixed order. The fixture's own offsets live in Correction, and apply()
// maps layer slot to fixture channel on the way out. Keeping the two apart is what stopped pan
// from landing on the red channel: at one point both claimed byte 0.

#include "light/light_types.h"

#include <cstdint>

namespace mm {

struct FixtureChannels {
    static constexpr uint8_t kAbsent = 255;   // the fixture has no channel for this role
    /// Where motion starts in a layer light: after RGBW. A layer carrying motion is at least this
    /// wide, and effects paint color into the bytes below it exactly as they do on a strip.
    static constexpr uint8_t kMotionBase = 4;

    uint8_t pan    = kAbsent;
    uint8_t tilt   = kAbsent;
    uint8_t zoom   = kAbsent;
    uint8_t rotate = kAbsent;
    uint8_t gobo   = kAbsent;

    /// True when this fixture can be aimed: an effect can ask before doing motion maths it would
    /// otherwise throw away on a strip.
    bool movable() const { return pan != kAbsent || tilt != kAbsent; }

    /// Walk the motion roles in their PACKING ORDER, calling `f(roleIndex, slot)` for each role
    /// the fixture carries. `present` says which of the five it has, in the same order.
    ///
    /// The one home for that order. Both sides need it and they must agree byte for byte or a
    /// pan value lands on a tilt channel: Drivers walks it to assign the layer slots, and
    /// Correction::apply walks it to read them back. Written twice, they would drift.
    template <typename Fn>
    static void forEachMotionSlot(const bool present[5], Fn&& f) {
        uint8_t slot = kMotionBase;
        for (uint8_t role = 0; role < 5; role++)
            if (present[role]) f(role, slot++);
    }
};

} // namespace mm
