#pragma once

#include "core/math16.h"              // atan16, dist16, kaleido, BeatPhase
#include "core/noise.h"               // fbm8, warp8
#include "light/effects/EffectBase.h"

namespace mm {

// PolarNoise: a warped noise field addressed in polar coordinates, folded into a kaleidoscope.
//
// The look this reaches for is the one Stefan Petrick made recognisable in the LED world: not a
// texture scrolling past the panel, but a field that seems to turn and breathe inside it. Three
// power functions compose to get there, and the effect itself is mostly parameter choices:
//
//   - `atan16`/`dist16` address the grid by ANGLE and RADIUS instead of x and y, which is what
//     makes the motion rotate around the centre rather than slide across it.
//   - `warp8` displaces the sample coordinate by another noise field, so the field flows and
//     marbles instead of merely drifting (Quilez's domain warping).
//   - `kaleido` folds the angle into n mirrored wedges, turning the field into a mandala with a
//     single modulo — the symmetry is free because it happens before the field is ever sampled.
//
// Cost: `warp` is 2 noise samples plus its inner fbm, so at octaves=2 this is ~4 samples/pixel.
// That is a rich-field effect, appropriate on small and medium fixtures and on desktop; on a large
// wall drop `octaves` to 1 (or `warp` to 0) and it degrades to a plain polar noise that still
// reads well. The controls are deliberately the cost knobs, not just the look knobs.
//
// Prior art: Stefan Petrick's polar/noise effect vocabulary (a friend of projectMM) and Iñigo
// Quilez's domain-warping article. Implemented fresh in fixed point over our own noise.
// @card PolarNoiseEffect.png
/// Effect: a warped, kaleidoscopic noise field in polar coordinates.
class PolarNoiseEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🖌️"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm      = 8;    // how fast the field drifts
    uint8_t scale    = 40;   // noise cells across the grid: low = broad shapes, high = fine detail
    uint8_t segments = 6;    // kaleidoscope wedges; 1 disables the fold
    uint8_t warp     = 90;   // domain-warp strength; 0 is a plain (unwarped) field
    uint8_t octaves  = 2;    // fbm octaves — the main cost knob
    uint8_t twist    = 30;   // how much the radius shears the angle, giving the field a spiral set

    void defineControls() override {
        controls_.addControl("bpm", bpm, 0, 60);
        controls_.addControl("scale", scale, 1, 255);
        controls_.addControl("segments", segments, 1, 16);
        controls_.addControl("warp", warp, 0, 255);
        controls_.addControl("octaves", octaves, 1, 4);
        controls_.addControl("twist", twist, 0, 255);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advance(elapsed(), bpm);
        const uint32_t t = phase_.phase(65536);

        // Centre in whole pixels; the field is smooth so sub-pixel centring buys nothing here.
        const int32_t cx = w / 2, cy = h / 2;

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const int32_t dx = static_cast<int32_t>(x) - cx;
                const int32_t dy = static_cast<int32_t>(y) - cy;

                // Polar address. The twist term shears the angle by the radius, which is what turns
                // concentric rings into spiral arms.
                const uint32_t r = dist16(dx, dy);
                angle16 a = atan16(dy, dx);
                a = static_cast<angle16>(a + (r * twist));

                // Fold into wedges BEFORE sampling, so the symmetry costs one modulo rather than a
                // second pass over the field.
                a = kaleido(a, segments);

                // Sample the field in (angle, radius) space: the angle drives one axis and the
                // radius the other, so the field wraps around the centre. Time moves the radius
                // axis, which reads as the pattern breathing outward.
                const uint32_t fx = (static_cast<uint32_t>(a) >> 6) * scale / 16u;
                const uint32_t fy = (r * scale) + (t >> 6);

                const uint8_t v = warp > 0 ? warp8(fx, fy, static_cast<uint16_t>(warp) * 4, octaves)
                                           : fbm8(fx, fy, octaves);

                const RGB c = colorFromPalette(*Palettes::active(), v, 255);
                draw::pixel(cv, {x, y, 0}, c);
            }
        }
    }

private:
    BeatPhase phase_;
};

}  // namespace mm
