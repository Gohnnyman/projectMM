// @module Noise2DEffect

#include "doctest.h"
#include "light/effects/Noise2DEffect.h"
#include "light/layouts/GridLayout.h"

// A single frame on an 8×8 grid fills the buffer with a palette-mapped noise field (non-zero).
TEST_CASE("Noise2DEffect writes a non-zero palette-mapped noise field") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::Noise2DEffect noise;
    layer.addChild(&noise);

    layer.onBuildState();
    // Palettes::active() is a process-wide static any prior test can mutate; pin a colourful palette
    // (Rainbow=0) so the non-black assertion is order-independent.
    mm::Palettes::setActive(0);
    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 64);

    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// The field is spatial: distant pixels read different noise samples, so their colours differ.
TEST_CASE("Noise2DEffect distant pixels carry different colours") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::Noise2DEffect noise;
    noise.scale = 64;  // default zoom: at (0,0) vs (8,8) the noise coords are (0,0) vs (512,512)
    layer.addChild(&noise);

    layer.onBuildState();
    mm::Palettes::setActive(0);
    layer.loop();

    auto* data = layer.buffer().data();
    uint8_t r0 = data[0], g0 = data[1], b0 = data[2];
    size_t idx88 = (8 * 16 + 8) * 3;
    uint8_t r1 = data[idx88], g1 = data[idx88 + 1], b1 = data[idx88 + 2];
    // Value noise is smooth but not constant; widely separated coords sample different field values,
    // which index different palette entries.
    CHECK((r0 != r1 || g0 != g1 || b0 != b1));
}

// Effects must run at every grid size: a 0×0×0 layer renders without crashing (the cols/rows guard).
TEST_CASE("Noise2DEffect survives a degenerate 0x0 grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::Noise2DEffect noise;
    layer.addChild(&noise);

    layer.onBuildState();
    layer.loop();  // must not crash on an empty grid

    CHECK(layer.buffer().count() == 0);
}
