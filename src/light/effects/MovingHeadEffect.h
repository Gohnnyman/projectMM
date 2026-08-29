#pragma once
// Moving head: sweeps a fixture's pan and tilt along two sine waves, so the beam traces a slow
// figure the eye can follow (a Lissajous path: two sines at different rates, which closes into a
// loop when their periods share a ratio and drifts when they do not).
//
// This is the first effect that aims a fixture rather than only coloring it. It writes pan and
// tilt through EffectBase's role setters, which are no-ops on a light that carries no such
// channel, so the same effect on an LED strip simply paints the color sweep and moves nothing.
// That is the orthogonality the pipeline is built on: an effect describes what it wants, and the
// fixture's own preset decides what can be expressed.
//
// The color is deliberately simple (one palette walk across the fixtures): the subject here is
// motion, and a busy color pattern on a single moving head reads as noise.
// Author: projectMM original

#include "core/math16.h"      // sin16, BeatPhase: the sweep clocks
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: sweeps a moving head's pan and tilt along sine waves.
/// @card MovingHeadEffect.gif
class MovingHeadEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬"; }
    Dim dimensions() const override { return Dim::D1; }

    /// Sweep rates in BPM, the project's speed unit: 60 = one full sweep a second. Pan and tilt
    /// run at different rates on purpose, which is what turns two sines into a path rather than a
    /// diagonal line.
    uint8_t panBpm  = 6;
    uint8_t tiltBpm = 9;
    /// How much of the fixture's travel to use, as a fraction of full range. A head at full pan
    /// travel spends much of its sweep pointing away from the audience, so the useful default is
    /// a band around center rather than the whole 540 degrees.
    uint8_t panRange  = 128;
    uint8_t tiltRange = 96;
    /// Where the sweep is centered (128 = the fixture's middle position).
    uint8_t panCenter  = 128;
    uint8_t tiltCenter = 128;

    void defineControls() override {
        controls_.addControl("panBpm", panBpm, 1, 120);
        controls_.addControl("tiltBpm", tiltBpm, 1, 120);
        controls_.addControl("panRange", panRange, 0, 255);
        controls_.addControl("tiltRange", tiltRange, 0, 255);
        controls_.addControl("panCenter", panCenter, 0, 255);
        controls_.addControl("tiltCenter", tiltCenter, 0, 255);
    }

    void prepare() override {
        pan_ = BeatPhase{};
        tilt_ = BeatPhase{};
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const nrOfLightsType n = nrOfLights();
        if (n == 0) return;

        // Own the background: an effect must not inherit the previous frame's picture. The color
        // pass below writes one light per fixture, so the rest of a grid would otherwise keep
        // whatever the last effect left there.
        draw::fill(cv, RGB{0, 0, 0});

        // One period per sweep, advanced by elapsed time so the motion is frame-rate independent
        // (the same rule every animated effect follows).
        pan_.advance(elapsed(), panBpm);
        tilt_.advance(elapsed(), tiltBpm);

        // phase(65536) is the angle16 form: sin16 takes a full turn as 0..65535, and the
        // truncation to uint16 is where the free wrap happens.
        const uint16_t panPhase  = static_cast<uint16_t>(pan_.phase(65536));
        const uint16_t tiltPhase = static_cast<uint16_t>(tilt_.phase(65536));

        for (nrOfLightsType i = 0; i < n; i++) {
            // Fixtures on a chain are spread along the wave rather than moving in unison, so a row
            // of heads reads as a travelling sweep. A single fixture is unaffected (offset 0).
            const uint16_t spread = n > 1 ? static_cast<uint16_t>((i * 65536u) / n) : 0;
            const int32_t p = sin16(static_cast<uint16_t>(panPhase + spread));
            const int32_t t = sin16(static_cast<uint16_t>(tiltPhase + spread));

            setPan(i, axis(p, panCenter, panRange));
            setTilt(i, axis(t, tiltCenter, tiltRange));

            // Color: one walk through the palette across the fixtures, moving with the pan sweep so
            // color and motion read as one gesture.
            const uint8_t hue = static_cast<uint8_t>((panPhase >> 8) + i * (256u / (n > 0 ? n : 1)));
            draw::splat(cv, draw::toSub(static_cast<lengthType>(i)), 0,
                        colorFromPalette(*Palettes::active(), hue));
        }
    }

private:
    /// Map a sine (-32768..32767) onto a DMX byte around `center`, using `range` of the travel.
    /// Integer-only and clamped: a center near an end plus a wide range must not wrap the axis
    /// around, which on a real fixture is a full-speed swing to the opposite stop.
    static uint8_t axis(int32_t wave, uint8_t center, uint8_t range) {
        const int32_t swing = (wave * range) / 65536;    // +-range/2 around center
        int32_t v = static_cast<int32_t>(center) + swing;
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        return static_cast<uint8_t>(v);
    }

    BeatPhase pan_;
    BeatPhase tilt_;
};

} // namespace mm
