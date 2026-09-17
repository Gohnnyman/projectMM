#pragma once

#include "core/util/math16.h"              // hashInt, easeInOutQuad, BeatPhase
#include "light/effects/EffectBase.h"

namespace mm {

/// Effect: two color fields trading places pixel by pixel, with no per-pixel state.
/// @card DissolveEffect.gif
///
/// The order looks random but is computed rather than stored.
/// Each pixel asks a hash of its own position for a threshold, and compares it against the progress.
///
/// Prior art: the classic dissolve, where the position-addressed form is the shader approach.
///
/// @moreinfo
///
/// ## The point is what it does not need
///
/// A dissolve normally shuffles a list of pixel indices, which costs memory and must stay in step.
/// Here there is no array, no shuffle and no per-pixel state.
/// Two devices agree without exchanging anything, since the hash is a function of position.
///
/// That is the supersync property: a stream generator advances per call and drifts, and this cannot.
///
/// Cost is one hash and one compare a pixel, with nothing scaling beyond the pixel count.
class DissolveEffect : public EffectBase {
public:
    /// Catalog tags: this effect is the power-function showcase.
    const char* tags() const override { return "💫"; }
    /// Writes the z=0 slice, which extrude fills through a volume.
    Dim dimensions() const override { return Dim::D2; }

    /// How fast one transition completes.
    uint8_t bpm      = 12;
    /// How long a pixel spends mid-flight, where 0 gives a hard edge.
    uint8_t spread   = 60;
    /// Ease the progress rather than sweeping it linearly, which reads as mechanical.
    bool    eased    = true;
    /// Dissolve in a scattered order, or wipe positionally when off.
    bool    scatter  = true;

    /// Publish the transition's speed, its softness and its order.
    void defineControls() override {
        controls_.addControl("bpm", bpm, 1, 120);
        controls_.addControl("spread", spread, 0, 255);
        controls_.addControl("eased", eased);
        controls_.addControl("scatter", scatter);
    }

    /// Compare each pixel's own threshold against the transition's progress.
    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();

        phase_.advanceTo(elapsed(), bpm);
        const uint32_t raw = phase_.phase(65536);

        // Each sweep is a generation, which seeds the hash so no two passes share an order.
        const uint32_t generation = raw >> 16;
        uint16_t progress = static_cast<uint16_t>(raw & 0xFFFF);
        if (eased) progress = easeInOutQuad(progress);

        // The fields are palette reads a generation apart, so the effect walks the palette.
        const uint8_t hueA = static_cast<uint8_t>(generation * kHueStep);

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                // With scatter off the threshold comes from position, which wipes diagonally.
                const uint16_t threshold = scatter
                    ? hashInt(static_cast<uint32_t>(x), static_cast<uint32_t>(y), generation)
                    : static_cast<uint16_t>(((x + y) * 65535u) / (w + h > 0 ? w + h : 1));

                // `spread` widens the comparison into a ramp, so a pixel crosses over gradually.
                uint8_t mix;
                if (spread == 0) {
                    mix = progress > threshold ? 255 : 0;
                } else {
                    const int32_t band = static_cast<int32_t>(spread) * 128;
                    const int32_t d = static_cast<int32_t>(progress) - threshold;
                    mix = d <= -band ? 0 : (d >= band ? 255
                        : static_cast<uint8_t>(((d + band) * 255) / (2 * band)));
                }

                // The known delta, not the byte difference: across the wrap that runs backwards.
                const uint8_t idx = static_cast<uint8_t>(hueA + ((kHueStep * mix) >> 8));
                // Brightness dips mid-flight, so the transition shimmers rather than crossfading.
                const uint8_t bri = static_cast<uint8_t>(255 - (mix > 128 ? 255 - mix : mix) / 2);
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), idx, bri));
            }
        }
    }

private:
    /// How far the palette advances each generation.
    static constexpr int32_t kHueStep = 40;

    BeatPhase phase_;   ///< the transition clock
};

}  // namespace mm
