#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: projectMM original (rotating spiral)
/// Effect winding a lit spiral up a conical layout.
/// @card SpiralEffect.png
class SpiralEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 40;
    uint8_t twist = 4;
    uint8_t hue_shift = 0;

    void defineControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("twist", twist, 1, 255);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();

        uint32_t now = elapsed();
        // Shared accumulator: raw dt·rate in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advance(now, bpm);
        // Accumulate the raw (dt * bpm) product; divide only at the read site.
        // Per-tick `dt*bpm*256/60000` rounds to 0 on desktop (dt ≈ 0..1ms) and
        // freezes the animation; see MetaballsEffect for the same fix.
        uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        int16_t cx = static_cast<int16_t>(w >> 1);
        int16_t cy = static_cast<int16_t>(h >> 1);

        for (lengthType y = 0; y < h; y++) {
            int16_t dy = static_cast<int16_t>(y) - cy;
            uint8_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w) * cpl;
            for (lengthType x = 0; x < w; x++) {
                int16_t dx = static_cast<int16_t>(x) - cx;
                // 16-bit polar: atan16 resolves the sweep smoothly where the 8-bit form stepped, and
                // dist16 is a TRUE radius where dist8 approximated an octagon (visible corners on a
                // large panel). Both are taken down to 8 bits here because hue is mod-256 by design.
                const uint8_t angle = static_cast<uint8_t>(atan16(dy, dx) >> 8);
                const uint8_t dist = static_cast<uint8_t>(dist16(dx, dy));
                uint8_t hue = static_cast<uint8_t>(
                    angle + static_cast<uint8_t>(dist * twist) - t + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue);

                if (cpl >= 1) row[0] = c.r;
                if (cpl >= 2) row[1] = c.g;
                if (cpl >= 3) row[2] = c.b;
                row += cpl;
            }
        }
    }

private:
    // Numerator-only accumulator (units of dt*bpm). See tick() for why.
    BeatPhase phase_;
};

} // namespace mm
