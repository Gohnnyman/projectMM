#pragma once

#include "core/math16.h"              // hashInt
#include "light/effects/EffectBase.h"
#include "light/particles.h"

namespace mm {

// Ballpit: a heap of balls that fall, pile up, and shove each other out of the way.
//
// This is the half of the particle kernel Fireworks leaves untouched. Sparks never notice each
// other — they are cheap precisely because they don't — so nothing there exercises `collide()`.
// Here the balls are the whole point: they rest on the floor, stack, and a ball dropped into the
// pile pushes the others aside rather than passing through them.
//
// Piling is emergent, not scripted. Gravity pulls everything down, the floor stops it, and contact
// between neighbours does the rest — the shape of the heap is whatever the collisions produce.
//
// `tilt` shows why a force belongs in the kernel rather than in an effect: one call turns the pit
// into a slope and the whole heap slides and re-settles, with no code here that knows about slopes.
//
// Cost: collisions are the one non-linear part of the kernel, so the pool is deliberately small and
// `collide` is called once per frame before the move. At 40 balls that is 780 pair checks, nearly
// all of them rejected on the X test alone.
//
// Prior art: the WLED Particle System's ballpit family (@Brandon502 / WildCats08); the impulse
// response and the single-particle overlap push are the kernel's.
// @card BallpitEffect.png
/// Effect: falling balls that pile up and push each other aside.
class BallpitEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t balls    = 30;   // how many balls share the pit
    uint8_t gravity  = 8;    // how hard they fall
    uint8_t size     = 2;    // contact radius in pixels: how far apart balls sit when touching
    uint8_t bounce   = 120;  // restitution: how much speed a contact keeps
    uint8_t tilt     = 0;    // sideways force; turns the pit into a slope
    uint8_t drag     = 6;    // damping, so the heap settles instead of sloshing forever

    void defineControls() override {
        controls_.addUint8("balls", balls, 1, kPool);
        controls_.addUint8("gravity", gravity, 0, 64);
        controls_.addUint8("size", size, 1, 8);
        controls_.addUint8("bounce", bounce, 0, 255);
        controls_.addUint8("tilt", tilt, 0, 255);
        controls_.addUint8("drag", drag, 0, 64);
    }

    void prepare() override {
        x_.resize(kPool); y_.resize(kPool); vx_.resize(kPool); vy_.resize(kPool);
        ttl_.resize(kPool); hue_.resize(kPool); acc_.resize(kPool);
        if (x_ && y_ && vx_ && vy_ && ttl_ && hue_ && acc_) {
            pool_ = particles::Pool{};
            pool_.x = x_.data(); pool_.y = y_.data();
            pool_.vx = vx_.data(); pool_.vy = vy_.data();
            pool_.ttl = ttl_.data(); pool_.hue = hue_.data(); pool_.acc = acc_.data();
            pool_.count = kPool;
            pool_.clear();
        }
        time_.reset();
        filled_ = false;
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !pool_.valid()) return;

        const draw::pos_t wSub = draw::toSub(w - 1);
        const draw::pos_t hSub = draw::toSub(h - 1);

        // Fill the pit once, spread across the top so the balls fall in rather than starting stacked.
        if (!filled_) {
            const uint8_t n = balls < kPool ? balls : kPool;
            for (uint8_t i = 0; i < n; i++)
                pool_.spawn(static_cast<draw::pos_t>(hashInt(i, 1, 0, kSeed) % static_cast<uint32_t>(wSub ? wSub : 1)),
                            static_cast<draw::pos_t>(hashInt(i, 2, 0, kSeed) % static_cast<uint32_t>(hSub ? hSub : 1)),
                            0, 0, 255, static_cast<uint8_t>(hashInt(i, 3, 0, kSeed)));
            filled_ = true;
        }

        draw::fill(cv, RGB{0, 0, 0});

        // Every frame simulates its own share of a reference frame, so the pit behaves the same at
        // 60 fps and at 3000 (architecture.md, tick-rate rule).
        const uint32_t scale = time_.advance(elapsed());
        if (scale > 0) {
            pool_.gravity(static_cast<draw::pos_t>(gravity), scale);
            if (tilt) pool_.force(static_cast<draw::pos_t>(tilt) / 8, 0, scale);
            pool_.drag(drag, scale);
            // Contacts resolve BEFORE the move: a shove applied after integrating could push a ball
            // through the wall the bounce pass has already checked.
            pool_.collide(draw::toSub(size), bounce, frame_++);
            pool_.step(scale);
            pool_.bounce(wSub, hSub, bounce);
        }

        // Balls are drawn as discs rather than points so contact is visible — two points touching
        // looks like one bright pixel, two discs looks like a pile.
        for (uint16_t i = 0; i < pool_.count; i++) {
            if (!pool_.ttl[i]) continue;
            draw::fillCircle(cv, static_cast<lengthType>(draw::toPixel(pool_.x[i])),
                             static_cast<lengthType>(draw::toPixel(pool_.y[i])),
                             static_cast<lengthType>(size),
                             colorFromPalette(*Palettes::active(), pool_.hue[i], 255));
        }
    }

private:
    static constexpr uint8_t kPool = 40;      // collisions are O(n²); a small pit is the point
    static constexpr uint32_t kSeed = 0xBA11;

    ScratchBuffer<draw::pos_t> x_{*this}, y_{*this}, vx_{*this}, vy_{*this};
    ScratchBuffer<uint16_t> ttl_{*this};   // wide: life beyond 255 frames
    ScratchBuffer<uint8_t> hue_{*this}, acc_{*this};
    particles::Pool pool_;
    particles::FrameTime time_{60};
    uint32_t frame_ = 0;
    bool filled_ = false;
};

}  // namespace mm
