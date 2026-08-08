#pragma once

#include "light/effects/EffectBase.h"
#include "light/particles.h"   // FrameTime — the shared time scale

namespace mm {

// Bouncing Balls: a column of balls per x. Each ball is launched upward, falls under gravity,
// bounces with energy loss (dampening), and is re-launched when its velocity dies out — the
// classic 1D "BouncingBalls" pattern, run independently for every grid column so a 2D panel shows
// a forest of bouncing dots. Each ball's vertical position is the analytic projectile equation
// (½·g·t² + v₀·t), so the motion is real physics rather than a frame-step integration; the per-ball
// `impactVelocity` and `lastBounceTime` carry the state between frames.
//
// Prior art: MoonLight's BouncingBalls (E_MoonModules / MoonModules), itself the 2D-column
// generalisation of WLED's "Bouncing Balls" (Aircoookie, ported from Danny Wilson's idea) which is
// in turn the FastLED bouncing-balls demo lineage. The gravity constant (-9.81), the
// (255-grav)/64+1 time-scale, the 0.9 - i/(numBalls²) dampening, the √(-2g)·rand(5,11)/10 relaunch
// kick, and the palette-index spacing are reproduced exactly here, written fresh on EffectBase + the
// shared draw primitives. Per-column ball state lives on the heap (sized to width()×maxNumBalls),
// allocated in prepare and freed in release — never a large inline member.
// Author: Andrew Tuline (WLED-SR) — https://github.com/MoonModules/MoonLight/blob/main/src/MoonLight/Nodes/Effects/E_WLED.h
/// Physics effect: gravity-bounced balls trailing along the layer.
class BouncingBallsEffect : public EffectBase {
public:
    const char* tags() const override { return "💫🐙"; }  // MoonLight origin · 2D
    // Writes only the z=0 slice (one ball column per x, ball drawn at (x, pos)); Layer::extrude
    // duplicates it across z on 3D layers.
    Dim dimensions() const override { return Dim::D2; }

    static constexpr uint8_t maxNumBalls = 16;

    uint8_t grav     = 128;  // gravity strength (0..255); higher = faster fall (shorter time-scale)
    uint8_t numBalls = 8;    // balls per column (1..maxNumBalls)

    void defineControls() override {
        controls_.addUint8("grav", grav, 0, 255);
        controls_.addUint8("numBalls", numBalls, 1, maxNumBalls);
    }

    void prepare() override {
        // One ball array per x column: balls[width][maxNumBalls], flattened. resize() reallocs only
        // when the count changes and zero-fills — Ball is trivially zeroable, so a zeroed block equals
        // the MoonLight Ball{} init (height 0 / impactVelocity 0, bounces on the first frame). Frees on
        // 0; the disable path frees it through MoonModule::release().
        const size_t cols = static_cast<size_t>(width() > 0 ? width() : 0);
        balls_.resize(cols * maxNumBalls);
    }

    void tick() MM_NONBLOCKING override {
        if (!balls_) return;

        const int cols = width();
        const int rows = height();

        const draw::Canvas cv = canvas();

        // Motion trail: dim the whole buffer each frame (MoonLight: fadeToBlackBy(100)).
        // The trail fades per unit TIME, not per frame: fading once per frame makes the tail length a
        // property of the framerate (erased before the eye sees it on a fast device, smeared on a
        // slow one). See architecture.md, the tick-rate rule.
        {
            // Carry the fraction rather than rounding it up to 1: `fadeToBlackBy` runs once per
            // RENDER, so a floor of 1 at high fps applies many times the intended decay and the
            // trail is visibly shorter on a fast device than on a slow one.
            fadeCarry_ += 100u * trailTime_.advance(elapsed());
            uint32_t amt = fadeCarry_ / particles::FrameTime::kOne;
            if (amt > 0) {
                fadeCarry_ -= amt * particles::FrameTime::kOne;
                if (amt > 255) amt = 255;
                layer()->fadeToBlackBy(static_cast<uint8_t>(amt));
            }
        }

        constexpr float gravity = -9.81f;
        const uint32_t time = elapsed();

        // Clamp the active ball count to the array bound, exactly as the source does (min(numBalls,
        // maxNumBalls)). numBalls is also the divisor in the dampening and palette spacing below.
        const int nBalls = numBalls < maxNumBalls ? numBalls : maxNumBalls;
        if (nBalls <= 0) return;

        // Time-scale: divides real elapsed-ms so higher `grav` means a shorter (faster) fall.
        // MoonLight: timeSinceLastBounce = (time - lastBounceTime) / ((255-grav)/64 + 1).
        const uint32_t timeScale = static_cast<uint32_t>((255 - grav) / 64 + 1);

        for (int x = 0; x < cols; x++) {
            Ball* column = balls_.data() + static_cast<size_t>(x) * maxNumBalls;
            for (int i = 0; i < nBalls; i++) {
                Ball& ball = column[i];

                // Divide in FLOAT, not integer. MoonLight truncates here, which is harmless at its
                // fixed ~40 fps because the elapsed delta always exceeds `timeScale`. On a device
                // rendering hundreds or thousands of frames a second the delta is often SMALLER, so
                // the integer form truncates to zero and the ball's own clock never advances: the
                // balls freeze on the floor and the effect dies (measured: 270 lit pixels at 60 fps
                // against 24 at 1200). Keeping the fraction makes the trajectory identical at every
                // framerate, which is the system rule in architecture.md; the visible behaviour at
                // 40 fps is unchanged to within a rounding step.
                const float timeSinceLastBounce =
                    static_cast<float>(time - ball.lastBounceTime) / static_cast<float>(timeScale);
                const float timeSec = timeSinceLastBounce / 1000.0f;
                float height = (0.5f * gravity * timeSec + ball.impactVelocity) * timeSec;

                if (height <= 0.0f) {
                    // Ball has hit the floor: bounce. Lose energy by `dampening`, which grows with i
                    // so higher balls in the stack die out faster. MoonLight:
                    //   dampening = 0.9 - float(i)/(numBalls*numBalls)
                    height = 0.0f;
                    const float dampening = 0.9f - static_cast<float>(i) / static_cast<float>(nBalls * nBalls);
                    ball.impactVelocity = dampening * ball.impactVelocity;
                    ball.lastBounceTime = time;

                    if (ball.impactVelocity < 0.015f) {
                        // Energy spent: relaunch with a fresh upward kick. MoonLight:
                        //   impactVelocity = sqrt(-2 * gravity) * random8(5,11)/10
                        // sqrt(-2*gravity) = sqrt(19.62); random8(5,11) ∈ [5,10] (FastLED random8 is
                        // half-open), so the kick scales by 0.5..1.0.
                        ball.impactVelocity = std::sqrt(-2.0f * gravity) * static_cast<float>(rng_.below(5, 11)) / 10.0f;
                    }
                } else if (height > 1.0f) {
                    continue;  // ball flew off the top of the column this frame — draw nothing
                }

                // Map the 0..1 ball height onto the column, top-anchored: height 0 → bottom row.
                const int pos = (rows - 1) - static_cast<int>(lroundf(height * static_cast<float>(rows - 1)));

                // Palette spacing: each ball gets a slice of the wheel; divisor is max(numBalls,8) so
                // small ball counts still spread across the palette (MoonLight: i * (256/max(numBalls,8))).
                const int paletteDiv = nBalls > 8 ? nBalls : 8;
                const uint8_t index = static_cast<uint8_t>(i * (256 / paletteDiv));
                const RGB color = colorFromPalette(*Palettes::active(), index);

                draw::pixel(cv, {static_cast<lengthType>(x), static_cast<lengthType>(pos), 0}, color);
            }
        }
    }

private:
    // One ball's state. MoonLight's struct: { float height; float impactVelocity; unsigned long
    // lastBounceTime; }. `height` isn't carried between frames (recomputed analytically each loop),
    // so only the two persistent fields are stored.
    struct Ball {
        float impactVelocity = 0.0f;
        uint32_t lastBounceTime = 0;
    };

    // balls[width][maxNumBalls], flattened; column x = balls_.data() + x*maxNumBalls. Self-sizing,
    // self-freeing, self-reporting.
    ScratchBuffer<Ball> balls_{*this};
    Random8 rng_;               // relaunch-kick randomness (FastLED random8(5,11) → below(5,11))

    particles::FrameTime trailTime_{60};
    uint32_t fadeCarry_ = 0;   // sub-frame trail fade not yet applied   // trail decay is per second, not per frame
};

} // namespace mm