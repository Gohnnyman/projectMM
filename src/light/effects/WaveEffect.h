#pragma once

#include "light/effects/EffectBase.h"

namespace mm {

// A waveform that travels across the grid: each column x plots one point of a moving wave, so
// the lit points trace a sawtooth / triangle / sine / square / composite-sine / noise curve that
// scrolls sideways over time, leaving a fading trail behind it. The classic "oscilloscope wave"
// look. Prior art: MoonLight's Wave effect (Ewoud Wijma) — behaviour reproduced (the six waveform
// types, the per-column phase travel, the time-varying color, the frame fade), written fresh on
// projectMM's EffectBase + integer primitives (sin8 LUT, scale8); the color is a global-palette lookup.
//
// Axis convention: the waveform sets a y (its shape lives on HEIGHT); width is the travel axis.
// So a 1-tall grid shows no wave — to drive a 1D output (a strip, a row of Hue lights) lay it out
// as 1×N (the project's 1D-runs-along-Y convention; architecture.md § Dimensionality).
//
// Per column x the phase is `t + x·kColumnSkew` — the x term delays each column so the wave
// appears to move horizontally; `t` advances with bpm so it animates. The phase maps through the
// selected waveform to a y in [0, height); that pixel is lit, and for the discontinuous shapes
// (sawtooth, square) a vertical segment connects to the previous column so the line stays joined.
// Author: Ewoud Wijma (MoonLight) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Effect of a travelling wave across the layer.
class WaveEffect : public EffectBase {
public:
    const char* tags() const override { return "🌊"; }
    // D2 — writes the z=0 plane only; Layer::extrude duplicates it across z on a 3D layout.
    Dim dimensions() const override { return Dim::D2; }

    // Waveform shapes, index-aligned with waveY()'s switch. "Sin3" is a composite of three sines
    // (a richer, rolling wave); "Noise" plots value noise for a jittered band.
    static constexpr const char* kTypeOptions[] = {"Sawtooth", "Triangle", "Sine", "Square", "Sin3", "Noise"};
    static constexpr uint8_t kTypeCount = 6;

    uint8_t bpm  = 30;   // travel speed (phase advance per minute)
    uint8_t fade = 32;   // trail fade per frame (0 = instant clear of the old wave, 255 = long tail)
    uint8_t type = 2;    // index into kTypeOptions (default Sine)

    void defineControls() override {
        controls_.addUint8("bpm", bpm, 0, 255);
        controls_.addUint8("fade", fade, 0, 255);
        controls_.addSelect("type", type, kTypeOptions, kTypeCount);
    }

    // The trail is a persistent z=0-plane buffer (w·h·cpl): each frame is faded down, then the new
    // wave drawn on top, so the moving point leaves a tail. Sized off the hot path (cf. Particles).
    void prepare() override {
        const lengthType w = width(), h = height();
        const uint8_t cpl = channelsPerLight();
        const size_t needed = static_cast<size_t>(w) * h * cpl;
        const size_t had = trail_.bytes();
        // resize() reallocs (zero-filled) only when the byte count changes, and frees on 0.
        trail_.resize(needed);
        if (needed > 0 && needed == had && (w != trailW_ || h != trailH_ || cpl != trailCpl_)) {
            // Same byte count but different geometry (e.g. 8×16 → 16×8): resize() left the old bytes
            // in place, but they're laid out for the old w/h, so reusing them smears stale pixels.
            // Clear rather than realloc.
            std::memset(trail_.data(), 0, trail_.bytes());
        }
        trailW_ = w; trailH_ = h; trailCpl_ = cpl;
    }

    void tick() MM_NONBLOCKING override {
        if (!trail_) return;
        const lengthType w = width();
        const lengthType h = height();
        const uint8_t cpl = channelsPerLight();
        uint8_t* buf = buffer();
        if (w == 0 || h == 0 || cpl < 3) return;

        // 1. Fade the trail (scale8 toward black) — a smaller `fade` = shorter tail.
        for (size_t i = 0; i < trail_.bytes(); i++) trail_[i] = scale8(trail_[i], fade);

        // 2. Advance the travel phase from bpm. First tick: seed lastElapsed_ to now so the wave
        //    starts from phase 0 instead of jumping by the whole device uptime (lastElapsed_ is 0
        //    until the first loop). Afterwards advance by the real per-tick delta.
        const uint32_t now = elapsed();
        if (!started_) { lastElapsed_ = now; started_ = true; }
        phase_ += static_cast<uint64_t>(now - lastElapsed_) * bpm;
        lastElapsed_ = now;
        const uint8_t t = static_cast<uint8_t>((phase_ * 256) / 60000);    // uint8 angle (256 = full turn)
        // Color cycles slowly over time: now/50 indexes the active palette via waveColor.
        const uint8_t colorIndex = static_cast<uint8_t>(now / 50);

        // 3. Plot the wave point per column, joining discontinuous shapes to the previous column.
        const RGB c = waveColor(colorIndex);
        int prevY = -1;
        for (lengthType x = 0; x < w; x++) {
            const uint8_t ph = static_cast<uint8_t>(t + static_cast<uint8_t>(x) * kColumnSkew);
            const lengthType y = waveY(ph, h);
            plot(x, y, c, cpl, w);
            // Vertical join: fill between prevY and y so sawtooth/square read as a connected line
            // (sine/triangle are continuous, but joining is harmless — adjacent ys differ by ≤1).
            if (prevY >= 0) {
                const lengthType lo = prevY < y ? prevY : y;
                const lengthType hi = prevY < y ? y : prevY;
                for (lengthType yy = lo; yy <= hi; yy++) plot(x, yy, c, cpl, w);
            }
            prevY = static_cast<int>(y);
        }

        // 4. Blit the trail (faded history + this frame's wave) into the output buffer.
        std::memcpy(buf, trail_.data(), trail_.bytes());
    }

    // Test seam: the pure waveform map (phase → y), no buffer/time needed.
    static lengthType waveYForTest(uint8_t type, uint8_t phase, lengthType h) {
        return waveY(type, phase, h);
    }

private:
    // How much each column delays the phase — the horizontal travel speed of the wave shape.
    static constexpr uint8_t kColumnSkew = 8;

    // Persistent z=0-plane trail (w·h·cpl). The buffer sizes itself in prepare(), frees itself
    // on disable/teardown, and reports its own bytes. The geometry (trailW_/H_/Cpl_) is tracked
    // separately so prepare() can clear a same-byte-count reshape (which resize() can't detect).
    ScratchBuffer<uint8_t> trail_{*this};
    lengthType trailW_ = 0, trailH_ = 0;   // geometry the trail bytes are laid out for (clear on change)
    uint8_t  trailCpl_ = 0;
    uint64_t phase_ = 0;
    uint32_t lastElapsed_ = 0;
    bool     started_ = false;   // first-tick guard: seed lastElapsed_ before the first delta

    // The color for the wave this frame — one place: a lookup into the global active palette,
    // so the wave recolors when the palette changes.
    static RGB waveColor(uint8_t index) { return colorFromPalette(*Palettes::active(), index); }

    // Map a phase (uint8 angle) to a y in [0, h) for the selected waveform. Integer-only.
    lengthType waveY(uint8_t phase, lengthType h) const { return waveY(type, phase, h); }
    static lengthType waveY(uint8_t type, uint8_t phase, lengthType h) {
        if (h == 0) return 0;
        uint8_t v;   // the waveform value, 0..255, then scaled to [0, h)
        switch (type) {
            case 0: v = phase; break;                                        // Sawtooth: ramp 0→255
            case 1: v = triangle8(phase); break;                             // Triangle: up then down
            case 2: v = sin8(phase); break;                                  // Sine
            case 3: v = phase < 128 ? 0 : 255; break;                        // Square: low then high
            case 4: v = static_cast<uint8_t>(                                // Sin3: three summed sines
                        (sin8(phase) + sin8(static_cast<uint8_t>(phase * 2))
                                     + sin8(static_cast<uint8_t>(phase * 3))) / 3); break;
            default: v = inoise8(phase); break;                              // Noise (type 5) — shared 1D value noise
        }
        const lengthType y = static_cast<lengthType>((static_cast<uint32_t>(v) * h) / 256);
        return y < h ? y : static_cast<lengthType>(h - 1);
    }

    // Triangle wave: 0→255 over the first half, 255→0 over the second (the textbook fold of a ramp).
    static uint8_t triangle8(uint8_t x) {
        return x < 128 ? static_cast<uint8_t>(x * 2)
                       : static_cast<uint8_t>((255 - x) * 2);
    }

    // Write one pixel into the trail plane (bounds-checked; the join loop can reach any y).
    void plot(lengthType x, lengthType y, const RGB& c, uint8_t cpl, lengthType w) {
        if (x < 0 || y < 0 || x >= w) return;
        const size_t off = (static_cast<size_t>(y) * w + x) * cpl;
        if (off + 2 >= trail_.bytes()) return;
        trail_[off + 0] = c.r;
        trail_[off + 1] = c.g;
        trail_[off + 2] = c.b;
    }
};

} // namespace mm
