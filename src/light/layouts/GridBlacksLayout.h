#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

// A dense 3D grid with mid-strand DARK COLUMNS: the columns [blackStart, blackStart+blackCount) are
// held black in every row. A dark column is a GAP — a physical wire slot the driver still clocks (so
// WS2812 data flows THROUGH the unlit LEDs to reach the lit columns beyond) that maps to no logical
// light, so it stays black. This is for a sealed/continuous panel with a dark spacer strip, or a slat
// wall, where the strip cannot be cut and the effect must map across the gap unshifted (the picture is
// HOLED at its true coordinates, not squeezed). It is the plain [Grid](GridLayout.md) with one added
// capability; a grid without dark columns is just a Grid, so pick that.
//
// The gap decision is made once, at the emit site (sink.blackPixel vs sink.pixel), on the TRUE column
// x — so a dark column stays dark whichever way a serpentine strip snakes into the row. hasBlackPixels
// tells the Layer to build the folded LUT (which drops the gap slots) instead of the dense identity map
// (which would light them). See CoordSink for the two-kinds-of-pixel model.
/// Layout of a dense 3D grid with mid-strand dark columns (a spacer).
class GridBlacksLayout : public LayoutBase {
public:
    lengthType width = 16;
    lengthType height = 16;
    lengthType depth = 1;
    bool serpentine = false;   // odd rows wired in reverse (boustrophedon) — the snaked-strip matrix.
    lengthType blackStart = 0; // first dark column
    lengthType blackCount = 0; // number of dark columns; 0 = no gap (renders like a plain Grid)

    void defineControls() override {
        controls_.addInt16("width",  width,  1, 512);
        controls_.addInt16("height", height, 1, 512);
        controls_.addInt16("depth",  depth,  1, 512);
        controls_.addBool("serpentine", serpentine);
        controls_.addInt16("blackCount", blackCount, 0, 512);
        controls_.addInt16("blackStart", blackStart, 0, 512);
        controls_.setHidden(controls_.count() - 1, blackCount == 0);   // blackStart matters only with a run
    }

    nrOfLightsType lightCount() const override {
        // Multiply in uint32_t to detect overflow before casting. A gap is a PHYSICAL pixel (a wire slot
        // the driver clocks), so it counts here — dark and lit cells alike fill the box.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    // A non-empty run means dark columns exist, so the Layer must fold the LUT (dropping the gap slots)
    // rather than take the identity fast path (which would light them). No run → renders like a Grid.
    bool hasBlackPixels() const override { return blackCount != 0; }

    void placeLights(const CoordSink& sink) const override {
        // uint32_t idx so it never wraps on uint16_t nrOfLightsType (no-PSRAM 512×512 > 65535); stop at
        // the clamped lightCount() so emitted indices stay within the allocated buffer.
        const uint32_t limit = lightCount();
        const lengthType blackEnd = static_cast<lengthType>(blackStart + blackCount);   // exclusive
        uint32_t idx = 0;
        for (lengthType z = 0; z < depth && idx < limit; z++) {
            for (lengthType y = 0; y < height && idx < limit; y++) {
                // Serpentine reverses the wire order on odd rows; the emitted COORDINATE is still the
                // true (x,y,z) — only the index→position order changes.
                const bool reverse = serpentine && (y & 1);
                for (lengthType i = 0; i < width && idx < limit; i++) {
                    const lengthType x = reverse ? static_cast<lengthType>(width - 1 - i) : i;
                    // Gap test on the true x (the physical column), decided HERE at the emit site so it
                    // is never re-derived from the index elsewhere.
                    if (blackCount != 0 && x >= blackStart && x < blackEnd)
                        sink.blackPixel(static_cast<nrOfLightsType>(idx++), x, y, z);
                    else
                        sink.pixel(static_cast<nrOfLightsType>(idx++), x, y, z);
                }
            }
        }
    }
};

} // namespace mm
