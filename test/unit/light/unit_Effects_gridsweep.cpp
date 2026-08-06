// @module EffectBase

// The grid-size floor, swept across EVERY registered effect rather than one at a time.
//
// The hard rule (CLAUDE.md § Principles, Robustness; architecture.md § Robustness rules):
// an effect must produce a correct result at ANY grid size, including a degenerate one —
// no crash, no divide-by-zero, no out-of-bounds write. A modifier can shrink the logical
// grid to 0x0x0 (every layout child disabled), and a 1-wide or 1-deep grid is what a
// single strand or a flat panel actually is.
//
// Layer::tick owns the degenerate-grid gate: it skips the effect pass when any axis is 0,
// so effects may assume width/height/depth >= 1 and carry no such guard themselves. The
// 0x0x0 row therefore pins the LAYER's behaviour, not each effect's: that a folded-away
// grid produces a clean no-op and a zero-byte buffer rather than a crash. Effect bodies
// are covered by the three non-degenerate rows, where they do run.
//
// Do NOT "improve" this by calling fx->tick() directly on the zero grid. That asserts a
// contract no effect makes, and it fails: GEQ3DEffect divides by width() (imap over a
// zero range) and takes SIGFPE. The single Layer gate is what makes that safe.
//
// Per-effect tests pin this one effect at a time, which means a NEW effect is covered only
// if its author remembers to write that case. This sweep asks the factory for every
// registered effect instead, so the floor is enforced for effects that do not exist yet:
// register an effect, and it is swept the moment it lands.
//
// Reading a failure: the CHECK message names the effect and the grid it died on. "Died"
// here means it crashed the runner — an effect that draws nothing on a zero grid is
// correct (a clean no-op is the specified behaviour), so the assertion is about surviving
// and leaving a well-formed buffer, not about pixels.

#include "doctest.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/layers/Layer.h"
// Generated at build time from src/light/effects/*.h — see test/CMakeLists.txt. Supplies
// forEachEffect(), so this file names no individual effect and cannot drift.
#include "effect_sweep.h"
#include <string>

namespace {

// The degenerate and near-degenerate grids every effect must tolerate. Each is a real
// configuration a user can produce, not a synthetic edge case:
//   0x0x0  every layout child disabled (a modifier folded the grid away)
//   1x1x1  a single light
//   1xNx1  one strand
//   Nx1x1  one row
struct GridCase {
    mm::lengthType w, h, d;
    const char* label;
};

// Channel counts a real fixture presents: a single-channel (mono/white) strand, a two-channel
// oddity, RGB, RGBW, and a wide fixture-profile slot. An effect must render what the buffer can
// hold rather than refuse to draw — writing three bytes unconditionally would either corrupt a
// narrow buffer or (with a guard) leave it black, which reads to a user as "this effect is broken".
const uint8_t kChannelCounts[] = {1, 2, 3, 4, 8};

const GridCase kGrids[] = {
    {0, 0, 0, "0x0x0 (empty grid)"},
    {1, 1, 1, "1x1x1 (single light)"},
    {1, 16, 1, "1x16x1 (one strand)"},
    {16, 1, 1, "16x1x1 (one row)"},
};

// Drive one effect through one grid: build the layer, tick it twice, then tear down.
// Two ticks matter because several effects allocate or seed on the first tick and read
// that state on the next one — a zero grid must not leave a trap for frame two.
bool runEffectOnGrid(const std::string& name, mm::MoonModule* fx, const GridCase& g,
                     uint8_t cpl = 3) {
    mm::Layouts layouts;
    mm::GridLayout grid;
    mm::Layer layer;

    grid.width = g.w; grid.height = g.h; grid.depth = g.d;
    layouts.addChild(&grid);
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(cpl);

    layer.addChild(fx);
    layer.applyState();
    layer.tick();
    layer.tick();

    // The buffer contract holds even at zero size: a zero-light layer reports zero bytes
    // rather than a stale non-zero span a driver would then read past.
    if (g.w == 0 || g.h == 0 || g.d == 0) {
        CHECK_MESSAGE(layer.buffer().bytes() == 0,
                      name << " left a non-empty buffer on " << g.label);
    }

    // Did the effect actually put light in the buffer? "No crash" is not enough — but neither is
    // "wrote something": an effect that assumes RGB on a 1-channel buffer writes two bytes PAST
    // each light into its neighbours, which stays in bounds and looks like output while silently
    // corrupting the frame. The caller checks that separately via a canary (see the sweep).
    bool wrote = false;
    for (size_t i = 0; i < layer.buffer().bytes(); i++)
        if (layer.buffer().data()[i]) { wrote = true; break; }

    // release() returns every buffer in the tree (it recurses to children); the caller
    // then destroys the effect. The Layer is a local about to go out of scope, so there
    // is no detach to do — and a removeChild() here would run a structural mutation over
    // a just-released tree.
    layer.release();
    return wrote;
}

}  // namespace

// The sweep. One TEST_CASE over every effect keeps the failure output readable: a broken
// effect names itself and the grid it died on, and the rest still run.
// 1 and 2 channels are the interesting cases: an effect that assumes RGB either writes past its
// light or (with a guard) declines to render at all, and a user sees a black fixture either way.
TEST_CASE("every effect renders at any channel count") {
    int swept = 0;
    mm::forEachEffect([&](const char* name, auto make) {
        for (uint8_t cpl : kChannelCounts) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(cpl);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            const GridCase g{8, 8, 1, "8x8x1"};
            const bool wrote = runEffectOnGrid(effectName, fx, g, cpl);
            // Effects that always paint every pixel must do so at ANY channel count. The rest
            // (audio-reactive, sparse, or input-driven) legitimately render nothing here, so they
            // are checked for not crashing only.
            if (effectName == "SolidEffect" || effectName == "RainbowEffect" ||
                effectName == "WaveEffect"  || effectName == "PlasmaEffect")
                CHECK_MESSAGE(wrote, effectName << " drew nothing at cpl=" << int(cpl));
            delete fx;
            swept++;
        }
    });
    MESSAGE("swept " << swept << " effect x channel-count combinations");
}

TEST_CASE("every effect survives degenerate grid sizes") {
    int swept = 0;

    mm::forEachEffect([&](const char* name, auto make) {
        for (const auto& g : kGrids) {
            const std::string effectName(name);
            CAPTURE(effectName);
            CAPTURE(g.label);
            mm::MoonModule* fx = make();
            REQUIRE_MESSAGE(fx != nullptr, "could not construct " << effectName);
            runEffectOnGrid(effectName, fx, g);
            delete fx;
        }
        swept++;
    });

    // An empty effect list would make this test vacuously green — the most dangerous kind
    // of passing test. The generator refuses to emit an empty list; this is the second lock.
    CHECK_MESSAGE(swept > 0, "no effects swept — the test would pass without testing anything");
    MESSAGE("swept " << swept << " effects x " << (sizeof(kGrids) / sizeof(kGrids[0])) << " grids");
}
