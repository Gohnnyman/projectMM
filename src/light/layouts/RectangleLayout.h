#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

// A hollow rectangle: lights around the PERIMETER of a `width` x `height` box, nothing inside it.
// The strip-around-a-frame primitive — a TV backlight, a mirror surround, a sign border.
//
// - Each corner counts once, so the count is `2(width + height) - 4`. A strip bent around a frame
//   has one LED in the corner, even though that corner belongs to two edges.
// - `startCorner` and `clockwise` change the WIRING, not the shape: they rotate and reverse the
//   index order while every emitted coordinate stays identical.
// - Perimeter only. A filled rectangle is already GridLayout; this exists for the case where the
//   interior has no LEDs in it at all, which is every frame-mounted strip.
/// Layout of lights around the perimeter of a rectangle (hollow border).
class RectangleLayout : public LayoutBase {
public:
    uint16_t width = 32; // extent in LIGHTS along each edge; 32x18 is 16:9
    uint16_t height = 18;
    uint8_t startCorner = 0; // index into kStartCornerOptions
    bool clockwise = true;

    static constexpr const char* kStartCornerOptions[] = {"top-left", "top-right", "bottom-right",
                                                          "bottom-left"};
    static constexpr uint8_t kStartCornerCount = sizeof(kStartCornerOptions) / sizeof(kStartCornerOptions[0]);

    void defineControls() override {
        controls_.addUint16("width", width, 1, 500);
        controls_.addUint16("height", height, 1, 500);
        controls_.addSelect("startCorner", startCorner, kStartCornerOptions, kStartCornerCount);
        controls_.addBool("clockwise", clockwise);
    }

    nrOfLightsType lightCount() const override { return perimeter(); }

    void placeLights(const CoordSink& sink) const override {
        const nrOfLightsType n = perimeter();
        for (nrOfLightsType i = 0; i < n; i++) {
            const Coord3D c = coordAt(i, n);
            sink.pixel(i, c.x, c.y, c.z);
        }
    }

private:
    /// Perimeter cell count. the -4 is the four corners, each belonging to two edges. A box one
    /// light thick has no interior to go around, so it degenerates to a line — the rectangle
    /// formula would walk those cells twice and light phantom positions.
    nrOfLightsType perimeter() const {
        if (width == 0 || height == 0) return 0;
        if (height == 1) return width;
        if (width == 1) return height;
        return static_cast<nrOfLightsType>(2 * width + 2 * height - 4);
    }

    /// Coordinate of physical light `i` of `n`.
    ///
    /// Four segments, each dropping the corner the previous one emitted:
    ///     top     left to right    w   cells
    ///     right   top to bottom    h-1 cells
    ///     bottom  right to left    w-1 cells
    ///     left    bottom to top    h-2 cells   (both corners already placed)
    Coord3D coordAt(nrOfLightsType i, nrOfLightsType n) const {

        const int w = width, h = height, k = static_cast<int>(walkIndex(i, n));

        const auto at = [](int x, int y) {
            return Coord3D{static_cast<lengthType>(x), static_cast<lengthType>(y), 0};
        };

        if (h == 1) return at(k, 0);
        if (w == 1) return at(0, k);

        const int topEnd = w;                // steps [0, topEnd)           top edge
        const int rightEnd = w + h - 1;      //       [topEnd, rightEnd)    right edge
        const int bottomEnd = 2 * w + h - 2; //       [rightEnd, bottomEnd) bottom edge

        if (k < topEnd) return at(k, 0);
        if (k < rightEnd) return at(w - 1, k - topEnd + 1);
        if (k < bottomEnd) return at(bottomEnd - 1 - k, h - 1);
        return at(0, static_cast<int>(perimeter()) - k); // left edge, walking back up
    }

    /// Step at which each start corner sits on the reference walk — its segment boundaries, so a
    /// corner resolves to an exact index rather than a search.
    nrOfLightsType startIndex() const {
        const int w = width, h = height;
        switch (startCorner) {
        case 1: return static_cast<nrOfLightsType>(w - 1);         // top-right
        case 2: return static_cast<nrOfLightsType>(w + h - 2);     // bottom-right
        case 3: return static_cast<nrOfLightsType>(2 * w + h - 3); // bottom-left
        default: return 0;                                         // top-left
        }
    }

    /// Indexing lights starts from top-left, then walks the perimeter clockwise
    /// But if `startCorner` or `clockwise` changed, the indexing also changes (direction, starting point)
    /// This function adjusts index to the canonical clockwise top-left walk
    nrOfLightsType walkIndex(nrOfLightsType i, nrOfLightsType n) const {
        if (n == 0) return 0;
        const nrOfLightsType s = static_cast<nrOfLightsType>(startIndex() % n);
        return clockwise ? static_cast<nrOfLightsType>((s + i) % n)
                         : static_cast<nrOfLightsType>((s + n - (i % n)) % n);
    }
};

} // namespace mm
