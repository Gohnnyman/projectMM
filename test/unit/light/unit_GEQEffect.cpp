// @module GEQEffect
// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/GEQEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/AudioService.h"

#include <array>

// GEQ is an audio-reactive 2D effect: the 16 bands spread across the columns and each column rises as a
// bar from the floor (bottom row) up to a height set by its band's loudness. The frame comes from
// AudioService::latestFrame() (a process-wide static). To feed a signal on the host (no I2S mic) we run a
// live AudioService with `simulate` set to an "always" mode — synthesizeFrame() fills the bands each
// loop(). Every case that needs audio brackets its own AudioService setup()/teardown() so it never leaks
// the active-mic pointer into another test file. Buffer index = (y*width + x)*3, y=0 is the TOP row so
// y=height-1 is the floor the bars grow up from.

// With no live audio source every band is silent, so no bar rises and the buffer stays black.
TEST_CASE("GEQEffect stays black without an audio frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.ripple = 0;   // no falling peak dot: silence must leave the buffer fully dark
    layer.addChild(&geq);

    layer.onBuildState();
    // No AudioService is active → latestFrame() is the static all-silence frame (bands all 0). Each loop
    // fades then reads silence → every bar height 0 → nothing drawn.
    for (int i = 0; i < 8; i++) layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 64);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// A bar grows from the floor up: when a column's band is loud, its bottom (floor) pixel is lit while a
// pixel above the bar's top stays dark — bars fill upward from the bottom row, not top-down or floating.
TEST_CASE("GEQEffect fills columns from the floor upward") {
    mm::AudioService audio;
    audio.onBuildControls();
    audio.simulate = 4;   // sweep (always): one band lit at a time, deterministic — column 0 maps to
                          // band 0 (bass), so we can drive a known column loud.
    audio.setup();

    const int W = 16, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.ripple = 0;    // disable the peak dot so the only lit pixels are the bar itself
    geq.fadeOut = 255; // fully clear the previous frame so a lit floor pixel is this frame's bar
    layer.addChild(&geq);

    layer.onBuildState();

    auto floorLit = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(H - 1) * W + x) * 3;   // bottom row of column x
        return d[idx] || d[idx + 1] || d[idx + 2];
    };
    auto topLit = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(0) * W + x) * 3;       // top row of column x
        return d[idx] || d[idx + 1] || d[idx + 2];
    };

    // Sweep steps a lit band every ~250 ms; run frames until some column's floor lights, then assert
    // the invariant on that column: if the floor is dark, the top must be dark too (a bar never floats).
    bool sawBar = false;
    for (int i = 0; i < 64; i++) {
        audio.loop();
        layer.loop();
        for (int x = 0; x < W; x++) {
            if (floorLit(x)) sawBar = true;
            // A lit top pixel with a dark floor would mean the bar didn't grow from the bottom.
            if (topLit(x)) CHECK(floorLit(x));
        }
    }
    CHECK(sawBar);   // at least one column rose during the sweep

    audio.teardown();
}

// colorBars colours each bar by its column index, so two well-separated lit columns take different hues
// rather than sharing the row-height gradient — the toggle changes what colour a bar is.
TEST_CASE("GEQEffect colorBars colours bars per column") {
    mm::AudioService audio;
    audio.onBuildControls();
    audio.simulate = 3;   // music (always): keeps every band non-zero so many columns rise together
    audio.setup();

    const int W = 16, H = 8;
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = W;
    grid.height = H;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::GEQEffect geq;
    geq.colorBars = true;   // hue per column: imap(x,0,W-1,0,255)
    geq.ripple = 0;
    geq.fadeOut = 255;
    layer.addChild(&geq);

    layer.onBuildState();
    mm::Palettes::setActive(0);   // Rainbow: index maps to a spread of hues, order-independent

    // Advance until both an early and a late column have a lit floor, then compare their colours.
    const int xa = 0, xb = W - 1;
    auto color = [&](int x) {
        auto* d = layer.buffer().data();
        size_t idx = (static_cast<size_t>(H - 1) * W + x) * 3;
        return std::array<uint8_t, 3>{d[idx], d[idx + 1], d[idx + 2]};
    };
    auto lit = [](const std::array<uint8_t, 3>& c) { return c[0] || c[1] || c[2]; };

    bool compared = false;
    for (int i = 0; i < 64 && !compared; i++) {
        audio.loop();
        layer.loop();
        auto ca = color(xa), cb = color(xb);
        if (lit(ca) && lit(cb)) {
            // Column 0 (hue 0) and column 15 (hue 255) are opposite ends of the palette: their bar
            // colours differ, confirming the colour is driven by column, not by shared row height.
            CHECK((ca[0] != cb[0] || ca[1] != cb[1] || ca[2] != cb[2]));
            compared = true;
        }
    }
    CHECK(compared);

    audio.teardown();
}

// The hard rule: the effect runs at any grid size without crashing, including 0×0×0 and 1×1, with a live
// audio frame feeding it every tick.
TEST_CASE("GEQEffect survives degenerate grid sizes") {
    mm::AudioService audio;
    audio.onBuildControls();
    audio.simulate = 3;
    audio.setup();

    for (auto dims : {mm::Coord3D{0, 0, 0}, mm::Coord3D{1, 1, 1}}) {
        mm::Layouts layouts;
        mm::GridLayout grid;
        grid.width = dims.x;
        grid.height = dims.y;
        grid.depth = dims.z;
        layouts.addChild(&grid);

        mm::Layer layer;
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);

        mm::GEQEffect geq;
        layer.addChild(&geq);

        layer.onBuildState();
        for (int i = 0; i < 4; i++) { audio.loop(); layer.loop(); }
    }
    CHECK(true);   // no crash at 0×0×0 or 1×1

    audio.teardown();
}
