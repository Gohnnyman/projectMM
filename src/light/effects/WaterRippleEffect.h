#pragma once

#include "core/math16.h"              // hashInt — drop placement without a stream RNG
#include "light/effects/EffectBase.h"

namespace mm {

// WaterRipple: a real propagating wave simulation. Drops land on the surface and their rings spread
// outward, reflect off the edges, cross each other and interfere — the crossing is the thing a
// formula cannot fake, because two rings meeting have to add and cancel.
//
// This is the classic two-buffer water algorithm (Hugo Elias): keep the surface height at this step
// and the previous one, and derive the next from the neighbourhood average minus the previous
// height. That one line IS the wave equation, discretised — the ripple is emergent, not drawn.
//
//   next = (sum of 4 neighbours) / 2 - previous,   then damped toward zero
//
// Distinct from RipplesEffect, which draws expanding rings from a closed-form radius: that one is
// cheaper and always looks like clean concentric circles, this one behaves like water. Both earn
// their place; the difference is stated here so a reader picks deliberately.
//
// Cost: two int16 buffers sized to the grid (2 bytes per light each) and one pass of four
// neighbour reads per pixel per frame. That is the cheapest interesting simulation in the toolbox,
// but the MEMORY scales with the fixture, so on a very large wall check the budget.
//
// Drop placement uses `hashInt` rather than a stream RNG, so the same frame on two devices puts
// drops in the same places (the supersync rule).
//
// Prior art: Hugo Elias's water surface algorithm, the standard demoscene/graphics form.
// @card WaterRippleEffect.png
/// Effect: a propagating water surface where drops ripple, reflect and interfere.
class WaterRippleEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t speed    = 60;    // simulation steps per second — how fast the water itself moves
    uint8_t dropRate = 24;    // how often drops land (0 = none, higher = busier)
    uint8_t damping  = 16;    // how fast waves lose energy (higher = calmer water)
    uint8_t strength = 200;   // how hard a drop hits
    bool    colorByHeight = true;   // color the surface by height instead of by brightness alone
    uint8_t hueBase   = 140;  // where in the palette the still surface sits
    uint8_t hueSpread = 110;  // how far a crest and a trough reach from it

    void defineControls() override {
        controls_.addUint8("speed", speed, 1, 120);
        controls_.addUint8("dropRate", dropRate, 0, 255);
        controls_.addUint8("damping", damping, 1, 64);
        controls_.addUint8("strength", strength, 1, 255);
        controls_.addBool("colorByHeight", colorByHeight);
        controls_.addUint8("hueBase", hueBase, 0, 255);
        controls_.addUint8("hueSpread", hueSpread, 0, 127);
    }

    void prepare() override {
        // Two height fields, sized to the grid. resize() frees on teardown and reallocs only when
        // the grid changes, so a live layout change is handled by the framework rather than here.
        const size_t n = static_cast<size_t>(width() > 0 ? width() : 0) *
                         static_cast<size_t>(height() > 0 ? height() : 0);
        a_.resize(n);
        b_.resize(n);
        frame_ = 0;
        lastDropMs_ = 0;
        lastStepMs_ = 0;
        started_ = false;
        agc_ = kAgcFloor;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        // Clamped neighbours make any grid valid, including a strip. The ScratchBuffers may be
        // absent if allocation failed (degrade, never crash).
        if (w < 1 || h < 1 || !a_ || !b_) return;

        // Drops are timed in MILLISECONDS, not per frame. Testing the rate every frame made the
        // downpour scale with the framerate: at 100 fps a rate of 24 landed ~9 drops a second, which
        // reads as rain rather than the slow rippling the effect is for.
        const uint32_t now = elapsed();
        const uint32_t interval = dropRate > 0
            ? static_cast<uint32_t>(2000u - (static_cast<uint32_t>(dropRate) * 7u))   // 2000ms..215ms
            : 0u;
        // `started_` makes the FIRST drop land immediately: waiting a full interval before the
        // first one leaves the surface flat for up to two seconds after the effect is selected,
        // which reads as broken rather than calm.
        const bool dropNow = dropRate > 0 && (!started_ || (now - lastDropMs_) >= interval);
        started_ = true;
        if (dropNow) {
            lastDropMs_ = now;
            frame_++;                       // advances only on a drop, so each drop lands elsewhere
        }

        // Land a drop. hashInt makes the placement a pure function of the drop counter, so two
        // devices agree on where drops fall without exchanging anything.
        if (dropNow) {
            const lengthType dx = static_cast<lengthType>(hashInt(frame_, 1, 0, kDropSeed) % w);
            const lengthType dy = static_cast<lengthType>(hashInt(frame_, 2, 0, kDropSeed) % h);
            field()[static_cast<size_t>(dy) * w + dx] = static_cast<int16_t>(strength * 64);
        }

        // The wave advances on WALL-CLOCK time, not once per rendered frame. A wave step is a fixed
        // amount of simulated time, so tying it to the framerate made the same water look calm at
        // 25 fps and frantic at 100 — the speed of the ripples was a property of the hardware
        // rather than a setting. Stepping on a timer makes `speed` mean the same thing everywhere.
        // Advance the surface only when a simulation step is due. Rendering still happens EVERY
        // frame from whatever the surface currently holds, so a slow speed shows calm water rather
        // than a stuttering panel. At most one step per frame: a long stall must not spend the
        // whole frame budget replaying the simulation.
        const uint32_t stepMs = 1000u / (speed > 0 ? speed : 1);
        if (now - lastStepMs_ >= stepMs) {
            lastStepMs_ = now;
            // The wave step reads the current field and writes the previous one, which then BECOMES
            // the current field when the roles flip below. No buffer is copied.
            //
            // EVERY pixel steps, including the border. Looping the interior only (1..h-2) is the
            // usual way to keep the neighbour reads in bounds, but it leaves the outermost ring
            // frozen at zero — a dead one-pixel frame around the whole fixture, which on a real
            // panel is a visibly unlit edge. Clamping the neighbour coordinate instead means an
            // edge pixel reads itself in place of the missing neighbour, which is the standard
            // reflecting (Neumann) boundary: a wave bounces off the wall rather than vanishing
            // into it.
            for (lengthType y = 0; y < h; y++) {
                for (lengthType x = 0; x < w; x++) {
                    const size_t i = static_cast<size_t>(y) * w + x;
                    const size_t left  = static_cast<size_t>(y) * w + (x > 0 ? x - 1 : 0);
                    const size_t right = static_cast<size_t>(y) * w + (x < w - 1 ? x + 1 : w - 1);
                    const size_t up    = static_cast<size_t>(y > 0 ? y - 1 : 0) * w + x;
                    const size_t down  = static_cast<size_t>(y < h - 1 ? y + 1 : h - 1) * w + x;
                    const int32_t neighbours = static_cast<int32_t>(field()[left]) + field()[right]
                                             + field()[up] + field()[down];
                    int32_t next = (neighbours >> 1) - lastField()[i];
                    next -= next * damping >> 10;        // damping: the surface settles
                    // Clamp before narrowing: two drops interfering constructively can exceed
                    // int16, and a wrap flips the wave's sign — the surface turns inside out at the
                    // exact moment it should be brightest.
                    if (next > 32767) next = 32767;
                    if (next < -32768) next = -32768;
                    lastField()[i] = static_cast<int16_t>(next);
                }
            }
            flip_ = !flip_;   // the fields exchange roles; no data moves
        }

        // Brightness is scaled against the frame's own peak, not a fixed shift. A drop's energy
        // spreads over the whole surface, so the amplitude PER PIXEL falls as the grid grows: a
        // fixed `>> 6` peaked at 31/255 on a 16x16 grid and only 11/255 at 64x64, which is why the
        // surface looked almost unlit and worse the higher the resolution. Tracking the peak makes
        // the effect look the same at every size.
        uint32_t peak = 0;
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const int32_t v = field()[static_cast<size_t>(y) * w + x];
                const uint32_t m = static_cast<uint32_t>(v < 0 ? -v : v);
                if (m > peak) peak = m;
            }
        // Follow the peak up instantly and down slowly, so brightness does not pump frame to frame;
        // the floor keeps flat water dark instead of amplifying the last ripple to full scale.
        if (peak > agc_) agc_ = peak; else agc_ -= (agc_ - peak) >> 5;
        if (agc_ < kAgcFloor) agc_ = kAgcFloor;
        // Scale against a TYPICAL ripple, not the single tallest pixel. A drop's energy spreads out,
        // so the mean magnitude measures ~8% of the peak and over half the surface sits at exactly
        // zero; normalising against the peak therefore mapped the typical pixel to ~8% brightness
        // and to a palette index a few steps from the centre — a dark, single-hue surface however
        // the water moved. A fraction of the peak puts the ripples themselves in range, and the
        // clamp below keeps the few tallest crests from blowing out.
        const uint32_t ref = agc_ > 8u ? (agc_ / 4u) : 2u;

        // Render: height becomes brightness, and optionally palette index, so crests and troughs
        // read differently rather than both showing as "bright".
        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const int32_t v = field()[static_cast<size_t>(y) * w + x];
                const int32_t mag = v < 0 ? -v : v;
                // Square-root response. The wave's amplitude falls off with distance from the drop,
                // so a linear map leaves one bright ridge and a dim surface; the sqrt lifts the
                // mid-range where the ripples actually are (measured: 3% of pixels clearly visible
                // linear vs 32% sqrt on a 64x64 grid).
                uint32_t lin = (static_cast<uint32_t>(mag) * 255u) / ref;
                if (lin > 255) lin = 255;
                uint8_t bri = static_cast<uint8_t>(isqrt(lin * 255u));
                if (bri == 0) { draw::pixel(cv, {x, y, 0}, RGB{0, 0, 0}); continue; }
                // A crest and a trough sit at opposite ends of the palette. The height is scaled
                // against the SAME running peak the brightness uses, so the index spans the whole
                // palette: a fixed shift (`v >> 7`) left every pixel within a few steps of 128,
                // which is one narrow colour band and why the surface read as a single dark hue.
                // Map the height across the WHOLE palette. Centring on 128 pinned every pixel to
                // one colour (128 is a single palette entry — cyan in the default rainbow), so the
                // surface could only ever be one hue however the water moved. Spanning the range
                // means a trough and a crest genuinely differ, and `hueSpread` sets how much of the
                // palette the water uses.
                uint8_t idx;
                if (colorByHeight) {
                    const int32_t rel = (v * static_cast<int32_t>(hueSpread)) / static_cast<int32_t>(ref);
                    const int32_t lo = -static_cast<int32_t>(hueSpread);
                    const int32_t clamped = rel > hueSpread ? hueSpread : (rel < lo ? lo : rel);
                    idx = static_cast<uint8_t>(hueBase + clamped);
                } else {
                    idx = static_cast<uint8_t>(hueBase + (bri >> 1));
                }
                draw::pixel(cv, {x, y, 0}, colorFromPalette(*Palettes::active(), idx, bri));
            }
        }
    }

private:
    static constexpr uint32_t kDropSeed = 0x5EAu;

    // The two fields exchange roles each frame rather than exchanging CONTENTS: copying the whole
    // grid every frame to achieve a swap would cost a second full pass for nothing. `flip_` says
    // which buffer currently holds "this step".
    ScratchBuffer<int16_t>& field()     { return flip_ ? b_ : a_; }
    ScratchBuffer<int16_t>& lastField() { return flip_ ? a_ : b_; }

    static constexpr uint32_t kAgcFloor = 2048;   // below this the surface reads as flat, not lit

    uint32_t agc_ = kAgcFloor;          // the amplitude brightness is measured against
    ScratchBuffer<int16_t> a_{*this};   // the two height fields; roles alternate via flip_
    ScratchBuffer<int16_t> b_{*this};
    bool flip_ = false;
    uint32_t frame_ = 0;        // counts DROPS, not frames: it seeds each drop's position
    uint32_t lastDropMs_ = 0;   // when the last drop landed
    uint32_t lastStepMs_ = 0;   // when the surface last advanced
    bool     started_ = false;  // so the first drop does not wait for the interval
};

}  // namespace mm
