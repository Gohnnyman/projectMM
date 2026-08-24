#pragma once

#include "light/effects/EffectBase.h"
#include "light/particles.h"   // particles::FrameTime, the shared elapsed-to-scale conversion

namespace mm {

// Random: each frame the whole buffer is dimmed a little, then exactly ONE randomly chosen
// light is lit to a random palette color. Over many frames this scatters fading sparkles of
// color across the whole volume — a slow, twinkling field whose density is set by the fade
// amount (less fade = pixels linger and the field fills; more fade = sparse, quick-decaying
// specks).
//
// Prior art: MoonLight's Random effect (E_MoonModules / MoonModules). The behaviour is
// reproduced exactly — one fadeToBlackBy(fade) plus one setRGB(random index, palette[random])
// per frame — written fresh on EffectBase + the shared draw/Palette primitives. The light is
// chosen by a flat light index across all nrOfLights (the engine's native ordering, the direct
// equivalent of MoonLight's index-based setRGB), so it can land anywhere in a 1D/2D/3D layer.
// Author: MoonLight — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Effect that fills the layer with animated random colors.
class RandomEffect : public EffectBase {
public:
    const char* tags() const override { return "💫"; }  // MoonLight origin
    // D3: the single lit light is picked by flat index over the entire volume, so this effect
    // writes into any z slice — it iterates (addresses) every axis the layer has.
    Dim dimensions() const override { return Dim::D3; }

    uint8_t fade = 70;  // per-frame fadeToBlackBy amount (0..255)

    void defineControls() override {
        controls_.addControl("fade", fade, 0, 255);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const nrOfLightsType n = nrOfLights();
        const uint8_t cpl = cv.cpl;

        // Dim the whole buffer (source: layer->fadeToBlackBy(fade)).
        layer()->fadeToBlackBy(fade);

        // Light one random light per REFERENCE FRAME, not per render. The source lights one per
        // frame, which makes the sparkle rate a property of the hardware: the same effect is a
        // gentle twinkle at 60 fps and a solid wash at 1200. Carrying the fraction spends whole
        // lights as time earns them, so the rate is the same on any device and a faster one simply
        // places them more evenly. See architecture.md, the tick-rate rule.
        spawnCarry_ += time_.advance(elapsed());
        uint32_t due = spawnCarry_ / particles::FrameTime::kOne;
        if (due > 64) due = 64;                     // a long stall tops up, it does not fill the grid
        spawnCarry_ -= due * particles::FrameTime::kOne;

        uint8_t* d = cv.data;
        for (uint32_t k = 0; k < due; k++) {
            // The index is a flat light index (the engine's native light ordering) so the write
            // goes straight into the buffer at that light, the direct equivalent of MoonLight's
            // index-based setRGB. (There is no flat-index draw primitive; draw::pixel takes a
            // coordinate, hence the byte write here.)
            const nrOfLightsType idx = static_cast<nrOfLightsType>(rng_.next16() % n);
            const RGB c = colorFromPalette(*Palettes::active(), rng_.next8());
            const size_t off = static_cast<size_t>(idx) * cpl;
            if (off + (cpl < 3 ? cpl : 3) > cv.bytes) continue;
            d[off + 0] = c.r;
            if (cpl >= 2) d[off + 1] = c.g;
            if (cpl >= 3) d[off + 2] = c.b;
        }
    }

private:
    particles::FrameTime time_{60};   // spawn rate is per second, not per frame
    uint32_t spawnCarry_ = 0;         // sub-frame lights not yet placed
    Random8 rng_;  // per-effect PRNG (deterministic, independent sequence)
};

} // namespace mm