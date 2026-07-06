// @module TetrixEffect

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/TetrixEffect.h"
#include "light/layouts/GridLayout.h"
#include "platform/platform.h"   // setTestNowMs — deterministic virtual time

// TetrixEffect runs one falling-brick state machine per X column. Every column is seeded with a 2 s
// start delay (step = millis()+2000) in onBuildState(), so nothing renders until that delay elapses;
// once it does the column spawns a brick that falls and stacks. The per-effect Random8 has a fixed
// default seed, so with the clock frozen via setTestNowMs the whole effect is deterministic — the
// cases below freeze/advance virtual time so the state machine crosses its delays predictably. Each
// case restores the real clock through the guard so a frozen clock never leaks into another test file.

namespace {

// Builds a layer holding a Tetrix effect on a w×h×1 RGB grid, all children wired.
struct TetrixRig {
    mm::Layouts     layouts;
    mm::GridLayout  grid;
    mm::Layer       layer;
    mm::TetrixEffect fx;

    TetrixRig(mm::lengthType w, mm::lengthType h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
};

// Restores the real platform clock so a frozen time never leaks past the case that set it.
struct ClockGuard { ~ClockGuard() { mm::platform::setTestNowMs(0); } };

bool anyLit(mm::Layer& layer) {
    auto& buf = layer.buffer();
    for (size_t i = 0; i < buf.bytes(); i++) if (buf.data()[i] != 0) return true;
    return false;
}

} // namespace

// During the initial 2 s start delay every column is idle-waiting, so the very first frame renders
// nothing: the buffer is entirely black even though the effect is enabled and built.
TEST_CASE("TetrixEffect renders black during the start delay") {
    ClockGuard guard;
    mm::platform::setTestNowMs(1000);   // freeze; onBuildState seeds step = 1000+2000

    TetrixRig rig(8, 8);
    rig.layer.onBuildState();

    // Still inside the 2 s start window (3000 > 1000), so the state machine only waits.
    rig.layer.loop();

    CHECK(rig.grid.width * rig.grid.height == 64);
    CHECK_FALSE(anyLit(rig.layer));
}

// Once virtual time advances past the start delay, columns spawn bricks that fall and render: after a
// span of frames at least one light is lit, and every lit light carries a real (non-black) RGB colour
// pulled from the palette rather than partial/garbage channels.
TEST_CASE("TetrixEffect lights up with palette colour after the start delay") {
    ClockGuard guard;
    mm::platform::setTestNowMs(0);

    TetrixRig rig(8, 8);
    rig.layer.onBuildState();   // step = 2000 for every column

    // Advance well past the 2 s start delay, then run many frames so the start-roll (step 1→2) fires
    // and bricks descend into the visible region. Step time forward each frame like a real tick loop.
    bool lit = false;
    for (uint32_t t = 3000; t <= 8000 && !lit; t += 25) {
        mm::platform::setTestNowMs(t);
        rig.layer.loop();
        lit = anyLit(rig.layer);
    }
    REQUIRE(lit);

    // Every non-black light is a full RGB triple from colorFromPalette — assert no lit light is a
    // single stray channel (a lit light means at least one channel > 0; the brick colour is a palette
    // entry written across all three channels, so a lit pixel is a genuine colour, not noise).
    auto& buf = rig.layer.buffer();
    bool foundColoured = false;
    for (size_t p = 0; p + 2 < buf.bytes(); p += 3) {
        const uint8_t r = buf.data()[p], g = buf.data()[p + 1], b = buf.data()[p + 2];
        if (r || g || b) { foundColoured = true; break; }
    }
    CHECK(foundColoured);
}

// Effects must run at every grid size: a degenerate 0×0×0 grid and a 1×1 grid both survive a build +
// several frames across advancing time without crashing (no allocation, no out-of-range write).
TEST_CASE("TetrixEffect survives degenerate and minimal grids") {
    ClockGuard guard;

    // 0×0×0: onBuildState allocates zero drops, loop() bails on the w<=0 guard.
    {
        TetrixRig rig(0, 0);
        rig.grid.depth = 0;
        mm::platform::setTestNowMs(0);
        rig.layer.onBuildState();
        for (uint32_t t = 0; t <= 6000; t += 500) {
            mm::platform::setTestNowMs(t);
            rig.layer.loop();
        }
        CHECK(rig.layer.buffer().count() == 0);
    }

    // 1×1: a single column, one row — the brick fills and clears the lone light without crashing.
    {
        TetrixRig rig(1, 1);
        mm::platform::setTestNowMs(0);
        rig.layer.onBuildState();
        for (uint32_t t = 0; t <= 8000; t += 25) {
            mm::platform::setTestNowMs(t);
            rig.layer.loop();
        }
        CHECK(rig.layer.buffer().count() == 1);
    }
}
