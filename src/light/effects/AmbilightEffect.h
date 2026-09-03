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
// The source is far the bigger (e.g. a 640x480 picture onto a strip of 60 positions) so each
// light position owns a whole rectangle of pixels and shows their average.
//
// The layout decides the shape: on a RectangleLayout the interior maps to no LED, so a border
// strip shows the frame's border for free; on a GridLayout the same effect is a video wall. The
// effect asks the mapping only ONE question (does this position light anything) and skips the
// averaging where the answer is no. On a border layout that is most of the box.

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
        controls_.addControl("brightness", brightness, 0, 255);
        controls_.addControl("saturation", saturation, 0, 200);
        controls_.addControl("smoothing", smoothing, 0, 255);
        // A cut is a real jump, and smoothing through it reads as the lights lagging the picture.
        controls_.addControl("snapAbove", snapAbove, 0, 255);
        controls_.setHidden(controls_.count() - 1, smoothing == 0);
        // Its own control because smoothing lags the COLOUR and this ramps the LEVEL.
        controls_.addControl("fadeInMs", fadeInMs, 0, 10000);
        controls_.addControl("edgeDepth", edgeDepth, 0, 50); // Hyperion samples ~8%
        // - a letterboxed film puts bars where the top and bottom lights look, so they go dark
        // - edgeDepth cannot help: it widens a zone from the edge, so the bar stays inside it
        // - this moves the zones instead, mapping the lights across the picture it finds
        controls_.addControl("detectBlackBars", detectBlackBars);
        // Raise it if bars are missed, lower it if dark scenes get cropped; the doc page has why.
        controls_.addControl("barLevel", barLevel, 0, 64);
        controls_.setHidden(controls_.count() - 1, !detectBlackBars);
    }

    /// Turning smoothing on or off allocates or frees the accumulators, so it has to re-run
    /// prepare(): without this the buffer stays empty and the setting does nothing.
    bool affectsPrepare(const char* name) const override { return std::strcmp(name, "smoothing") == 0; }

    /// Cold path. applyState() prepares a parent before its children, so the Layer's mapping is
    /// already built when buildLitList() reads it.
    void prepare() override {
        // lengthType is signed: a stray negative would cast to a colossal size_t, not to nothing.
        const lengthType w = width(), h = height();
        const size_t positions = (w > 0 && h > 0) ? static_cast<size_t>(w) * static_cast<size_t>(h) : 0;

        state_.resize(smoothing != 0 ? positions * 3u : 0); // 8.8 per channel, only while smoothing
        primed_ = false;
        buildLitList(positions);
    }

    /// The positions that reach an LED, packed y<<16|x, so tick() walks only those: a few hundred
    /// of tens of thousands on a border layout.
    void buildLitList(size_t positions) {
        litCount_ = 0;
        const MappingLUT& lut = layer()->lut();
        // A table-free (identity) mapping lights every position, so the list would be 0,1,2,3...
        //: 4 bytes a position to say "all of them", where the plain loop needs none.
        allLit_ = !lut.hasLUT();
        if (allLit_ || positions == 0) {
            lit_.resize(0);
            return;
        }
        // Count first, then size to the count: sizing by the box reserves 156 KB on a 200x200
        // rectangle to hold the 3 KB its perimeter needs, the waste this list exists to remove.
        size_t lit = 0;
        for (size_t i = 0; i < positions; i++)
            if (lut.hasDestination(static_cast<nrOfLightsType>(i))) lit++;
        if (lit == 0 || !lit_.resize(lit)) return; // no list: tick() paints the whole box instead
        const lengthType w = width();
        for (size_t i = 0; i < positions; i++)
            if (lut.hasDestination(static_cast<nrOfLightsType>(i)))
                lit_[litCount_++] = static_cast<uint32_t>((i / w) << 16 | (i % w));
    }

    void tick() MM_NONBLOCKING override {
        const VideoFrame* frame = VideoService::latestFrame();
        const draw::Canvas out = canvas();

        // No source: paint black rather than return, or the PREVIOUS effect's picture stays frozen
        // on the strip. A merely dropped frame never lands here: VideoService keeps its buffer.
        if (!frame->rgb || frame->width == 0 || frame->height == 0) {
            draw::fill(out, {0, 0, 0});
            primed_ = false; // so the next frame lands whole instead of creeping up out of black
            return;
        }

        // The frame already on the strip. `primed_` is what makes that true: prepare() clears the
        // layer and resets it, so without it a rebuild against a frozen frame stays black.
        if (primed_ && frame->seq == lastSeq_) return;
        lastSeq_ = frame->seq;

        const lengthType lightsX = width(), lightsY = height();
        if (lightsX <= 0 || lightsY <= 0) return;

        const Region region = regionFor(*frame);
        if (region.width <= 0 || region.height <= 0) return;

        const bool canSmooth = smootherReady(lightsX, lightsY);
        if (!primed_) fadeStart_ = elapsed(); // a picture arriving after a gap restarts the ramp
        const uint16_t level = fadeLevel();

        // Three ways to reach the same set of positions, cheapest first.
        if (allLit_) {
            // Nothing to skip: the plain box, no clear, no mapping queries.
            for (lengthType y = 0; y < lightsY; y++)
                for (lengthType x = 0; x < lightsX; x++)
                    paint(out, *frame, region, x, y, lightsX, lightsY, canSmooth, level);
        } else if (lit_) {
            // The list. Unlit positions are never written, so they keep the black
            // Layer::prepare() left on the rebuild this effect's prepare() rode in on, BlendMap
            // never reads them, but PreviewDriver shows the raw buffer and must not see a ghost.
            for (size_t i = 0; i < litCount_; i++)
                paint(out, *frame, region, static_cast<lengthType>(lit_[i] & 0xFFFF),
                      static_cast<lengthType>(lit_[i] >> 16), lightsX, lightsY, canSmooth, level);
        } else {
            // The list could not be allocated. Same output, asking the mapping per position -
            // which is the cost the list exists to avoid.
            const MappingLUT& lut = layer()->lut();
            draw::fill(out, {0, 0, 0});
            for (lengthType y = 0; y < lightsY; y++)
                for (lengthType x = 0; x < lightsX; x++)
                    if (lut.hasDestination(static_cast<nrOfLightsType>(y * lightsX + x)))
                        paint(out, *frame, region, x, y, lightsX, lightsY, canSmooth, level);
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

        /// Which source pixels one light position covers: its share of the picture, shifted back
        /// into frame coordinates. Every input lives here, so the loop only asks.
        Span cols(int x, int lightsX) const { return spanFor(x, lightsX, width, deepX).shifted(left); }
        Span rows(int y, int lightsY) const { return spanFor(y, lightsY, height, deepY).shifted(top); }
    };

    /// One light position: average its pixels, correct, smooth, level, write.
    void paint(const draw::Canvas& out, const VideoFrame& frame, const Region& region, lengthType x,
               lengthType y, lengthType lightsX, lengthType lightsY, bool canSmooth,
               uint16_t level) MM_NONBLOCKING {
        RGB color = adjust(meanOf(frame, region.cols(x, lightsX), region.rows(y, lightsY)));
        if (canSmooth) color = smooth(static_cast<size_t>(y) * lightsX + x, color);
        if (level != 256) color = dim(color, level);
        draw::pixel(out, {x, y, 0}, color);
    }

    Region regionFor(const VideoFrame& frame) MM_NONBLOCKING {
        const Bars bars = trackBars(frame);
        Region r;
        r.left = bars.left;
        r.top = bars.top;
        r.width = frame.width - bars.left - bars.right;
        r.height = frame.height - bars.top - bars.bottom;
        // Rounded UP, so any non-zero percentage is at least one pixel. Flooring would let a small
        // setting on a small frame land on 0, which is the off value: the control would go quiet.
        r.deepX = (r.width * edgeDepth + 99) / 100;
        r.deepY = (r.height * edgeDepth + 99) / 100;
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
    /// every pixel: a bar is uniform, so a handful of probes settles it for a fraction of the cost.
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

    /// Scan this frame and return the bars IN EFFECT, which is not necessarily what was just
    /// seen. A reading is adopted only once kStableFrames of them agree: bars come and go at scene
    /// changes, and a mapping that follows every dark frame twitches worse than one that ignores
    /// them. Hence the state; the return value is what the caller should actually map across.
    Bars trackBars(const VideoFrame& frame) MM_NONBLOCKING {
        if (!detectBlackBars) {
            // All of it, not just the adopted value: a surviving candidate_ with a saturated
            // stable_ makes the next enable agree with itself immediately and never re-adopt, so
            // the setting would look dead until the picture's geometry changed.
            bars_ = candidate_ = Bars{};
            stable_ = 0;
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
    /// - a position ON an edge takes exactly `deep` instead of its share: deeper OR shallower, so
    ///   the control sets the depth rather than raising a floor under it
    /// - `deep` of 0 leaves the plain division; interior positions are on no edge either way
    /// - an empty range widens to one pixel, so a strip finer than the picture still lights up
    static Span spanFor(int lightId, int lightsSize, int pixels, int deep) {
        int begin = static_cast<int>((static_cast<long>(lightId) * pixels) / lightsSize);
        int end = static_cast<int>((static_cast<long>(lightId + 1) * pixels) / lightsSize);
        if (deep > 0) {
            if (lightId == 0)
                end = deep; // down/right, inward
            else if (lightId == lightsSize - 1)
                begin = pixels - deep; // up/left, inward
        }
        if (begin < 0) begin = 0;
        if (begin >= pixels) begin = pixels - 1;
        if (end <= begin) end = begin + 1;
        if (end > pixels) end = pixels;
        return {begin, end};
    }

    /// Mean of one light position's pixels: the box filter Hyperion uses. uint32 accumulators
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
        const uint32_t pixels =
            static_cast<uint32_t>(rows.end - rows.begin) * static_cast<uint32_t>(cols.end - cols.begin);
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
    /// a step survives between frames: in whole bytes a slow setting rounds every step to zero.
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

    ScratchBuffer<uint32_t> lit_{*this}; // packed y<<16|x, the positions that reach an LED
    size_t litCount_ = 0;
    bool allLit_ = true; // no list: the mapping lights every position

    ScratchBuffer<uint16_t> state_{*this}; // 8.8 per channel per light position, while smoothing is on
    bool primed_ = false;                  // false until one frame has been written
    uint32_t lastSeq_ = 0;                 // the frame already on the strip
    uint32_t fadeStart_ = 0;               // millis() when the current picture first arrived
};

} // namespace mm
