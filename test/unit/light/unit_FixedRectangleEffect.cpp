// @module FixedRectangleEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/FixedRectangleEffect.h"
#include "light/layouts/GridLayout.h"

#include <utility>  // std::pair for the outside-the-box coordinate list

// A small box (2×2 at the origin) lights exactly its cells and leaves every cell outside it black.
TEST_CASE("FixedRectangleEffect lights only cells inside the configured rect") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FixedRectangleEffect rect;
    rect.rectX = 0; rect.rectY = 0; rect.rectZ = 0;
    rect.rectW = 2; rect.rectH = 2; rect.rectD = 1;
    rect.red = 182; rect.green = 15; rect.blue = 98;
    layer.addChild(&rect);

    layer.applyState();
    layer.tick();

    auto* data = layer.buffer().data();
    REQUIRE(layer.buffer().count() == 16);

    // Inside the 2×2 box: each cell carries the configured RGB.
    for (int y = 0; y < 2; y++) {
        for (int x = 0; x < 2; x++) {
            size_t off = static_cast<size_t>(y * 4 + x) * 3;
            CHECK(data[off + 0] == 182);
            CHECK(data[off + 1] == 15);
            CHECK(data[off + 2] == 98);
        }
    }

    // Outside the box: cells stay black (e.g. (2,0), (0,2), (3,3)).
    for (auto [x, y] : {std::pair{2, 0}, std::pair{3, 0}, std::pair{0, 2}, std::pair{3, 3}}) {
        size_t off = static_cast<size_t>(y * 4 + x) * 3;
        CHECK(data[off + 0] == 0);
        CHECK(data[off + 1] == 0);
        CHECK(data[off + 2] == 0);
    }
}

// With defaults (origin 0,0,0 + 15×15×15 extent) the box fills the whole grid — the origin corner lights up.
TEST_CASE("FixedRectangleEffect defaults light the origin corner and fill a small grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FixedRectangleEffect rect;  // defaults: 15×15×15 box at (0,0,0)
    layer.addChild(&rect);

    layer.applyState();
    layer.tick();

    auto* data = layer.buffer().data();

    // Origin corner (0,0) is lit with the default color {182,15,98}.
    CHECK(data[0] == 182);
    CHECK(data[1] == 15);
    CHECK(data[2] == 98);

    // The 15×15 default clamps to the 4×4 grid, so every cell is lit (none black).
    bool allLit = true;
    for (size_t i = 0; i < layer.buffer().count(); i++) {
        size_t off = i * 3;
        if (data[off] == 0 && data[off + 1] == 0 && data[off + 2] == 0) { allLit = false; break; }
    }
    CHECK(allLit);
}

// The box is offset away from the origin: only the offset cell lights, the origin stays black.
TEST_CASE("FixedRectangleEffect honours a non-zero origin") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FixedRectangleEffect rect;
    rect.rectX = 2; rect.rectY = 1; rect.rectZ = 0;
    rect.rectW = 1; rect.rectH = 1; rect.rectD = 1;
    layer.addChild(&rect);

    layer.applyState();
    layer.tick();

    auto* data = layer.buffer().data();

    // Cell (2,1) is lit.
    size_t litOff = static_cast<size_t>(1 * 4 + 2) * 3;
    CHECK((data[litOff] > 0 || data[litOff + 1] > 0 || data[litOff + 2] > 0));

    // Origin (0,0) is untouched — black.
    CHECK(data[0] == 0);
    CHECK(data[1] == 0);
    CHECK(data[2] == 0);
}

// A degenerate 0×0×0 grid must not crash (Effects run at every grid size).
TEST_CASE("FixedRectangleEffect survives a degenerate grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::FixedRectangleEffect rect;
    layer.addChild(&rect);

    layer.applyState();
    layer.tick();  // must return without dereferencing an empty buffer

    CHECK(true);
}
