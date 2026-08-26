#pragma once

#include "core/VideoService.h"
#include "light/effects/EffectBase.h"

namespace mm {

// Screen-follow ambient light: paints the layer with the live video frame, so lights around a
// display glow the colour of the picture nearest them (the Ambilight / Hyperion behaviour).
//
// - Reads its pixels instead of generating them. Pulled from VideoService::latestFrame()
//
// - Averages a rectangle per output cell
//
// - Fills the whole logical box uniformly and never asks which cells reach an LED — that is the
//   layout's business. On a RectangleLayout the interior maps to nothing, so a border strip shows
//   the frame's border for free; on a GridLayout the same effect is a video wall.

/// Effect that paints the layer with the live video frame (screen-follow ambient light).
class AmbilightEffect : public EffectBase {
public:
    Dim dimensions() const override { return Dim::D2; } // a frame is flat; the Layer extrudes z

    uint8_t brightness = 255; // dims THE VIDEO; the driver's brightness dims everything
    uint8_t saturation = 130; // percent of the distance from grey; 100 = the mean untouched

    void defineControls() override {
        controls_.addUint8("brightness", brightness, 0, 255);
        controls_.addUint8("saturation", saturation, 0, 200);
    }

    void tick() MM_NONBLOCKING override {
        const VideoFrame* f = VideoService::latestFrame();
        const draw::Canvas cv = canvas();

        // No source: paint black rather than return.
        // Returning would leave the PREVIOUS effect's picture frozen on the strip
        // A dropped frame never reaches here: VideoService keeps its buffer, so `rgb` stays readable.
        if (!f->rgb || f->width == 0 || f->height == 0) {
            draw::fill(cv, {0, 0, 0});
            return;
        }
        const lengthType dstWidth = width(), dstHeight = height();
        if (dstWidth <= 0 || dstHeight <= 0) return;

        // Meaning each zone in one destination Cell
        for (lengthType y = 0; y < dstHeight; y++) {
            for (lengthType x = 0; x < dstWidth; x++) {
                const Zone z = zoneFor(x, y, dstWidth, dstHeight, *f);
                draw::pixel(cv, {x, y, 0}, adjust(meanOf(*f, z)));
            }
        }
    }

private:
    /// Half-open range of source pixels `[begin, end)` along one axis.
    struct Span {
        int begin, end;
    };

    /// The source rectangle one logical cell owns, and averages down to its colour.
    struct Zone {
        Span cols, rows;
    };

    /// Split one axis of `srcLen` source pixels across `cells` cells.
    /// - From the cell EDGES, so consecutive spans meet exactly: every source pixel belongs to one
    ///   cell, none to two, none to nothing.
    /// - An empty span widens to one shared pixel, so a layer finer than the source still writes
    ///   every light instead of leaving some unset.
    static Span spanFor(int index, int dstLen, int srcLen) {
        const int begin = static_cast<int>((static_cast<long>(index) * srcLen) / dstLen);
        int end = static_cast<int>((static_cast<long>(index + 1) * srcLen) / dstLen);
        if (end <= begin) end = begin + 1;
        return {begin, end < srcLen ? end : srcLen};
    }

    static Zone zoneFor(int x, int y, int w, int h, const VideoFrame& f) {
        return {spanFor(x, w, f.width), spanFor(y, h, f.height)};
    }

    /// Mean colour of one zone — the box filter, the same per-zone computation Hyperion performs.
    /// uint32 accumulators: 640x480 into 32x18 is ~520 pixels per zone, and 520 x 255 overflows 16
    /// bits several times over.
    static RGB meanOf(const VideoFrame& f, const Zone& z) {
        uint32_t sr = 0, sg = 0, sb = 0;
        for (int y = z.rows.begin; y < z.rows.end; y++) {
            const uint8_t* px = f.rgb + (static_cast<size_t>(y) * f.width + z.cols.begin) * 3;
            for (int x = z.cols.begin; x < z.cols.end; x++, px += 3) {
                sr += px[0];
                sg += px[1];
                sb += px[2];
            }
        }
        const uint32_t n = static_cast<uint32_t>(z.rows.end - z.rows.begin) *
                           static_cast<uint32_t>(z.cols.end - z.cols.begin);
        return {static_cast<uint8_t>(sr / n), static_cast<uint8_t>(sg / n), static_cast<uint8_t>(sb / n)};
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

    /// Move one channel `saturation` percent of the way out from `luma`, clamped to a byte.
    uint8_t stretch(uint8_t v, int luma) const MM_NONBLOCKING {
        const int out = luma + ((static_cast<int>(v) - luma) * static_cast<int>(saturation)) / 100;
        return static_cast<uint8_t>(out < 0 ? 0 : (out > 255 ? 255 : out));
    }
};

} // namespace mm
