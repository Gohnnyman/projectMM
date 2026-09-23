#pragma once

#include "light/layouts/LayoutBase.h"

namespace mm {

/// Layout of a dense 3D grid with mid-strand dark columns (a spacer).
///
/// @moreinfo
///
/// A dense 3D grid with mid-strand DARK COLUMNS: the columns [blackStart, blackStart+blackCount) are held black in every row.
/// A dark column is a gap: a physical wire slot the driver still clocks, so data flows through the unlit lights to reach the lit columns beyond.
/// It maps to no logical light, so it stays black.
/// This is for a sealed panel with a dark spacer strip, or a slat wall, where the strip cannot be cut.
/// The effect maps across the gap unshifted, so the picture is holed at its true coordinates rather than squeezed.
/// It is the plain [Grid](GridLayout.md) with one added capability; a grid without dark columns is just a Grid, so pick that.
///
/// The gap decision is made once at the emit site, on the true column, so a dark column stays dark whichever way a serpentine strip snakes into the row.
/// A non-empty run tells the Layer to build the folded mapping, which drops the gap slots, rather than the dense identity map, which would light them.
/// See CoordSink for the two-kinds-of-pixel model.
class GridBlacksLayout : public LayoutBase {
public:
    /// The catalog tags this layout carries.
    const char* tags() const override { return "💫"; }
    /// How many axes this layout places lights on.
    Dim dimensions() const override { return Dim::D3; }
    /// The grid's extent on x.
    lengthType width = 16;
    /// Its extent on y.
    lengthType height = 16;
    /// Its extent on z.
    lengthType depth = 1;
    bool serpentine = false;   // odd rows wired in reverse (boustrophedon) — the snaked-strip matrix.
    /// The first dark column.
    lengthType blackStart = 0;
    lengthType blackCount = 0; // number of dark columns; 0 = no gap (renders like a plain Grid)

    /// The controls a user sets on the card.
    void defineControls() override {
        controls_.addControl("width",  width,  1, 512);
        controls_.addControl("height", height, 1, 512);
        controls_.addControl("depth",  depth,  1, 512);
        controls_.addControl("serpentine", serpentine);
        controls_.addControl("blackCount", blackCount, 0, 512);
        controls_.addControl("blackStart", blackStart, 0, 512);
        controls_.setHidden(controls_.count() - 1, blackCount == 0);   // blackStart matters only with a run
    }

    /// How many lights the current settings place.
    nrOfLightsType lightCount() const override {
        /// A gap is a physical wire slot the driver clocks, so it counts toward the box.
        uint32_t n = static_cast<uint32_t>(width) * height * depth;
        constexpr uint32_t kMax = std::numeric_limits<nrOfLightsType>::max();
        return static_cast<nrOfLightsType>(n > kMax ? kMax : n);
    }

    /// Whether dark columns exist, which is what makes the Layer fold the mapping.
    bool hasBlackPixels() const override { return blackCount != 0; }

    /// Emit every light's coordinate, in wiring order.
    void placeLights(const CoordSink& sink) const override {
        // A wide index, and the clamped count as the bound, so no emit leaves the buffer.
        const uint32_t limit = lightCount();
        const lengthType blackEnd = static_cast<lengthType>(blackStart + blackCount);   // exclusive
        uint32_t idx = 0;
        for (lengthType z = 0; z < depth && idx < limit; z++) {
            for (lengthType y = 0; y < height && idx < limit; y++) {
                // Serpentine changes only the index-to-position order, never the coordinate.
                const bool reverse = serpentine && (y & 1);
                for (lengthType i = 0; i < width && idx < limit; i++) {
                    const lengthType x = reverse ? static_cast<lengthType>(width - 1 - i) : i;
                    // Decided here at the emit site, so it is never re-derived from an index.
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
