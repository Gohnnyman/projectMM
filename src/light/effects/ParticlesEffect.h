#pragma once

#include "light/effects/EffectBase.h"
#include "light/particles.h"

namespace mm {

// Author: WildCats08 / @Brandon502 (MoonLight) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_MoonLight.h
/// Particle-system effect with spawned, moving points.
/// @card ParticlesEffect.png
class ParticlesEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🦅"; }  // MoonLight origin · David Jupijn / Rising Step
    // Iterates y and x only; Layer::extrude fills z on 3D layers. The trail
    // buffer is sized to the z=0 plane (w*h*cpl), not the full 3D buffer.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t MAX_PARTICLES = 64;

    uint8_t count = 32;
    uint8_t speed = 80;
    uint8_t fade = 240;
    uint8_t hue_shift = 0;

    void defineControls() override {
        controls_.addControl("count", count, 1, 255);
        controls_.addControl("speed", speed, 1, 255);
        controls_.addControl("fade", fade, 1, 255);
        controls_.addControl("hue_shift", hue_shift, 0, 255);
    }

    void prepare() override {
        // D2 effect: trail buffer covers only the z=0 plane (w*h*cpl). Extrude fills z on 3D
        // layers — avoids allocating depth× more heap than needed. resize() reallocs (zero-filled)
        // only when the byte count changes, frees on 0, and keeps dynamicBytes current.
        trail_.resize(static_cast<size_t>(width()) * height() * channelsPerLight());
        // `trail_` IS the degenerate-grid gate on this path. Layer::tick's gate covers tick()
        // only, and prepare() runs outside it — so a zero grid is stopped here by resize(0)
        // freeing the buffer, not by that gate. Without this, initParticles seeds every
        // particle from `(rand8() * w) >> 4` with w == 0.
        px_.resize(MAX_PARTICLES); py_.resize(MAX_PARTICLES);
        vx_.resize(MAX_PARTICLES); vy_.resize(MAX_PARTICLES);
        ttl_.resize(MAX_PARTICLES); hue_.resize(MAX_PARTICLES);
        if (px_ && py_ && vx_ && vy_ && ttl_ && hue_) {
            pool_ = particles::Pool{};
            pool_.x = px_.data(); pool_.y = py_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = MAX_PARTICLES;
        } else {
            pool_ = particles::Pool{};   // a failed resize must leave valid() false, not a stale pool
        }
        if (trail_) initParticles();
        time_.reset();
    }

    void tick() MM_NONBLOCKING override {
        if (!trail_) return;

        lengthType w = width();
        lengthType h = height();
        uint8_t cpl = channelsPerLight();
        uint8_t* buf = buffer();

        // 1. Fade the persistent trail, by the fraction of a reference frame that elapsed. This is
        // ordinary exponential decay over a buffer — scaling the AMOUNT is the right fix, unlike
        // Echo where the frame re-samples its own transformed output and the compounding cannot be
        // undone by a smaller step. Gating it on a whole frame elapsing (tried first) meant a fast
        // device almost never faded at all: the trail never decayed and the effect turned into a
        // solid mass while particles kept drawing every frame.
        const uint32_t fadeSlice = time_.advance(elapsed());
        if (fadeSlice > 0) {
            // `fade` is "keep this much" per reference frame, so a partial frame must lose less.
            // The remainder CARRIES: at 1200 fps one frame's share of a fade of 240 is
            // (255-240)*12/256 == 0, so truncating meant the trail never decayed at all and the
            // effect turned solid — measured, and the exact symptom the PO saw on the desktop.
            fadeCarry_ += (255u - fade) * fadeSlice;
            const uint32_t lost = fadeCarry_ / particles::FrameTime::kOne;
            if (lost > 0) {
                fadeCarry_ -= lost * particles::FrameTime::kOne;
                const uint8_t keep = static_cast<uint8_t>(lost >= 255 ? 0 : 255 - lost);
                for (size_t i = 0; i < trail_.bytes(); i++) trail_[i] = scale8(trail_[i], keep);
            }
        }

        // 2. Move and draw the particles — the kernel owns the physics now. The private 12.4 pair,
        // the hand-rolled integrate and the four wall tests are gone; what stays is this effect's
        // own character: how fast the drift is, and how the hue maps.
        if (!pool_.valid()) return;
        const draw::pos_t maxX = draw::toSub(w - 1);
        const draw::pos_t maxY = draw::toSub(h - 1);

        // The pool holds MAX_PARTICLES; `count` is how many the user wants alive, so the tail is
        // parked rather than reallocated when the control moves.
        const uint8_t live = count < MAX_PARTICLES ? count : MAX_PARTICLES;
        for (uint16_t i = 0; i < MAX_PARTICLES; i++) pool_.ttl[i] = (i < live) ? 255 : 0;

        // `speed` scales the stored velocity, and FrameTime keeps the drift rate identical on every
        // target (architecture.md, tick-rate rule) — the old form advanced a fixed step per frame.
        // step(scale) moves by velocity*scale/256, and the original moved by velocity*speed/64 per
        // frame — so one reference frame is scale = speed*4, and the elapsed fraction scales that.
        // Folding `speed` into the FrameTime value directly (the first attempt) made the step ~80x
        // too large and reintroduced framerate coupling, which the audit caught at 6.2x.
        const uint32_t fs = fadeSlice;
        if (fs > 0) {
            // The original moved by velocity*speed/64 each frame; with velocities now in 24.8 the
            // same proportion is scale = speed*4, scaled again by the elapsed fraction.
            // Carry the remainder, for the same reason the fade does: at 1200 fps a slow `speed`
            // divides to 0 every frame and the particles simply stop, while the identical setting
            // moves at 60. Accumulating the numerator spends whole steps and keeps the fraction.
            stepCarry_ += static_cast<uint32_t>(speed) * 4u * fs;
            const uint32_t scale = stepCarry_ / particles::FrameTime::kOne;
            stepCarry_ -= scale * particles::FrameTime::kOne;
            pool_.step(scale);
            pool_.bounce(maxX, maxY, 256);          // 256 = a perfect bounce, as before
        }

        // Draw into the persistent trail: one whole pixel per particle, matching the original.
        draw::Canvas trailCv{trail_.data(), trail_.bytes(), {w, h, 1}, cpl};
        for (uint16_t i = 0; i < live; i++) {
            // The ACTIVE PALETTE, like every other particle effect (BallpitEffect does the
            // same lookup). A raw hsvToRgb ignored the user's palette choice, so this effect
            // alone stayed rainbow when the device was set to anything else.
            const RGB c = colorFromPalette(*Palettes::active(),
                                           static_cast<uint8_t>(pool_.hue[i] + hue_shift), 255);
            draw::pixel(trailCv, {static_cast<lengthType>(draw::toPixel(pool_.x[i])),
                                  static_cast<lengthType>(draw::toPixel(pool_.y[i])), 0}, c);
        }

        // 3. Copy persistent trail buffer to layer buffer (layer cleared it)
        std::memcpy(buf, trail_.data(), trail_.bytes());
    }

private:
    // The particle state lives in the shared kernel's SoA pool rather than a private struct — the
    // 12.4 pair, the hand-rolled integrate and the wall tests it used to carry are all the kernel's.
    ScratchBuffer<draw::pos_t> px_{*this}, py_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};
    ScratchBuffer<uint8_t> hue_{*this};
    particles::Pool pool_;
    particles::FrameTime time_{60};
    uint32_t fadeCarry_ = 0;   // sub-frame fade remainder; see tick()
    uint32_t stepCarry_ = 0;   // sub-step motion not yet spent, so a slow speed still moves at high fps
    bool initialized_ = false;
    // Persistent z=0-plane trail (w·h·cpl). Self-sizing, self-freeing, self-reporting.
    ScratchBuffer<uint8_t> trail_{*this};
    Random8 rng_{0xBADF00Du};   // the shared PRNG; rand8() adapts it to the call shape below
    uint8_t rand8() { return rng_.next8(); }

    void initParticles() {
        lengthType w = width();
        lengthType h = height();
        if (initialized_) return;
        if (!pool_.valid()) return;
        pool_.clear();
        for (uint16_t i = 0; i < MAX_PARTICLES; i++) {
            int32_t sx = static_cast<int8_t>(rand8()) >> 1;
            int32_t sy = static_cast<int8_t>(rand8()) >> 1;
            if (sx == 0) sx = 1;
            if (sy == 0) sy = 1;
            pool_.x[i] = static_cast<draw::pos_t>((static_cast<uint32_t>(rand8()) * w) >> 8) * draw::kSubOne;
            pool_.y[i] = static_cast<draw::pos_t>((static_cast<uint32_t>(rand8()) * h) >> 8) * draw::kSubOne;
            // Velocities are SUB-PIXEL per reference frame: the original's ±63 meant sixteenths of
            // a pixel (12.4), so the same motion in 24.8 is that value times 16. Storing the raw
            // number instead makes each particle move 1/256th of a pixel a frame, which rounds to
            // nothing on a fast device and froze the whole effect — the audit caught it as 6.2x.
            pool_.vx[i] = static_cast<draw::pos_t>(sx * 16);
            pool_.vy[i] = static_cast<draw::pos_t>(sy * 16);
            pool_.hue[i] = rand8();
            pool_.ttl[i] = 255;
        }
        initialized_ = true;
    }
};

} // namespace mm
