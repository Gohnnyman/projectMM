#pragma once

#include "core/VideoService.h"
#include "light/effects/EffectBase.h"

#include <algorithm> // std::max / std::min
#include <cstring>

namespace mm {

// Screen-follow ambient light: paints the layer with the live video frame, so lights around a
// display glow the colour of the picture nearest them (the Ambilight / Hyperion behaviour).
//
// TWO SPACES, and every name below says which one it is in:
//
//   SOURCE       the video frame, counted in PIXELS           frame.width x frame.height
//   DESTINATION  the layer's logical box, counted in          lightsX x lightsY
//                LIGHT POSITIONS
//
// The source is far the bigger — e.g. a 640x480 picture onto a strip of 60 positions — so each light
// position owns a whole rectangle of pixels and shows their average. That rectangle is a Zone.
//
// It fills the box uniformly and never asks which positions actually reach an LED; that is the
// layout's business. On a RectangleLayout the interior maps to nothing, so a border strip shows the
// frame's border for free; on a GridLayout the same effect is a video wall.

/// Effect that paints the layer with the live video frame (screen-follow ambient light).
class AmbilightEffect : public EffectBase {
public:
    Dim dimensions() const override { return Dim::D2; } // a frame is flat; the Layer extrudes z

    uint8_t brightness = 255; // dims THE VIDEO; the driver's brightness dims everything
    uint8_t saturation = 130; // percent of the distance from grey; 100 = the mean untouched
    uint8_t smoothing = 0;    // 0 = follow the frame exactly; higher = slower to move
    uint8_t snapAbove = 80;   // jump rather than smooth when a channel moves further than this; 0 = never
    uint16_t fadeInMs = 0;    // ramp up from black over this long when a picture first arrives; 0 = off
    uint8_t edgeDepth = 0;    // percent of the frame the OUTERMOST positions look in; 0 = their own share

    void defineControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addUint8("saturation", saturation, 0, 200);
        controls_.addUint8("smoothing", smoothing, 0, 255);
        // A cut is a real jump, and smoothing through it reads as the lights lagging the picture.
        controls_.addUint8("snapAbove", snapAbove, 0, 255);
        controls_.setHidden(controls_.count() - 1, smoothing == 0);
        // Its own control because smoothing lags the COLOUR and this ramps the LEVEL.
        controls_.addUint16("fadeInMs", fadeInMs, 0, 10000);
        // How deep the outermost positions look into the picture. 0 = their own share,
        // 50 = the outer half of the picture
        // Hyperion samples ~8%
        controls_.addUint8("edgeDepth", edgeDepth, 0, 50);
    }

    /// Turning smoothing on or off allocates or frees the accumulators, so it has to re-run
    /// prepare() — without this the buffer stays empty and the setting does nothing.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "smoothing") == 0; }

    /// One 8.8 accumulator per channel, allocated only while smoothing is on.
    void prepare() override {
        // lengthType is signed: a stray negative would cast to a colossal size_t, not to nothing.
        const lengthType w = width(), h = height();
        const bool sized = smoothing != 0 && w > 0 && h > 0;
        state_.resize(sized ? static_cast<size_t>(w) * static_cast<size_t>(h) * 3u : 0);
        primed_ = false;
    }

    void tick() MM_NONBLOCKING override {
        const VideoFrame* frame = VideoService::latestFrame();
        const draw::Canvas out = canvas();

        // No source: paint black rather than return, or the PREVIOUS effect's picture stays frozen
        // on the strip. A merely dropped frame never lands here — VideoService keeps its buffer.
        if (!frame->rgb || frame->width == 0 || frame->height == 0) {
            draw::fill(out, {0, 0, 0});
            primed_ = false; // so the next frame lands whole instead of creeping up out of black
            return;
        }

        const lengthType lightsX = width(), lightsY = height();
        if (lightsX <= 0 || lightsY <= 0) return;

        const size_t needed = static_cast<size_t>(lightsX) * static_cast<size_t>(lightsY) * 3u;
        const bool canSmooth = smoothing != 0 && state_ && state_.count() >= needed;

        if (!primed_) fadeStart_ = elapsed(); // a picture arriving after a gap restarts the ramp
        const uint16_t level = fadeLevel();

        // Pixels, not percent, so the divide happens twice a frame rather than twice a position.
        const int deepX = (frame->width * edgeDepth) / 100;
        const int deepY = (frame->height * edgeDepth) / 100;

        for (lengthType y = 0; y < lightsY; y++) {
            const Span rows = spanFor(y, lightsY, frame->height, deepY); // constant down the row
            for (lengthType x = 0; x < lightsX; x++) {
                const Span cols = spanFor(x, lightsX, frame->width, deepX);
                const size_t lightId = static_cast<size_t>(y) * lightsX + x;
                RGB color = adjust(meanOf(*frame, {cols, rows}));
                if (canSmooth) color = smooth(lightId, color);
                if (level != 256) color = dim(color, level);
                draw::pixel(out, {x, y, 0}, color);
            }
        }
        primed_ = true;
    }

private:
    /// Half-open range of SOURCE pixels `[begin, end)` along one axis.
    struct Span {
        int begin, end;
    };

    /// The block of source pixels ONE light position owns, and averages down to its colour.
    struct Zone {
        Span cols, rows;
    };

    /// Which source pixels light position `lightId` covers along one axis.
    /// - `pixels` shared evenly among `lightsSize` positions, cut at the edges so ranges meet exactly
    /// - an empty range widens to one pixel, so a strip finer than the picture still lights up
    /// - a position ON an edge then reaches `deep` pixels in from it, never less than its own share
    /// - `deep` of 0 leaves the plain division; interior positions are on no edge either way
    static Span spanFor(int lightId, int lightsSize, int pixels, int deep) {
        int begin = static_cast<int>((static_cast<long>(lightId) * pixels) / lightsSize);
        int end = static_cast<int>((static_cast<long>(lightId + 1) * pixels) / lightsSize);
        if (end <= begin) end = begin + 1;
        if (end > pixels) end = pixels;
        if (lightId == 0) end = std::max(end, deep);                             // down/right, inward
        if (lightId == lightsSize - 1) begin = std::min(begin, pixels - deep);   // up/left, inward
        return {begin, end};
    }

    /// Mean colour of one zone — the box filter Hyperion uses. uint32 accumulators because
    /// 640x480 into 32x18 is ~520 pixels a zone, and 520 x 255 overflows 16 bits several times.
    static RGB meanOf(const VideoFrame& frame, const Zone& zone) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int py = zone.rows.begin; py < zone.rows.end; py++) {
            const uint8_t* px = frame.rgb + (static_cast<size_t>(py) * frame.width + zone.cols.begin) * 3;
            for (int pxX = zone.cols.begin; pxX < zone.cols.end; pxX++, px += 3) {
                sr += px[0];
                sg += px[1];
                sb += px[2];
            }
        }
        const uint32_t pixels = static_cast<uint32_t>(zone.rows.end - zone.rows.begin) *
                                static_cast<uint32_t>(zone.cols.end - zone.cols.begin);
        return {static_cast<uint8_t>(sr / pixels), static_cast<uint8_t>(sg / pixels),
                static_cast<uint8_t>(sb / pixels)};
    }

    /// Saturation runs on the RAW mean, before brightness: stretching around an already-dimmed luma
    /// would shrink the boost as the lights were turned down.
    RGB adjust(RGB c) const {
        if (saturation != 100) {
            // Rec.601 weights (77/150/29 of 256). A flat (r+g+b)/3 would brighten greens and dim
            // blues as saturation rose, because it is not what the eye does.
            const int luma = static_cast<int>((77 * c.r + 150 * c.g + 29 * c.b) >> 8);
            c = {stretch(c.r, luma), stretch(c.g, luma), stretch(c.b, luma)};
        }
        if (brightness != 255) {
            c = {static_cast<uint8_t>((c.r * brightness) / 255),
                 static_cast<uint8_t>((c.g * brightness) / 255),
                 static_cast<uint8_t>((c.b * brightness) / 255)};
        }
        return c;
    }

    /// How far up the ramp this frame is, 256 (unity) once it is over or when fadeInMs is 0.
    /// Unsigned subtraction, so the millisecond counter wrapping costs one frame at full level.
    uint16_t fadeLevel() const MM_NONBLOCKING {
        if (!fadeInMs) return 256;
        const uint32_t since = elapsed() - fadeStart_;
        return since < fadeInMs ? static_cast<uint16_t>((since * 256u) / fadeInMs) : 256;
    }

    /// Scale by `level`/256, the fade-in envelope.
    static RGB dim(RGB c, uint16_t level) MM_NONBLOCKING {
        return {static_cast<uint8_t>((c.r * level) >> 8), static_cast<uint8_t>((c.g * level) >> 8),
                static_cast<uint8_t>((c.b * level) >> 8)};
    }

    /// Walk each channel a fraction of the way toward `color`. The state is 8.8 so the fraction of
    /// a step survives between frames — in whole bytes a slow setting rounds every step to zero.
    RGB smooth(size_t lightId, RGB color) MM_NONBLOCKING {
        const uint8_t target[3] = {color.r, color.g, color.b};
        const int32_t step = 256 - smoothing;                      // gap closed per frame, of 256
        const int32_t snap = static_cast<int32_t>(snapAbove) << 8; // 8.8, to compare against delta
        uint8_t out[3];
        for (uint8_t ch = 0; ch < 3; ch++) {
            const size_t slot = lightId * 3 + ch; // three accumulators per position, row-major
            const int32_t held = state_[slot];
            const int32_t want = static_cast<int32_t>(target[ch]) << 8;
            const int32_t delta = want - held;
            // The first frame after a gap, and any move big enough to be a cut, land whole.
            const bool jump = !primed_ || (snapAbove && (delta > snap || delta < -snap));
            // >> floors, so without the nudge a rising channel stalls one count short for ever
            // (white would render as 254) while a falling one arrives.
            int32_t move = (delta * step) >> 8;
            if (move == 0 && delta != 0) move = delta > 0 ? 1 : -1;
            state_[slot] = static_cast<uint16_t>(jump ? want : held + move);
            out[ch] = static_cast<uint8_t>(state_[slot] >> 8);
        }
        return {out[0], out[1], out[2]};
    }

    ScratchBuffer<uint16_t> state_{*this}; // 8.8 per channel per light position, while smoothing is on
    bool primed_ = false;                  // false until one frame has been written
    uint32_t fadeStart_ = 0;               // millis() when the current picture first arrived

    /// Move one channel `saturation` percent of the way out from `luma`, clamped to a byte.
    uint8_t stretch(uint8_t v, int luma) const MM_NONBLOCKING {
        const int out = luma + ((static_cast<int>(v) - luma) * static_cast<int>(saturation)) / 100;
        return static_cast<uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    }
};

} // namespace mm
