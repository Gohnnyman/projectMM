// @module BlurzEffect
// @also AudioService

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/effects/BlurzEffect.h"
#include "light/layouts/GridLayout.h"
#include "core/AudioService.h"

// Blurz is an audio-reactive effect: its dot is coloured by the current band's magnitude and only
// appears when there is a signal. The frame comes from AudioService::latestFrame() (a process-wide
// static). To feed a signal on the host (no I2S mic) we run a live AudioService with `simulate` set to
// an "always" mode — synthesizeFrame() then fills the bands each loop(). Every case brackets its own
// AudioService setup()/teardown() so it never leaks the active-mic pointer into another test file.

// With no live audio source the buffer stays black: the dot is audio-gated, so silence renders nothing.
TEST_CASE("BlurzEffect stays black without an audio frame") {
    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    layer.addChild(&blurz);

    layer.onBuildState();
    // No AudioService is active → latestFrame() is the static all-silence frame (bands all 0). loop()
    // does the first-frame clear + fade, then reads silence and returns before drawing any dot.
    for (int i = 0; i < 8; i++) layer.loop();

    auto& buf = layer.buffer();
    REQUIRE(buf.count() == 64);
    bool anyLit = false;
    for (size_t i = 0; i < buf.bytes(); i++) {
        if (buf.data()[i] != 0) { anyLit = true; break; }
    }
    CHECK_FALSE(anyLit);
}

// With a synthesized audio frame the effect lights the buffer: the coloured dot appears and the blur
// smears it into a soft blob, so at least some lights become non-zero.
TEST_CASE("BlurzEffect lights the buffer when fed a signal") {
    mm::AudioService audio;
    audio.onBuildControls();
    audio.simulate = 3;   // music (always): synthesizeFrame() fills the bands every loop, no mic needed
    audio.setup();        // claims the active-mic seat so latestFrame() points at this frame

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 8;
    grid.height = 8;
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    layer.addChild(&blurz);

    layer.onBuildState();

    // Advance the audio and the effect together a few frames: the band cursor wraps 0..15, so over
    // several ticks the dot lands a non-zero magnitude and paints.
    bool anyLit = false;
    for (int i = 0; i < 32 && !anyLit; i++) {
        audio.loop();
        layer.loop();
        auto& buf = layer.buffer();
        for (size_t j = 0; j < buf.bytes(); j++) {
            if (buf.data()[j] != 0) { anyLit = true; break; }
        }
    }
    CHECK(anyLit);

    audio.teardown();   // release the active-mic seat; leave no residue for other tests
}

// geqScanner sweeps the dot steadily across the strip: one pixel per frame, so consecutive frames land
// the lit dot at different linear positions rather than the same spot.
TEST_CASE("BlurzEffect geqScanner sweeps the dot to a new position each frame") {
    mm::AudioService audio;
    audio.onBuildControls();
    audio.simulate = 3;   // music (always): keeps the bands non-zero so the dot has colour
    audio.setup();

    mm::Layouts layouts;
    mm::GridLayout grid;
    grid.width = 16;
    grid.height = 1;      // a strip so the scan position maps directly to the x index
    grid.depth = 1;
    layouts.addChild(&grid);

    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);

    mm::BlurzEffect blurz;
    blurz.geqScanner = true;   // steady sweep instead of a random jump
    blurz.blur = 1;            // minimal blur so the brightest pixel stays close to the dot core
    blurz.fadeRate = 255;      // fade the previous frame's trail fully, so the current dot dominates
    layer.addChild(&blurz);

    layer.onBuildState();

    auto brightestIndex = [&]() {
        auto& buf = layer.buffer();
        auto* d = buf.data();
        int best = -1, bestSum = -1;
        for (size_t p = 0; p < buf.count(); p++) {
            int sum = d[p * 3] + d[p * 3 + 1] + d[p * 3 + 2];
            if (sum > bestSum) { bestSum = sum; best = static_cast<int>(p); }
        }
        return best;
    };

    audio.loop(); layer.loop();
    const int pos1 = brightestIndex();
    audio.loop(); layer.loop();
    const int pos2 = brightestIndex();

    // The scanner advances the dot one pixel per frame, so the brightest pixel moves between frames.
    CHECK(pos1 != pos2);

    audio.teardown();
}

// The hard rule: the effect runs at any grid size without crashing, including a 0×0×0 and a 1×1 grid.
TEST_CASE("BlurzEffect survives degenerate grid sizes") {
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

        mm::BlurzEffect blurz;
        layer.addChild(&blurz);

        layer.onBuildState();
        for (int i = 0; i < 4; i++) { audio.loop(); layer.loop(); }
    }
    CHECK(true);   // no crash at 0×0×0 or 1×1

    audio.teardown();
}
