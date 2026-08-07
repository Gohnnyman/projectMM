#pragma once

#include "core/math16.h"              // BeatPhase, hashInt
#include "light/effects/EffectBase.h"
#include "light/particles.h"

namespace mm {

// Fireworks: shells rise, stall, and burst into a shower of sparks that arc over and fall.
//
// This is the particle kernel end to end — every stage of a firework is a kernel call, and the
// effect itself only decides when to launch and what colour:
//
//   launch   -> pool.spawn        a shell with upward velocity
//   rise     -> pool.gravity      the shell decelerates on its own; no timer decides the apex
//   burst    -> pool.angleEmit    a ring of sparks from wherever the shell actually stalled
//   fall     -> gravity + drag    the arc, and air resistance flattening it
//   fade     -> pool.age          brightness rides ttl, so a spark dies as it dims
//
// The apex is worth pointing at: nothing schedules it. The shell is launched upward, gravity acts
// on it every frame, and the burst fires when its vertical velocity crosses zero — the physics
// decides, which is why a faster launch bursts higher without a second control.
//
// Trails come from the Layer's `fadeToBlackBy`, not from the pool: one decay path for the whole
// system rather than a second hidden inside particles.
//
// Cost: one pass per force over a pool the effect sizes itself, plus a sub-pixel splat per live
// spark. At the default 120 particles that is well inside the budget on any target.
//
// Prior art: the WLED Particle System's firework family (@Brandon502 / WildCats08) for the effect
// vocabulary; the physics is the kernel's.
// @card FireworksEffect.png
/// Effect: shells that rise, stall, and burst into falling sparks.
class FireworksEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t launchRate = 30;    // how often a new shell goes up
    uint8_t launchSpeed = 150;  // how hard it is thrown, and so how high it bursts
    uint8_t gravity = 6;        // how fast everything falls, per 60 Hz step
    uint8_t sparks = 20;        // sparks per burst
    uint8_t sparkLife = 200;    // how long a spark survives
    uint8_t drag = 3;           // air resistance flattening the arc
    uint8_t fade = 25;          // trail length (the Layer's decay, not the pool's)

    void defineControls() override {
        controls_.addUint8("launchRate", launchRate, 1, 255);
        controls_.addUint8("launchSpeed", launchSpeed, 10, 255);
        controls_.addUint8("gravity", gravity, 1, 128);
        controls_.addUint8("sparks", sparks, 1, 64);
        controls_.addUint8("sparkLife", sparkLife, 10, 255);
        controls_.addUint8("drag", drag, 0, 64);
        controls_.addUint8("fade", fade, 1, 255);
    }

    void prepare() override {
        // The pool is sized once here and never reallocated. Shells are tracked separately because
        // a shell is not a spark: it bursts rather than simply dying.
        x_.resize(kPool); y_.resize(kPool); vx_.resize(kPool); vy_.resize(kPool);
        ttl_.resize(kPool); hue_.resize(kPool);
        if (x_ && y_ && vx_ && vy_ && ttl_ && hue_) {
            pool_ = particles::Pool{};
            pool_.x = x_.data(); pool_.y = y_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data();
            pool_.count = kPool;
            pool_.clear();
        }
        shellLive_ = false;
        frame_ = 0;
        time_.reset();
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !pool_.valid()) return;


        const draw::pos_t gy = static_cast<draw::pos_t>(gravity);
        const draw::pos_t wSub = draw::toSub(w - 1);
        const draw::pos_t hSub = draw::toSub(h - 1);

        // EVERY frame simulates, by the fraction of a reference frame it actually covered. The
        // controls are written against 60 fps, so a device rendering at 600 takes ten steps a tenth
        // the size — the same trajectory at ten times the resolution. Quantising to a fixed 60 Hz
        // and skipping frames would throw away exactly the smoothness the extra frames buy.
        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) {
            frame_++;
            simulate(gy, wSub, hSub, scale);
            // The trail decays per unit TIME too, scaled the same way, or its length would be a
            // property of the framerate: erased before the eye sees it on a fast device, smeared on
            // a slow one.
            uint32_t f = (static_cast<uint32_t>(fade) * scale) / particles::FrameTime::kOne;
            if (f == 0) f = 1;
            layer()->fadeToBlackBy(static_cast<uint8_t>(f > 255 ? 255 : f));
        }

        pool_.render(cv, sparkLife);

        // Draw the rising shell as a single bright point, so the launch reads as a shell rather
        // than as nothing happening until the burst.
        if (shellLive_)
            draw::splat(cv, shellX_, shellY_, colorFromPalette(*Palettes::active(), shellHue_, 255));
    }

private:
    /// One fixed slice of simulated time: launch, rise, burst, fall, age. No drawing.
    void simulate(draw::pos_t gy, draw::pos_t wSub, draw::pos_t hSub, uint32_t scale) MM_NONBLOCKING {
        // --- The shell ---------------------------------------------------------------------
        if (shellLive_) {
            const auto sc = [scale](draw::pos_t v) {
                return static_cast<draw::pos_t>(particles::scaleSigned(v, static_cast<int32_t>(scale),
                                                                       particles::FrameTime::kOne));
            };
            shellVy_ = static_cast<draw::pos_t>(shellVy_ + sc(gy));   // gravity acts on the shell too
            shellY_  = static_cast<draw::pos_t>(shellY_ + sc(shellVy_));
            shellX_  = static_cast<draw::pos_t>(shellX_ + sc(shellVx_));
            // Burst when it stops rising: the apex is where the physics puts it, not a timer.
            if (shellVy_ >= 0 || shellY_ <= 0) {
                pool_.angleEmit(shellX_, shellY_, 0, static_cast<draw::pos_t>(launchSpeed * 2),
                                65535, sparks, sparkLife, shellHue_, frame_);
                shellLive_ = false;
            }
        } else if ((hashInt(frame_, 0, 0, kSeed) % 256u) < launchRate / 4u) {
            // Launch from a point along the floor, thrown up and slightly sideways.
            shellX_  = static_cast<draw::pos_t>(hashInt(frame_, 1, 0, kSeed) % static_cast<uint32_t>(wSub ? wSub : 1));
            shellY_  = hSub;
            shellVy_ = -static_cast<draw::pos_t>(launchSpeed);
            shellVx_ = static_cast<draw::pos_t>((hashInt(frame_, 2, 0, kSeed) % 33u)) - 16;
            shellHue_ = static_cast<uint8_t>(hashInt(frame_, 3, 0, kSeed));
            shellLive_ = true;
        }

        // --- The sparks --------------------------------------------------------------------
        pool_.gravity(gy, scale);
        pool_.drag(drag, scale);
        pool_.step(scale);
        pool_.killOutside(wSub, hSub, draw::toSub(2));
        pool_.age(1, scale);
    }

    static constexpr uint16_t kPool = 120;
    static constexpr uint32_t kSeed = 0xF17E;

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};   // wide: life beyond 255 frames
    ScratchBuffer<uint8_t> hue_{*this};
    particles::Pool pool_;
    particles::FrameTime time_{60};    // controls are written against 60 fps

    // The shell is one object with its own state: it bursts rather than dying, which is not
    // something the pool models.
    draw::pos_t shellX_ = 0, shellY_ = 0, shellVx_ = 0, shellVy_ = 0;
    uint8_t shellHue_ = 0;
    bool shellLive_ = false;
    uint32_t frame_ = 0;
};

}  // namespace mm
