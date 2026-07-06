// @module PraxisEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/PraxisEffect.h"
#include "light/layouts/GridLayout.h"

// Praxis overwrites EVERY pixel each frame (a full-grid palette field, no black
// background) — with a non-black palette active, no light is left at (0,0,0).
TEST_CASE("PraxisEffect fills every pixel from the palette") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PraxisEffect praxis;
    layer.addChild(&praxis);

    layer.onBuildState();
    // Rainbow palette (0) is generated at full saturation/value, so every wheel index
    // maps to a lit colour — makes "every pixel lit" order-independent of prior tests.
    mm::Palettes::setActive(0);
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 64);

    // No pixel is black: Praxis writes every (x,y) rather than painting sparsely.
    auto* data = buf.data();
    bool everyPixelLit = true;
    for (size_t p = 0; p < buf.count(); p++) {
        uint8_t r = data[p * 3], g = data[p * 3 + 1], b = data[p * 3 + 2];
        if (r == 0 && g == 0 && b == 0) { everyPixelLit = false; break; }
    }
    CHECK(everyPixelLit);
}

// The hue is a function of (x, y): pixels far apart in the grid carry different colours,
// so the field is spatial, not a uniform fill.
TEST_CASE("PraxisEffect varies colour across the grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::PraxisEffect praxis;
    layer.addChild(&praxis);

    layer.onBuildState();
    mm::Palettes::setActive(0);
    layer.loop();

    auto* data = layer.buffer().data();
    // The y·macro·x cross term makes far-apart pixels land on different hues. Compare the
    // origin with the far corner (15,15), where the spatial term is at its largest.
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t idx = (15 * 16 + 15) * 3;
    uint8_t r1 = data[idx], g1 = data[idx + 1], b1 = data[idx + 2];
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}

// Hard rule: the effect runs at a degenerate grid without crashing. width/height <= 0
// is guarded, and a 1×1 grid exercises the render loop at its smallest.
TEST_CASE("PraxisEffect survives degenerate grid sizes") {
    for (int dim : {0, 1}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dim;
        grid.height = dim;
        grid.depth = dim;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::PraxisEffect praxis;
        layer.addChild(&praxis);

        layer.onBuildState();
        layer.loop();  // must not crash at 0×0×0 or 1×1×1
        CHECK(true);
    }
}
