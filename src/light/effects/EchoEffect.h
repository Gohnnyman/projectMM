#pragma once

#include "core/math16.h"              // sin16, BeatPhase, easeInOutQuad
#include "light/effects/EffectBase.h"

namespace mm {

// Echo: feedback. Each frame the PREVIOUS frame is read back slightly zoomed and rotated, dimmed,
// and a small bright source is drawn on top. The result is a trail that spirals away from itself —
// the visual equivalent of pointing a camera at its own monitor.
//
// This effect exists to make an argument from the canon survey concrete: **feedback is not a
// primitive.** Once the grid can be read as a texture at sub-pixel coordinates, feedback is three
// lines — sample the last frame through a transform, combine, draw a source. The whole family
// (motion trails, zoom blur, spiral smear, kaleidoscopic feedback) follows from `sampleWrap`, so the
// toolbox gains a general capability rather than one more special-case effect.
//
// It is the clearest USE of the gather half of draw.h: `sampleWrap` bilinearly reads the previous
// frame with wrapping, and `combineMax` keeps a trail bright where it overlaps instead of averaging
// it into mud.
//
// Cost: one bilinear sample (4 pixel reads) per pixel, plus one scratch copy of the frame. No
// per-pixel divide and no noise, so this is one of the cheaper full-frame effects.
//
// Prior art: video feedback, a demoscene and video-art staple; the sample-transform-combine
// formulation follows the standard texture-feedback shader shape.
// @card EchoEffect.png
/// Effect: the previous frame fed back through a zoom and rotation, leaving spiralling trails.
class EchoEffect : public EffectBase {
public:
    const char* tags() const override { return "🔬"; }   // power-function showcase
    Dim dimensions() const override { return Dim::D2; }  // writes the z=0 slice; extrude fills z

    uint8_t bpm    = 30;    // how fast the source moves
    uint8_t zoom   = 12;    // how much the feedback grows each frame (0 = no zoom)
    uint8_t rotate = 8;     // rotation per frame, which turns the trail into a spiral
    uint8_t decay  = 24;    // how fast the echo fades (higher = shorter trail)
    uint8_t size   = 2;     // radius of the bright source

    void defineControls() override {
        controls_.addUint8("bpm", bpm, 0, 120);
        controls_.addUint8("zoom", zoom, 0, 64);
        controls_.addUint8("rotate", rotate, 0, 64);
        controls_.addUint8("decay", decay, 1, 128);
        controls_.addUint8("size", size, 0, 16);
    }

    void prepare() override {
        // One frame of history. The feedback has to read a STABLE copy: reading the live buffer
        // while writing it would feed partly-updated pixels back into the transform, which shows up
        // as a directional smear rather than a clean trail.
        const size_t n = static_cast<size_t>(width() > 0 ? width() : 0) *
                         static_cast<size_t>(height() > 0 ? height() : 0) * 3u;
        history_.resize(n);
    }

    void tick() MM_NONBLOCKING override {
        const draw::Canvas cv = canvas();
        const lengthType w = width(), h = height();
        if (w < 2 || h < 2 || !history_) return;

        phase_.advance(elapsed(), bpm);
        const angle16 t = static_cast<angle16>(phase_.phase(65536));

        // Wrap the history in its own Canvas so the gather primitives can read it exactly as they
        // read any grid — the previous frame is just another texture.
        draw::Canvas hist{history_.data(), history_.bytes(), {w, h, 1}, 3};

        const draw::pos_t cx = draw::toSub(w) / 2;
        const draw::pos_t cy = draw::toSub(h) / 2;

        // Feedback pass: for each pixel, find where it came from in the previous frame. The
        // transform is applied INVERSELY (scale down to sample from further out), which is what
        // makes the image appear to grow outward.
        const int32_t s = 65536 - (static_cast<int32_t>(zoom) * 64);       // 16.16 inverse scale
        const angle16 a = static_cast<angle16>(-(static_cast<int32_t>(rotate) * 64));
        const int32_t cosA = static_cast<int32_t>(cos16(a)) - 32768;       // -32768..32767
        const int32_t sinA = static_cast<int32_t>(sin16(a)) - 32768;

        for (lengthType y = 0; y < h; y++) {
            for (lengthType x = 0; x < w; x++) {
                const int32_t px = draw::toSub(x) - cx;
                const int32_t py = draw::toSub(y) - cy;
                // Rotate then scale, all in fixed point.
                const int32_t rx = (px * cosA - py * sinA) >> 15;
                const int32_t ry = (px * sinA + py * cosA) >> 15;
                const draw::pos_t sx = static_cast<draw::pos_t>(((static_cast<int64_t>(rx) * s) >> 16) + cx);
                const draw::pos_t sy = static_cast<draw::pos_t>(((static_cast<int64_t>(ry) * s) >> 16) + cy);

                RGB c = draw::sampleWrap(hist, sx, sy);
                // Fade what came back, so a trail dies out instead of accumulating to white.
                c.r = scale8(c.r, static_cast<uint8_t>(255 - decay));
                c.g = scale8(c.g, static_cast<uint8_t>(255 - decay));
                c.b = scale8(c.b, static_cast<uint8_t>(255 - decay));
                draw::pixel(cv, {x, y, 0}, c);
            }
        }

        // The source: a small bright disc orbiting the centre. Its position is what the echo trail
        // is a record of, so it moves on the beat.
        const int32_t orbit = (w < h ? w : h) * 3 / 8;
        const int32_t ox = w / 2 + ((static_cast<int32_t>(sin16(t)) - 32768) * orbit) / 32768;
        const int32_t oy = h / 2 + ((static_cast<int32_t>(cos16(t)) - 32768) * orbit) / 32768;
        const RGB src = colorFromPalette(*Palettes::active(), static_cast<uint8_t>(t >> 8), 255);
        if (size == 0) {
            draw::pixel(cv, {static_cast<lengthType>(ox), static_cast<lengthType>(oy), 0}, src);
        } else {
            draw::fillCircle(cv, static_cast<lengthType>(ox), static_cast<lengthType>(oy),
                             static_cast<lengthType>(size), src);
        }

        // Keep this frame as the history the next one reads.
        const size_t bytes = static_cast<size_t>(w) * h * 3u;
        for (lengthType y = 0; y < h; y++)
            for (lengthType x = 0; x < w; x++) {
                const RGB c = draw::get(cv, {x, y, 0});
                const size_t i = (static_cast<size_t>(y) * w + x) * 3u;
                if (i + 2 < bytes) { history_[i] = c.r; history_[i + 1] = c.g; history_[i + 2] = c.b; }
            }
    }

private:
    ScratchBuffer<uint8_t> history_{*this};   // the previous frame, read as a texture
    BeatPhase phase_;
};

}  // namespace mm
