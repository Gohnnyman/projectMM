#pragma once

#include "core/math16.h"              // peakHold, smoothFollow, map32
#include "light/effects/EffectBase.h"

namespace mm {

// Spectrum: an audio analyser with real meter ballistics.
//
// Every VU meter ever built has the same two behaviours, and they are the reason a meter reads as
// an instrument rather than a bar chart:
//
//   - **Asymmetric response.** The bar rises fast enough to catch a transient and falls slowly
//     enough to be readable. A symmetric follower either misses the hit or flickers.
//   - **A held peak.** A dot marks the highest recent level and drifts down, so a glance shows both
//     what is happening now and what just happened.
//
// Both live in the toolbox as `smoothFollow` and `peakHold` rather than in this effect, which is
// what separates it from the existing GEQ: that one hand-rolls its peak tracker into a heap buffer
// sized to the column count. Here the ballistics are two named function calls, and any effect that
// wants a meter gets the same feel.
//
// `bar` draws the columns, so the effect owns no drawing loop either. What remains is the mapping
// from bands to columns and the choice of colors — which is genuinely this effect's own.
//
// Cost: trivial. One follower and one peak per column, and the bars are direct writes.
//
// Prior art: the standard VU/PPM meter ballistics (BBC/EBU peak-programme-meter behaviour) and
// WLED's GEQ family for the band-to-column mapping.
// @card SpectrumEffect.png
/// Audio effect: a spectrum analyser with asymmetric ballistics and falling peak dots.
class SpectrumEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬📊"; }  // showcase + audio-reactive
    Dim dimensions() const override { return Dim::D2; }   // writes the z=0 slice; extrude fills z

    uint8_t attack   = 200;  // how fast a bar rises toward a new level (255 = instant)
    uint8_t release  = 30;   // how fast it falls back
    uint8_t peakDecay = 2;   // how fast the peak dot drifts down
    bool    showPeaks = true;
    bool    colorByColumn = false;  // color per band instead of by height

    void defineControls() override {
        controls_.addUint8("attack", attack, 1, 255);
        controls_.addUint8("release", release, 1, 128);
        controls_.addUint8("peakDecay", peakDecay, 0, 32);
        controls_.addBool("showPeaks", showPeaks);
        controls_.addBool("colorByColumn", colorByColumn);
    }

    void prepare() override {
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        levels_.resize(cols);
        peaks_.resize(cols);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (!levels_ || !peaks_) return;   // this effect's own buffers; the grid is the Layer's

        draw::fill(cv, RGB{0, 0, 0});

        const AudioFrame* f = AudioService::latestFrame();
        if (!f) return;   // no audio: a dark panel, not a crash

        for (lengthType x = 0; x < w; x++) {
            // Map the column onto one of the 16 bands, so the analyser fits any width.
            const uint8_t band = static_cast<uint8_t>(
                static_cast<uint32_t>(x) * kBands / static_cast<uint32_t>(w > 0 ? w : 1));
            const uint8_t mag = f->bands[band];

            // The ballistics: rise with `attack`, fall with `release`. Asymmetry is the whole point,
            // so the two directions use different rates rather than one shared smoothing.
            const uint8_t rate = mag > levels_[x] ? attack : release;
            levels_[x] = smoothFollow(levels_[x], mag, rate);

            // The peak dot rises instantly and drifts down — the other half of a real meter.
            peaks_[x] = peakHold(peaks_[x], levels_[x], peakDecay);

            const lengthType lit = static_cast<lengthType>(
                (static_cast<uint32_t>(levels_[x]) * h) / 255u);
            const uint8_t columnHue = static_cast<uint8_t>(
                map32(x, 0, w > 1 ? w - 1 : 1, 0, 255));

            // One bar per column, growing up from the floor. The color rides the height unless the
            // caller asked for per-column hues.
            draw::bar(cv, x, static_cast<lengthType>(h - 1), lit, draw::Grow::Up,
                      [&](lengthType row) {
                          const uint8_t idx = colorByColumn
                              ? columnHue
                              : static_cast<uint8_t>((static_cast<uint32_t>(row) * 255u) / (h > 1 ? h - 1 : 1));
                          return colorFromPalette(*Palettes::active(), idx, 255);
                      });

            // The floating peak, drawn one row above where the bar currently reaches.
            if (showPeaks && peaks_[x] > 0) {
                const lengthType py = static_cast<lengthType>(
                    h - 1 - (static_cast<uint32_t>(peaks_[x]) * h) / 255u);
                if (py >= 0 && py < h)
                    draw::pixel(cv, {x, py, 0}, colorFromPalette(*Palettes::active(), 255, 255));
            }
        }
    }

private:
    static constexpr uint32_t kBands = 16;

    ScratchBuffer<uint8_t> levels_{*this};   // the smoothed bar height per column
    ScratchBuffer<uint8_t> peaks_{*this};    // and the held peak above it
};

}  // namespace mm
