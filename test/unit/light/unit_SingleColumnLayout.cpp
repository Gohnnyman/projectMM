// @module SingleColumnLayout
// @also GridLayout

// Pins the vertical-column layout: index order, spatial offset, and reversed wiring.
//
// The cross-check against GridLayout is the load-bearing one. A 1-wide, N-high grid and an N-high
// column describe the SAME strip, so the two must emit identical coordinates; anything else means
// one of them is wrong, and only a direct comparison catches a layout that is self-consistently
// wrong. GridLayout had a test and this had none, which is why the pair is asserted here rather
// than each in isolation.

#include "doctest.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/SingleColumnLayout.h"

#include <vector>

namespace {

struct CoordEntry {
    mm::nrOfLightsType idx;
    mm::lengthType x, y, z;
};

void collectCoord(void* ctx, mm::nrOfLightsType idx, mm::lengthType x, mm::lengthType y, mm::lengthType z) {
    static_cast<std::vector<CoordEntry>*>(ctx)->push_back({idx, x, y, z});
}

std::vector<CoordEntry> coordsOf(const mm::LayoutBase& layout) {
    std::vector<CoordEntry> out;
    layout.forEachCoord(mm::CoordSink{collectCoord, nullptr, &out});
    return out;
}

} // namespace

// Indices are contiguous 0..N-1 and every light sits at the configured x — a gap or a repeat here
// is a light the driver would never write, so the strip would have a permanently dark pixel.
TEST_CASE("SingleColumnLayout of height 10 emits ten consecutively indexed lights") {
    mm::SingleColumnLayout column;
    column.height = 10;
    column.xposition = 3;   // non-zero: `x == 0` would pass on the default whatever the code did

    CHECK(column.lightCount() == 10);

    const auto coords = coordsOf(column);
    REQUIRE(coords.size() == 10);
    for (mm::nrOfLightsType i = 0; i < 10; i++) {
        CHECK(coords[i].idx == i);                       // contiguous, no holes
        CHECK(coords[i].x == 3);                         // every light on the configured column
        CHECK(coords[i].y == static_cast<mm::lengthType>(i));
        CHECK(coords[i].z == 0);
    }
}

// The two layouts must agree: a 1×10×1 grid and a 10-high column are the same physical strip, so
// they produce identical coordinates in identical order or one of them is wrong.
TEST_CASE("A 10-high column emits the same coordinates as a 1x10x1 grid") {
    mm::SingleColumnLayout column;
    column.height = 10;

    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 10;
    grid.depth = 1;

    CHECK(column.lightCount() == grid.lightCount());

    const auto a = coordsOf(column);
    const auto b = coordsOf(grid);
    REQUIRE(a.size() == b.size());
    for (size_t i = 0; i < a.size(); i++) {
        CHECK(a[i].idx == b[i].idx);
        CHECK(a[i].x == b[i].x);
        CHECK(a[i].y == b[i].y);
        CHECK(a[i].z == b[i].z);
    }
}

// `starting Y` offsets the coordinates but NOT the indices: the driver writes light 0 first
// whatever the column's position in space, so an offset that shifted indices would leave the
// first `start_y` lights of the strip unwritten.
TEST_CASE("starting Y moves the column in space without renumbering its lights") {
    mm::SingleColumnLayout column;
    column.start_y = 5;
    column.height = 4;

    const auto coords = coordsOf(column);
    REQUIRE(coords.size() == 4);
    CHECK(coords[0].idx == 0);
    CHECK(coords[0].y == 5);
    CHECK(coords[3].idx == 3);
    CHECK(coords[3].y == 8);
}

// Reversed wiring flips which end of the strip is light 0 — the y values run high to low while
// the indices still start at 0 and stay contiguous.
TEST_CASE("reversed order walks the column from the far end while keeping indices contiguous") {
    mm::SingleColumnLayout column;
    column.start_y = 5;     // reversal must compose with the offset, not ignore it
    column.height = 4;
    column.reversed_order = true;

    // Assert EVERY entry, not just the ends: an interior swap or a repeat leaves the first and
    // last correct while the middle of the strip is wrong.
    const auto coords = coordsOf(column);
    REQUIRE(coords.size() == 4);
    const mm::lengthType expectY[4] = {8, 7, 6, 5};   // start_y + height - 1 down to start_y
    for (mm::nrOfLightsType i = 0; i < 4; i++) {
        CHECK(coords[i].idx == i);                    // indices still ascend from 0
        CHECK(coords[i].y == expectY[i]);             // coordinates descend
    }
}
