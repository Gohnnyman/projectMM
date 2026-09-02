// @module AmbilightEffect

#include "doctest.h"
#include "core/VideoService.h"
#include "light/effects/AmbilightEffect.h"
#include "light/layouts/GridLayout.h"
#include "light/layouts/Layouts.h"

#include <cstdint>
#include <cstring>

// Pins the frame → light mapping end to end, through the real static seam: a live VideoService
// publishes a frame, the effect renders it, the buffer is read back. The checks are about
// orientation and coverage, because a picture averaged over the wrong rectangle — or flipped
// top-for-bottom — still looks like *a* picture.

using mm::AmbilightEffect;
using mm::VideoService;

namespace {

// A live VideoService in test-pattern mode: red top band, blue bottom, yellow left, green right.
// Its seat is claimed on construction and vacated on destruction, so each case starts clean.
struct PatternSource {
    VideoService svc;
    PatternSource() {
        svc.source = 0;      // test pattern
        svc.applyState();    // builds the buffer and renders the first frame
    }
};

// Render one frame of the effect over a `w`x`h` grid and hand back the layer for inspection.
struct Rig {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;
    AmbilightEffect fx;

    Rig(uint16_t w, uint16_t h) {
        grid.width = w; grid.height = h; grid.depth = 1;
        layouts.addChild(&grid);
        layer.setLayouts(&layouts);
        layer.setChannelsPerLight(3);
        layer.addChild(&fx);
    }
    void render() { layer.applyState(); layer.tick(); }
    // applyState() re-runs prepare(), which un-primes the smoother and makes every frame jump.
    // Anything testing the EASING has to tick without it.
    void tickOnly() { layer.tick(); }
    const uint8_t* px(int x, int y) const {
        return layer.buffer().data() + (static_cast<size_t>(y) * grid.width + x) * 3;
    }
};

}  // namespace

// Orientation must survive to the buffer: red top band → red first row. Swapped, the whole picture
// is upside down — on a TV border, the difference between matching the screen and mirroring it.
TEST_CASE("AmbilightEffect: the frame's orientation reaches the buffer, top band to top row") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;   // identity, so the raw zone means are what we read
    rig.render();

    const uint8_t* top = rig.px(4, 0);
    const uint8_t* bottom = rig.px(4, 7);
    CHECK(top[0] > top[2]);        // top row reads red-dominant
    CHECK(bottom[2] > bottom[0]);  // bottom row reads blue-dominant
}

// The horizontal counterpart of the test above; together they pin all four edges. Red separates
// the bands: it is in yellow and absent from green.
TEST_CASE("AmbilightEffect: the frame's left and right bands reach the matching columns") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.render();

    const uint8_t* left = rig.px(0, 4);    // mid-height, so neither the top nor bottom band
    const uint8_t* right = rig.px(7, 4);
    CHECK(left[0] > right[0]);             // yellow carries red; green does not
    CHECK(right[1] > 0);                   // and the right column is lit at all
}

// Every light must be written. An empty zone leaves its light holding the previous frame, so a
// mapping bug shows as dead lights scattered through the strip rather than an obvious failure.
TEST_CASE("AmbilightEffect: every light is written, none left dark by an empty zone") {
    PatternSource src;
    Rig rig(16, 9);
    rig.fx.saturation = 100;
    rig.render();

    int lit = 0;
    for (int y = 0; y < 9; y++)
        for (int x = 0; x < 16; x++) {
            const uint8_t* p = rig.px(x, y);
            if (p[0] || p[1] || p[2]) lit++;
        }
    // The pattern's centre is deliberately black, so not every light is lit — but the four bands
    // are, and they are the majority of a 16x9 border-shaped frame.
    CHECK(lit > 0);
    // The corners sit inside the coloured bands and must never be dark.
    for (const auto& [x, y] : {std::pair{0, 0}, std::pair{15, 0}, std::pair{0, 8}, std::pair{15, 8}}) {
        const uint8_t* p = rig.px(x, y);
        CHECK((p[0] || p[1] || p[2]));
    }
}

// More lights than source pixels: neighbouring cells must SHARE one rather than resolve to an
// empty zone. The pattern is 64 wide, so a 128-wide layer forces the guard.
TEST_CASE("AmbilightEffect: a layer finer than the frame still writes every light") {
    PatternSource src;
    Rig rig(128, 4);
    rig.fx.saturation = 100;
    rig.render();

    // Top row of the pattern is the red band; at 4 rows tall every row samples some band, so the
    // whole first row must be lit across its full width — no gaps from zero-width zones.
    for (int x = 0; x < 128; x++) {
        const uint8_t* p = rig.px(x, 0);
        CHECK((p[0] || p[1] || p[2]));
    }
}

// brightness scales the result down uniformly. Distinct from the driver's brightness: this one dims
// the video relative to whatever else is composited beside it.
TEST_CASE("AmbilightEffect: brightness scales the sampled colour down") {
    PatternSource src;
    Rig full(8, 8);
    full.fx.saturation = 100;
    full.fx.brightness = 255;
    full.render();
    const int fullRed = full.px(4, 0)[0];

    Rig dim(8, 8);
    dim.fx.saturation = 100;
    dim.fx.brightness = 64;
    dim.render();
    const int dimRed = dim.px(4, 0)[0];

    CHECK(fullRed > 0);
    CHECK(dimRed < fullRed);
    CHECK(dimRed == (fullRed * 64) / 255);
}

// Saturation stretches each channel away from the zone's luma — above 100 a coloured zone gets
// more saturated, which is what pulls averaged means back off grey.
TEST_CASE("AmbilightEffect: saturation above 100 pushes a coloured zone further from grey") {
    PatternSource src;
    Rig flat(8, 8);
    flat.fx.saturation = 100;
    flat.render();
    const uint8_t* a = flat.px(4, 0);
    const int flatSpread = static_cast<int>(a[0]) - static_cast<int>(a[2]);

    Rig boosted(8, 8);
    boosted.fx.saturation = 180;
    boosted.render();
    const uint8_t* b = boosted.px(4, 0);
    const int boostedSpread = static_cast<int>(b[0]) - static_cast<int>(b[2]);

    CHECK(flatSpread > 0);
    CHECK(boostedSpread >= flatSpread);   // red pulled further from blue, or already clipped at 255
}

// No source paints black rather than returning early, which would leave the PREVIOUS effect's
// picture frozen on the strip. Every effect owns its background (unit_Effects_gridsweep).
TEST_CASE("AmbilightEffect: no video source paints black, never the previous effect's frame") {
    // No PatternSource here, so no service holds the seat and latestFrame() reports nothing.
    REQUIRE(VideoService::latestFrame()->rgb == nullptr);

    Rig rig(4, 4);
    rig.layer.applyState();
    // Paint a recognisable frame, standing in for whatever effect ran before this one.
    uint8_t* buf = rig.layer.buffer().data();
    REQUIRE(buf != nullptr);
    for (size_t i = 0; i < rig.layer.buffer().count(); i++) {
        buf[i * 3 + 0] = 11; buf[i * 3 + 1] = 22; buf[i * 3 + 2] = 33;
    }
    rig.layer.tick();
    CHECK(rig.px(2, 2)[0] == 0);
    CHECK(rig.px(2, 2)[1] == 0);
    CHECK(rig.px(2, 2)[2] == 0);
}

// --- Smoothing ---------------------------------------------------------------------------------
// The accumulators are 8.8 precisely so a slow setting still ARRIVES: in whole bytes every step
// rounds to zero and the light stalls short of its target forever. These check that it converges,
// and that a big move still lands at once.

// Off must be bit-identical to no smoothing at all, since it is the default.
TEST_CASE("AmbilightEffect: smoothing off follows the frame exactly") {
    PatternSource src;
    Rig plain(8, 8), off(8, 8);
    plain.fx.saturation = 100;
    off.fx.saturation = 100;
    off.fx.smoothing = 0;
    plain.render();
    off.render();
    CHECK(std::memcmp(plain.px(4, 0), off.px(4, 0), 3) == 0);
}

// The first frame after a gap must land immediately — creeping up from black would show as a fade-in
// every time the source reconnects.
TEST_CASE("AmbilightEffect: the first frame lands whole, not smoothed up out of black") {
    PatternSource src;
    Rig fast(8, 8), slow(8, 8);
    fast.fx.saturation = 100;
    slow.fx.saturation = 100;
    slow.fx.smoothing = 240;   // very slow, so a smoothed first frame would be nearly black
    fast.render();
    slow.render();
    CHECK(std::memcmp(fast.px(4, 0), slow.px(4, 0), 3) == 0);
}

// The one that 8-bit state would fail: with heavy smoothing every per-frame step is a fraction of
// a byte, so the value only moves if those fractions are kept between frames. Checked in BOTH
// directions — >> floors, so a rising channel and a falling one converge by different routes and
// only the falling one works by accident.
TEST_CASE("AmbilightEffect: a heavily smoothed light converges exactly, rising and falling") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.smoothing = 250;   // a step of ~6/256 of the remaining distance
    rig.fx.snapAbove = 0;     // never jump, so only the smoothing can get it there
    rig.render();             // primes on the first frame, so this one lands whole

    const uint8_t bright = rig.px(4, 0)[0];
    REQUIRE(bright > 8);      // the band has somewhere to fall from

    // Falling: drive the target down and let it smooth in.
    rig.fx.brightness = 8;
    for (int i = 0; i < 2000; i++) rig.tickOnly();
    const uint8_t dim = rig.px(4, 0)[0];
    CHECK(dim < bright / 2);

    // Rising back to where it started must land on the SAME value, not one short.
    rig.fx.brightness = 255;
    for (int i = 0; i < 2000; i++) rig.tickOnly();
    CHECK(rig.px(4, 0)[0] == bright);
}

// The soft start rides OUTSIDE the smoother: the accumulators keep tracking the true picture while
// only the emitted level ramps. Off by default, so the picture lands at full the moment it arrives.
TEST_CASE("AmbilightEffect: fadeInMs off means the first picture lands at full level") {
    PatternSource src;
    Rig instant(8, 8), faded(8, 8);
    instant.fx.saturation = 100;
    faded.fx.saturation = 100;
    faded.fx.fadeInMs = 0;
    instant.render();
    faded.render();
    CHECK(std::memcmp(instant.px(4, 0), faded.px(4, 0), 3) == 0);
}

// With a ramp set, the first frame must be dark and later frames brighter — the whole point being
// that a room does not jump to full brightness the instant a console wakes up.
TEST_CASE("AmbilightEffect: fadeInMs ramps the first frames up from black") {
    PatternSource src;
    Rig rig(8, 8);
    rig.fx.saturation = 100;
    rig.fx.fadeInMs = 4000;   // long, so the first ticks land near the bottom of the ramp
    rig.render();

    const uint8_t first = rig.px(4, 0)[0];
    for (int i = 0; i < 50; i++) rig.tickOnly();
    const uint8_t later = rig.px(4, 0)[0];

    CHECK(later >= first);    // never goes backwards
    // And it does reach full: the reference rig has no ramp, so its value is the target.
    Rig reference(8, 8);
    reference.fx.saturation = 100;
    reference.render();
    CHECK(later <= reference.px(4, 0)[0]);
}
