#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

// A hollow rectangle: lights around the PERIMETER of a `width` x `height` box, nothing inside it.
// The strip-around-a-frame primitive — a TV backlight, a mirror surround, a sign border.
//
// - Each corner counts once by default, so the count is `2(width + height) - 4`: a strip bent
//   around a frame has ONE LED in the corner, even though that corner belongs to two edges. Four
//   separate strips instead have their own end there — `sharedCorners` off gives `2(width+height)`,
//   with two lights on each corner coordinate.
// - `offset` slides the wiring around the perimeter, for a strip that starts partway along an edge
//   rather than at a corner.
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
    bool sharedCorners = true; // one light per corner; off = four strips, each with its own end
    uint16_t offset = 0;       // lights past startCorner where the strip actually begins

    static constexpr const char* kStartCornerOptions[] = {"top-left", "top-right", "bottom-right",
                                                          "bottom-left"};
    static constexpr uint8_t kStartCornerCount = sizeof(kStartCornerOptions) / sizeof(kStartCornerOptions[0]);

    void defineControls() override {
        controls_.addUint16("width", width, 1, 500);
        controls_.addUint16("height", height, 1, 500);
        controls_.addSelect("startCorner", startCorner, kStartCornerOptions, kStartCornerCount);
        controls_.addBool("clockwise", clockwise);
        // Off when the corners are four separate strip ends rather than one bent light.
        controls_.addBool("sharedCorners", sharedCorners);
        // For a strip that starts partway along an edge instead of at the corner.
        controls_.addUint16("offset", offset, 0, 1999);
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
        // Unshared corners give each edge its full length, so the four edges just sum.
        return static_cast<nrOfLightsType>(sharedCorners ? 2 * width + 2 * height - 4
                                                         : 2 * width + 2 * height);
    }

    /// Coordinate of physical light `i` of `n`.
    ///
    /// Four segments, each dropping the corner the previous one emitted:
    ///     top     left to right    w   cells
    ///     right   top to bottom    h-1 cells   (h with unshared corners)
    ///     bottom  right to left    w-1 cells   (w  "")
    ///     left    bottom to top    h-2 cells   (h  "")
    ///
    /// Unshared, each edge keeps its own corner: the right edge starts AT the top-right rather than
    /// below it, so two lights land on each corner coordinate — four strip ends meeting there.
    Coord3D coordAt(nrOfLightsType i, nrOfLightsType n) const {

        const int w = width, h = height, k = static_cast<int>(walkIndex(i, n));
        const int drop = sharedCorners ? 1 : 0; // cells each edge gives up to the one before it

        const auto at = [](int x, int y) {
            return Coord3D{static_cast<lengthType>(x), static_cast<lengthType>(y), 0};
        };

        if (h == 1) return at(k, 0);
        if (w == 1) return at(0, k);

        const int topEnd = w;                            // steps [0, topEnd)           top edge
        const int rightEnd = topEnd + h - drop;          //       [topEnd, rightEnd)     right edge
        const int bottomEnd = rightEnd + w - drop;       //       [rightEnd, bottomEnd)  bottom edge

        if (k < topEnd) return at(k, 0);                                    // x rises, y = 0
        if (k < rightEnd) return at(w - 1, k - topEnd + drop);             // y rises, x = w-1
        if (k < bottomEnd) return at(w - 1 - drop - (k - rightEnd), h - 1); // x falls, y = h-1
        return at(0, h - 1 - (k - bottomEnd) - drop);                      // y falls, x = 0
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

    /// Indexing lights starts from top-left, then walks the perimeter clockwise. `startCorner`,
    /// `offset` and `clockwise` move where index 0 sits and which way it runs; this maps a driver
    /// index back onto the canonical walk.
    nrOfLightsType walkIndex(nrOfLightsType i, nrOfLightsType n) const {
        if (n == 0) return 0;
        const nrOfLightsType s = static_cast<nrOfLightsType>((startIndex() + offset) % n);
        return clockwise ? static_cast<nrOfLightsType>((s + i) % n)
                         : static_cast<nrOfLightsType>((s + n - (i % n)) % n);
    }
};

} // namespace mm
