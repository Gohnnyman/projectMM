// @module LissajousEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/LissajousEffect.h"
#include "light/layouts/GridLayout.h"

// A single frame paints part of the grid: the swept curve lights some pixels (not a black frame).
TEST_CASE("LissajousEffect traces a lit curve on the grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::LissajousEffect lissajous;
    layer.addChild(&lissajous);

    layer.onBuildState();
    // Pin a colourful palette (Rainbow=0) so painted pixels are non-black regardless of prior tests
    // mutating the process-wide active palette.
    mm::Palettes::setActive(0);
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 256);

    // The curve samples 256 points across a 16×16 grid, so at least one pixel is lit.
    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// The curve is sparse: on a large grid it lights only some pixels, leaving others black.
TEST_CASE("LissajousEffect leaves untouched pixels black") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 32;
    grid.height = 32;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::LissajousEffect lissajous;
    layer.addChild(&lissajous);

    layer.onBuildState();
    mm::Palettes::setActive(0);
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 1024);

    // A single 256-sample sweep cannot cover 1024 lights, so at least one pixel stays black —
    // this distinguishes a traced curve from a full-buffer fill.
    bool hasBlack = false;
    for (mm::nrOfLightsType px = 0; px < buf.count(); px++) {
        size_t idx = static_cast<size_t>(px) * 3;
        if (buf.data()[idx] == 0 && buf.data()[idx + 1] == 0 && buf.data()[idx + 2] == 0) {
            hasBlack = true;
            break;
        }
    }
    CHECK(hasBlack);
}

// On a 1×1 grid the whole curve collapses onto the single origin light without indexing out of bounds.
TEST_CASE("LissajousEffect on a 1x1 grid maps the curve to the origin light") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 1;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::LissajousEffect lissajous;
    layer.addChild(&lissajous);

    layer.onBuildState();
    mm::Palettes::setActive(0);
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 1);
    // A size-1 axis maps to coordinate 0, so every sample lands on light 0: it must be lit.
    CHECK((buf.data()[0] > 0 || buf.data()[1] > 0 || buf.data()[2] > 0));
}

// Effects must run at every grid size (hard rule): a 0×0×0 grid renders without crashing.
TEST_CASE("LissajousEffect on a 0x0x0 grid does not crash") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::LissajousEffect lissajous;
    layer.addChild(&lissajous);

    layer.onBuildState();
    // Degenerate grid: loop() bails on width/height <= 0, so this is a safe no-op frame.
    layer.loop();

    CHECK(layer.buffer().count() == 0);
}
