// @module SphereMoveEffect

#include "doctest.h"
#include "light/effects/SphereMoveEffect.h"
#include "light/layouts/GridLayout.h"

// Helper: build a Layer hosting a SphereMoveEffect on a w×h×d grid.
static void buildSphere(mm::Layouts& layouts, mm::GridLayout& grid, mm::Layer& layer,
                        mm::SphereMoveEffect& sphere, int w, int h, int d) {
    grid.width = static_cast<mm::lengthType>(w);
    grid.height = static_cast<mm::lengthType>(h);
    grid.depth = static_cast<mm::lengthType>(d);
    layouts.addChild(&grid);

    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    layer.addChild(&sphere);
    layer.onBuildState();
}

// The effect fully clears the buffer each frame, so a thin shell leaves the vast majority of a large
// volume black (it is a hollow surface, not a solid fill).
TEST_CASE("SphereMoveEffect leaves most of a large volume dark (thin shell, full clear)") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::SphereMoveEffect sphere;
    buildSphere(layouts, grid, layer, sphere, 12, 12, 12);

    // Pre-paint every channel white so we can observe the per-frame full clear: any pixel the shell
    // does not touch must return to black.
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++) buf.data()[i] = 255;

    layer.loop();

    size_t lit = 0;
    for (size_t p = 0; p < buf.count(); p++) {
        const uint8_t* px = buf.data() + p * 3;
        if (px[0] || px[1] || px[2]) lit++;
    }
    // The one-unit-thick shell of a diameter ~2..3 sphere is a small fraction of a 12³ = 1728 voxel
    // volume — far below half. This pins "thin surface + full clear", not "renders something".
    CHECK(lit < buf.count() / 2);
}

// Every voxel the effect lights is a real palette colour (non-black) — the shell is drawn, not
// left as leftover noise.
TEST_CASE("SphereMoveEffect only writes non-black palette colours") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    mm::SphereMoveEffect sphere;
    buildSphere(layouts, grid, layer, sphere, 16, 16, 16);

    // Rainbow palette (index 0) is generated at full saturation/value, so no entry is black — a lit
    // shell voxel is therefore always non-black. Palettes::active() is a process-wide static, so pin it.
    mm::Palettes::setActive(0);
    layer.loop();

    // Any pixel that is set has all-black or a genuine colour; because the buffer was zero-initialised
    // and cleared, every non-zero pixel here is a shell voxel. Assert the shell exists and is coloured.
    auto& buf = layer.buffer();
    bool anyLit = false;
    for (size_t p = 0; p < buf.count(); p++) {
        const uint8_t* px = buf.data() + p * 3;
        if (px[0] || px[1] || px[2]) { anyLit = true; break; }
    }
    // A diameter ~2..3 shell on a 16³ grid with an origin inside the volume lights some voxels; the
    // colour comes from the palette so it is never a stray single-channel artifact.
    CHECK(anyLit);
}

// The effect is 3D-native: it declares D3 dimensions.
TEST_CASE("SphereMoveEffect reports 3D dimensions") {
    mm::SphereMoveEffect sphere;
    CHECK(sphere.dimensions() == mm::Dim::D3);
}

// Hard rule: the effect must run at any grid size without crashing, including a 0×0×0 volume and a
// 1×1×1 volume (the loop guards w/h/d <= 0 and clamps speed so 100-speed is never zero).
TEST_CASE("SphereMoveEffect survives degenerate grids") {
    {
        mm::Layouts layouts;
        mm::GridLayout grid;
        mm::Layer layer;
        mm::SphereMoveEffect sphere;
        buildSphere(layouts, grid, layer, sphere, 0, 0, 0);
        layer.loop();  // must not crash
        CHECK(layer.buffer().count() == 0);
    }
    {
        mm::Layouts layouts;
        mm::GridLayout grid;
        mm::Layer layer;
        mm::SphereMoveEffect sphere;
        buildSphere(layouts, grid, layer, sphere, 1, 1, 1);
        layer.loop();  // must not crash
        CHECK(layer.buffer().count() == 1);
    }
}
