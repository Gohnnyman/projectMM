// @module BouncingBallsEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/BouncingBallsEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"  // setTestNowMs — freeze millis() for a deterministic first frame

// Restore the real clock after any test that froze it, so a frozen value can't leak into
// order-dependent neighbours.
namespace { struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } }; }

// On the first frame every ball is at rest (zero-init state) and bounces off the floor, so the
// effect paints the bottom row of every column and leaves the rows above it black.
TEST_CASE("BouncingBallsEffect lights the bottom row on the first frame") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);  // any non-zero value freezes millis() at t=1ms

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    layer.addChild(&balls);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 16);

    const int w = 4, h = 4;
    // Bottom row (y = h-1): at least one column is lit — the balls all rest here on frame one.
    bool bottomLit = false;
    for (int x = 0; x < w; x++) {
        size_t idx = static_cast<size_t>((h - 1) * w + x) * 3;
        if (buf.data()[idx] | buf.data()[idx + 1] | buf.data()[idx + 2]) { bottomLit = true; break; }
    }
    CHECK(bottomLit);

    // Every row above the floor is black on the first frame (balls have not risen yet).
    bool upperLit = false;
    for (int y = 0; y < h - 1; y++) {
        for (int x = 0; x < w; x++) {
            size_t idx = static_cast<size_t>(y * w + x) * 3;
            if (buf.data()[idx] | buf.data()[idx + 1] | buf.data()[idx + 2]) { upperLit = true; }
        }
    }
    CHECK_FALSE(upperLit);
}

// numBalls=0 draws nothing: the effect clamps below its minimum and the buffer stays black.
TEST_CASE("BouncingBallsEffect with zero balls leaves the buffer black") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1);

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 4;
    grid.height = 4;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    balls.numBalls = 0;  // nBalls <= 0 → tick() returns before drawing
    layer.addChild(&balls);

    layer.applyState();
    layer.tick();

    auto& buf = layer.buffer();
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// The effect runs at a degenerate grid size without crashing (the "every grid size" hard rule).
TEST_CASE("BouncingBallsEffect survives a 0x0x0 grid") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 0;
    grid.height = 0;
    grid.depth = 0;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    layer.addChild(&balls);

    // No allocation, no draw, no crash on a zero grid.
    layer.applyState();
    layer.tick();

    CHECK(layer.buffer().count() == 0);
}

// A ball reaches its apex mid-flight: once time advances, at least one ball has risen off the
// bottom row, so the lit column occupies a row above the floor.
TEST_CASE("BouncingBallsEffect balls rise above the floor as time advances") {
    ClockGuard guard;

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 1;
    grid.height = 16;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BouncingBallsEffect balls;
    balls.numBalls = 8;
    layer.addChild(&balls);

    layer.applyState();

    // Frame one at t=1 relaunches every ball (impactVelocity kick set from its floor bounce).
    mm::platform::setTestNowMs(1);
    layer.tick();

    // A ball with the maximum kick (√19.62·1.0 ≈ 4.43) climbs for ~450ms before falling back; sample
    // partway up its arc. Scan a spread of instants so the assertion doesn't hinge on one exact frame.
    const int w = 1, h = 16;
    bool roseAboveFloor = false;
    for (uint32_t t = 50; t <= 400 && !roseAboveFloor; t += 50) {
        mm::platform::setTestNowMs(1 + t);
        layer.tick();
        for (int y = 0; y < h - 1; y++) {  // any row strictly above the floor
            size_t idx = static_cast<size_t>(y * w + 0) * 3;
            if (layer.buffer().data()[idx] | layer.buffer().data()[idx + 1] | layer.buffer().data()[idx + 2]) {
                roseAboveFloor = true;
                break;
            }
        }
    }
    CHECK(roseAboveFloor);
}
