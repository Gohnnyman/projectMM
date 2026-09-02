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
// The source is far the bigger — e.g. a 640x480 picture onto a strip of 60 positions — so each
// light position owns a whole rectangle of pixels and shows their average.
//
// It fills the box uniformly and never asks which positions actually reach an LED; that is the
// layout's business. On a RectangleLayout the interior maps to nothing, so a border strip shows the
// frame's border for free; on a GridLayout the same effect is a video wall.

/// Effect that paints the layer with the live video frame (screen-follow ambient light).
class AmbilightEffect : public EffectBase {
public:
    Dim dimensions() const override { return Dim::D2; } // a frame is flat; the Layer extrudes z

    uint8_t brightness = 255;     // dims THE VIDEO; the driver's brightness dims everything
    uint8_t saturation = 130;     // percent of the distance from grey; 100 = the mean untouched
    uint8_t smoothing = 0;        // 0 = follow the frame exactly; higher = slower to move
    uint8_t snapAbove = 80;       // jump rather than smooth when a channel moves further than this; 0 = never
    uint16_t fadeInMs = 0;        // ramp up from black over this long when a picture first arrives; 0 = off
    uint8_t edgeDepth = 0;        // percent of the frame the OUTERMOST positions look in; 0 = their own share
    bool detectBlackBars = false; // find the letterbox and map the lights across the picture
    uint8_t barLevel = 12;        // a channel at or below this counts as bar; ~5%, for compression noise

    void defineControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addUint8("saturation", saturation, 0, 200);
        controls_.addUint8("smoothing", smoothing, 0, 255);
        // A cut is a real jump, and smoothing through it reads as the lights lagging the picture.
        controls_.addUint8("snapAbove", snapAbove, 0, 255);
        controls_.setHidden(controls_.count() - 1, smoothing == 0);
        // Its own control because smoothing lags the COLOUR and this ramps the LEVEL.
        controls_.addUint16("fadeInMs", fadeInMs, 0, 10000);
        controls_.addUint8("edgeDepth", edgeDepth, 0, 50); // Hyperion samples ~8%
        // - a letterboxed film puts bars where the top and bottom lights look, so they go dark
        // - edgeDepth cannot help: it widens a zone from the edge, so the bar stays inside it
        // - this moves the zones instead, mapping the lights across the picture it finds
        controls_.addBool("detectBlackBars", detectBlackBars);
        // Raise it if bars are missed, lower it if dark scenes get cropped; the doc page has why.
        controls_.addUint8("barLevel", barLevel, 0, 64);
        controls_.setHidden(controls_.count() - 1, !detectBlackBars);
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

        const Region region = regionFor(*frame);
        if (region.width <= 0 || region.height <= 0) return;

        const bool canSmooth = smootherReady(lightsX, lightsY);
        if (!primed_) fadeStart_ = elapsed(); // a picture arriving after a gap restarts the ramp
        const uint16_t level = fadeLevel();

        for (lengthType y = 0; y < lightsY; y++) {
            const Span rows = region.rows(y, lightsY); // constant down the row
            for (lengthType x = 0; x < lightsX; x++) {
                const Span cols = region.cols(x, lightsX);
                const size_t lightId = static_cast<size_t>(y) * lightsX + x;
                RGB color = adjust(meanOf(*frame, cols, rows));
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
        Span shifted(int by) const { return {begin + by, end + by}; }
    };

    // The actual region of the source frame that the lights cover.
    // Could be smaller than the full frame if black bars are detected.
    struct Region {
        int left = 0, top = 0; // where the picture starts inside the frame
        int width = 0, height = 0;
        int deepX = 0, deepY = 0; // edgeDepth in pixels, so the divide is not per position

        /// Which source pixels one light position covers — its share of the picture, shifted back
        /// into frame coordinates. Every input lives here, so the loop only asks.
        Span cols(int x, int lightsX) const { return spanFor(x, lightsX, width, deepX).shifted(left); }
        Span rows(int y, int lightsY) const { return spanFor(y, lightsY, height, deepY).shifted(top); }
    };

    Region regionFor(const VideoFrame& frame) MM_NONBLOCKING {
        const Bars bars = trackBars(frame);
        Region r;
        r.left = bars.left;
        r.top = bars.top;
        r.width = frame.width - bars.left - bars.right;
        r.height = frame.height - bars.top - bars.bottom;
        r.deepX = (r.width * edgeDepth) / 100;
        r.deepY = (r.height * edgeDepth) / 100;
        return r;
    }

    /// Whether this frame can be smoothed: turned on, and the accumulators are there and big enough.
    bool smootherReady(lengthType lightsX, lengthType lightsY) const MM_NONBLOCKING {
        return smoothing != 0 && state_ &&
               state_.count() >= static_cast<size_t>(lightsX) * static_cast<size_t>(lightsY) * 3u;
    }

    /// How thick the bar at each edge is, in pixels
    struct Bars {
        int top = 0, bottom = 0, left = 0, right = 0;
        bool operator==(const Bars& o) const {
            return top == o.top && bottom == o.bottom && left == o.left && right == o.right;
        }
    };

    static constexpr int kProbes = 8;         // sample points per scanned line
    static constexpr int kMaxBarPercent = 40; // a "bar" deeper than this is a dark scene
    static constexpr uint8_t kStableFrames = 30;

    /// Which edge a bar is being measured from. Top and Bottom scan rows, Left and Right columns.
    enum class Edge : uint8_t { Top, Bottom, Left, Right };

    static bool scansRows(Edge e) MM_NONBLOCKING { return e == Edge::Top || e == Edge::Bottom; }
    static bool scansFromEnd(Edge e) MM_NONBLOCKING { return e == Edge::Bottom || e == Edge::Right; }

    /// Is this line dark all the way across? Sampled at a few evenly spaced points rather than
    /// every pixel — a bar is uniform, so a handful of probes settles it for a fraction of the cost.
    bool lineIsDark(const VideoFrame& frame, int line, Edge edge) const MM_NONBLOCKING {
        const bool horizontal = scansRows(edge);
        const int along = horizontal ? frame.width : frame.height;
        for (int i = 0; i < kProbes; i++) {
            const int v = along * (2 * i + 1) / (2 * kProbes); // midpoints, so the corners are skipped
            const int x = horizontal ? v : line;
            const int y = horizontal ? line : v;
            const uint8_t* px = frame.rgb + (static_cast<size_t>(y) * frame.width + x) * 3;
            if (px[0] > barLevel || px[1] > barLevel || px[2] > barLevel) return false;
        }
        return true;
    }

    /// How many dark lines run inward from one edge, capped so a dark SCENE cannot be mistaken for
    /// a bar and blank the strip.
    int barFrom(const VideoFrame& frame, Edge edge) const MM_NONBLOCKING {
        const int extent = scansRows(edge) ? frame.height : frame.width;
        const int limit = extent * kMaxBarPercent / 100;
        for (int i = 0; i < limit; i++) {
            const int line = scansFromEnd(edge) ? extent - 1 - i : i;
            if (!lineIsDark(frame, line, edge)) return i;
        }
        return limit;
    }

    /// Scan this frame and return the bars IN EFFECT — which is not necessarily what was just
    /// seen. A reading is adopted only once kStableFrames of them agree: bars come and go at scene
    /// changes, and a mapping that follows every dark frame twitches worse than one that ignores
    /// them. Hence the state; the return value is what the caller should actually map across.
    Bars trackBars(const VideoFrame& frame) MM_NONBLOCKING {
        if (!detectBlackBars) {
            bars_ = Bars{};
            return bars_;
        }
        Bars found;
        found.top = barFrom(frame, Edge::Top);
        found.bottom = barFrom(frame, Edge::Bottom);
        found.left = barFrom(frame, Edge::Left);
        found.right = barFrom(frame, Edge::Right);
        if (!(found == candidate_)) {
            candidate_ = found;
            stable_ = 0;
        } else if (stable_ < kStableFrames && ++stable_ == kStableFrames) {
            bars_ = found;
        }
        return bars_;
    }

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
        if (lightId == 0) end = std::max(end, deep);                           // down/right, inward
        if (lightId == lightsSize - 1) begin = std::min(begin, pixels - deep); // up/left, inward
        return {begin, end};
    }

    /// Mean of one light position's pixels — the box filter Hyperion uses. uint32 accumulators
    /// because 640x480 onto 32x18 is ~520 pixels each, and 520 x 255 overflows 16 bits several times.
    static RGB meanOf(const VideoFrame& frame, Span cols, Span rows) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int py = rows.begin; py < rows.end; py++) {
            const uint8_t* px = frame.rgb + (static_cast<size_t>(py) * frame.width + cols.begin) * 3;
            for (int pxX = cols.begin; pxX < cols.end; pxX++, px += 3) {
                sr += px[0];
                sg += px[1];
                sb += px[2];
            }
        }
        const uint32_t pixels = static_cast<uint32_t>(rows.end - rows.begin) *
                                static_cast<uint32_t>(cols.end - cols.begin);
        return {static_cast<uint8_t>(sr / pixels), static_cast<uint8_t>(sg / pixels),
                static_cast<uint8_t>(sb / pixels)};
    }

    /// Move one channel `saturation` percent of the way out from `luma`, clamped to a byte.
    uint8_t stretch(uint8_t v, int luma) const MM_NONBLOCKING {
        const int out = luma + ((static_cast<int>(v) - luma) * static_cast<int>(saturation)) / 100;
        return static_cast<uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
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

    Bars bars_;      // in effect
    Bars candidate_; // seen most recently
    uint8_t stable_ = 0;

    ScratchBuffer<uint16_t> state_{*this}; // 8.8 per channel per light position, while smoothing is on
    bool primed_ = false;                  // false until one frame has been written
    uint32_t fadeStart_ = 0;               // millis() when the current picture first arrived
};

} // namespace mm
