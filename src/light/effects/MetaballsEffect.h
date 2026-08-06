#pragma once

#include "core/math16.h"            // BeatPhase — the shared BPM accumulator
#include "light/effects/EffectBase.h"

namespace mm {

// Author: projectMM original (metaballs)
/// Metaballs effect: smooth merging blobs via a scalar field.
/// @card MetaballsEffect.png
class MetaballsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    uint8_t bpm = 30;
    uint8_t radius = 28;
    uint8_t count = 4;   // number of balls (1..MAX_BALLS); each follows its own sine path
    uint8_t hue_shift = 0;

    static constexpr uint8_t MAX_BALLS = 8;

    void defineControls() override {
        controls_.addUint8("bpm", bpm, 1, 255);
        controls_.addUint8("radius", radius, 4, 255);
        controls_.addUint8("count", count, 1, MAX_BALLS);
        controls_.addUint8("hue_shift", hue_shift, 0, 255);
    }

    // Class scope, not function-local: -Wfunction-effects flags ANY static local in a
    // nonblocking function, including a constexpr that needs no guard variable. Same
    // storage and value here, and these are per-effect constants anyway.
    static constexpr uint8_t SPEED_MUL[MAX_BALLS]  = { 1, 2, 3, 1, 2, 3, 1, 2 };
    static constexpr uint8_t PHASE_X[MAX_BALLS]    = { 0, 30, 60, 120, 160, 200, 90, 220 };
    static constexpr uint8_t PHASE_Y[MAX_BALLS]    = { 64, 94, 124, 184, 16, 210, 150, 40 };

    void tick() MM_NONBLOCKING override {
        uint8_t* buf = buffer();
        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint32_t now = elapsed();
        // Shared accumulator: raw dt·bpm in 64 bits, divided only at the read, so a sub-millisecond
        // frame does not round to zero and freeze the animation (mm::BeatPhase owns that rule now).
        phase_.advance(now, bpm);
        const uint8_t t = static_cast<uint8_t>(phase_.phase(256));

        const uint8_t n = count < MAX_BALLS ? count : MAX_BALLS;
        int16_t bx[MAX_BALLS];
        int16_t by[MAX_BALLS];
        for (uint8_t b = 0; b < n; b++) {
            uint8_t tb = static_cast<uint8_t>(t * SPEED_MUL[b]);
            bx[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_X[b])) * w) >> 8);
            by[b] = static_cast<int16_t>((sin8(static_cast<uint8_t>(tb + PHASE_Y[b])) * h) >> 8);
        }

        // Field strength: sum of r^2 / (d^2 + 1)
        int32_t r2 = static_cast<int32_t>(radius) * radius;

        for (lengthType y = 0; y < h; y++) {
            uint8_t* row = buf + static_cast<size_t>(y) * w * cpl;
            for (lengthType x = 0; x < w; x++) {
                uint32_t field = 0;
                for (uint8_t b = 0; b < n; b++) {
                    int32_t dx = static_cast<int32_t>(x) - bx[b];
                    int32_t dy = static_cast<int32_t>(y) - by[b];
                    int32_t d2 = dx * dx + dy * dy + 1;
                    field += static_cast<uint32_t>((r2 * 64) / d2);
                }
                uint8_t bright = field > 255 ? 255 : static_cast<uint8_t>(field);
                uint8_t hue = static_cast<uint8_t>((field >> 1) + hue_shift);
                RGB c = colorFromPalette(*Palettes::active(), hue, bright);

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
