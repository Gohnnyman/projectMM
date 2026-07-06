// @module StarSkyEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/StarSkyEffect.h"
#include "light/layouts/GridLayout.h"

// Helper: build a layer around a grid and attach a StarSky effect, then build state once.
static void wire(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                 mm::StarSkyEffect& star, mm::lengthType w, mm::lengthType h, mm::lengthType d) {
    grid.width = w;
    grid.height = h;
    grid.depth = d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&star);
    layer.onBuildState();
}

// A field of stars lights at least some pixels on a populated 3D grid after a frame.
TEST_CASE("StarSkyEffect lights pixels on a populated grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::StarSkyEffect star;
    // A large fill ratio guarantees a healthy pool of stars so a lit pixel is near-certain.
    star.star_fill_ratio = 255;
    star.speed = 10;
    wire(layouts, grid, layer, star, 8, 8, 4);

    layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.data() != nullptr);
    REQUIRE(buf.count() == 8 * 8 * 4);

    bool hasNonZero = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { hasNonZero = true; break; }
    }
    CHECK(hasNonZero);
}

// White stars (usePalette=false) paint only greyscale: every lit pixel has R==G==B.
TEST_CASE("StarSkyEffect white stars paint greyscale pixels") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::StarSkyEffect star;
    star.usePalette = false;
    star.star_fill_ratio = 255;
    star.speed = 5;
    wire(layouts, grid, layer, star, 8, 8, 1);

    layer.loop();

    auto* data = layer.buffer().data();
    size_t litPixels = 0;
    for (size_t p = 0; p < layer.buffer().count(); p++) {
        uint8_t r = data[p * 3], g = data[p * 3 + 1], b = data[p * 3 + 2];
        if (r || g || b) {
            litPixels++;
            // A white star's colour is RGB{b,b,b}: the three channels must be equal.
            CHECK(r == g);
            CHECK(g == b);
        }
    }
    CHECK(litPixels > 0);
}

// A zero fill ratio still seeds a pool (nb_stars = ratio*count/10000 + 1) so a lit pixel appears.
TEST_CASE("StarSkyEffect always keeps at least one star") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::StarSkyEffect star;
    star.star_fill_ratio = 0;   // 0*count/10000 + 1 == 1 star
    star.speed = 20;
    wire(layouts, grid, layer, star, 4, 4, 1);

    // Step several frames; the single star keeps fading/respawning, so within a few frames it lights.
    bool everLit = false;
    for (int f = 0; f < 8 && !everLit; f++) {
        layer.loop();
        auto* data = layer.buffer().data();
        for (size_t i = 0; i < layer.buffer().bytes(); i++) {
            if (data[i] != 0) { everLit = true; break; }
        }
    }
    CHECK(everLit);
}

// The effect survives degenerate grids (0x0x0 and 1x1x1) without crashing — the every-grid-size rule.
TEST_CASE("StarSkyEffect runs at degenerate grid sizes") {
    {
        mm::Layouts layouts;
        mm::GridLayout grid;
        mm::Layer layer;
        mm::StarSkyEffect star;
        star.star_fill_ratio = 255;
        wire(layouts, grid, layer, star, 0, 0, 0);
        layer.loop();   // must not crash on an empty grid
        CHECK(layer.buffer().count() == 0);
    }
    {
        mm::Layouts layouts;
        mm::GridLayout grid;
        mm::Layer layer;
        mm::StarSkyEffect star;
        star.star_fill_ratio = 255;
        star.speed = 30;
        wire(layouts, grid, layer, star, 1, 1, 1);
        layer.loop();   // single cell — no out-of-bounds
        CHECK(layer.buffer().count() == 1);
    }
}
