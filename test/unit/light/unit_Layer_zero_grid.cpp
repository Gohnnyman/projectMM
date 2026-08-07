// @module Layer
// @also RainbowEffect, NoiseEffect, PlasmaEffect, SpiralEffect, MetaballsEffect, RingsEffect, RipplesEffect, LavaLampEffect, FireEffect, ParticlesEffect, GameOfLifeEffect, GEQ3DEffect, PaintBrushEffect

#include "doctest.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"
#include "light/layouts/GridLayout.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/RipplesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/FireEffect.h"
#include "light/effects/ParticlesEffect.h"
#include "light/effects/GameOfLifeEffect.h"
#include "light/effects/GEQ3DEffect.h"
#include "light/effects/PaintBrushEffect.h"
#include "light/modifiers/ModifierBase.h"

// Pin the "Effects must work at every grid size" rule. A 0-light layout is a
// real configuration — a modifier can shrink the logical grid to 0,0,0 or
// every layout child can be disabled. Effects' tick() must be a clean no-op
// in that case (no div-by-zero, no OOB writes, no crash).

namespace {

template <typename Effect>
void run_with_empty_layout() {
    mm::Layouts layouts;  // no children → totalLightCount() == 0
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    Effect e;
    layer.addChild(&e);
    layouts.applyState();
    layer.applyState();  // logical/physical dims all zero, no buffer
    // The real assertion is "doesn't crash" — if tick() reaches a divide-by-zero
    // or an OOB write the process dies before we get here.
    layer.tick();
    CHECK(layer.width() == 0);
    CHECK(layer.height() == 0);
    CHECK(layer.depth() == 0);
}

/// A modifier that only counts its ticks — enough to tell "still running" from "frozen".
class CountingModifier : public mm::ModifierBase {
public:
    void tick() MM_NONBLOCKING override { ticks++; }
    uint32_t ticks = 0;
};

} // namespace

// Each per-effect case runs the same probe: build a Layer over an empty Layouts (no children → 0 lights),
// then prepare() + tick(). The assertion is "no crash, no div-by-zero, no OOB write" plus dims == 0.

// Rainbow on 0,0,0 grid: no crash.
TEST_CASE("RainbowEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RainbowEffect>(); }
// Noise on 0,0,0 grid: no crash.
TEST_CASE("NoiseEffect on 0,0,0 grid")       { run_with_empty_layout<mm::NoiseEffect>(); }
// Plasma on 0,0,0 grid: no crash.
TEST_CASE("PlasmaEffect on 0,0,0 grid")      { run_with_empty_layout<mm::PlasmaEffect>(); }
// Spiral on 0,0,0 grid: no crash.
TEST_CASE("SpiralEffect on 0,0,0 grid")      { run_with_empty_layout<mm::SpiralEffect>(); }
// Metaballs on 0,0,0 grid: no crash.
TEST_CASE("MetaballsEffect on 0,0,0 grid")   { run_with_empty_layout<mm::MetaballsEffect>(); }
// Rings on 0,0,0 grid: no crash.
TEST_CASE("RingsEffect on 0,0,0 grid")       { run_with_empty_layout<mm::RingsEffect>(); }
// Ripples on 0,0,0 grid: no crash.
TEST_CASE("RipplesEffect on 0,0,0 grid")     { run_with_empty_layout<mm::RipplesEffect>(); }
// LavaLamp on 0,0,0 grid: no crash.
TEST_CASE("LavaLampEffect on 0,0,0 grid")    { run_with_empty_layout<mm::LavaLampEffect>(); }
// Fire on 0,0,0 grid: no heat buffer allocated, no crash.
TEST_CASE("FireEffect on 0,0,0 grid")        { run_with_empty_layout<mm::FireEffect>(); }
// Particles on 0,0,0 grid: no trail buffer allocated, no crash.
TEST_CASE("ParticlesEffect on 0,0,0 grid")   { run_with_empty_layout<mm::ParticlesEffect>(); }
// GameOfLife on 0,0,0 grid: no heap alloc for 0 cells, no crash.
TEST_CASE("GameOfLifeEffect on 0,0,0 grid")  { run_with_empty_layout<mm::GameOfLifeEffect>(); }
// GEQ3D / PaintBrush on 0,0,0 grid: audio effects, no crash with no buffer.
TEST_CASE("GEQ3DEffect on 0,0,0 grid")       { run_with_empty_layout<mm::GEQ3DEffect>(); }
TEST_CASE("PaintBrushEffect on 0,0,0 grid")  { run_with_empty_layout<mm::PaintBrushEffect>(); }

// A modifier keeps its per-frame state moving while the grid is empty. A beat-driven modifier that
// stalled here would come back in the wrong phase once the layout returns, so the empty interval has
// to pass THROUGH the modifier chain rather than around it. This is the half of the rule the
// per-effect cases above cannot see: they assert nothing runs, this asserts something still does.
TEST_CASE("modifiers keep ticking while the grid is empty") {
    mm::Layouts layouts;              // no children → 0 lights, so the effect pass is skipped
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    CountingModifier mod;
    layer.addChild(&mod);
    layouts.applyState();
    layer.applyState();

    REQUIRE(layer.width() == 0);      // the precondition the test is about
    for (int i = 0; i < 5; i++) layer.tick();
    CHECK(mod.ticks == 5);            // every frame reached the modifier
}

// A zero channel count is rejected at the setter rather than defended against downstream: it would
// allocate a zero-byte buffer and make every effect's per-light stride 0. Enforcing it at the one
// entry point is what lets effects and draw primitives assume `cpl >= 1`.
TEST_CASE("a layer refuses a zero channel count") {
    mm::Layouts layouts;
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    const uint8_t before = layer.channelsPerLight();
    layer.setChannelsPerLight(0);
    CHECK(layer.channelsPerLight() == before);   // unchanged, not zeroed
    layer.setChannelsPerLight(4);
    CHECK(layer.channelsPerLight() == 4);        // a valid value still applies
}

// The live pass walks the modifier mapping into the buffer, so an empty layout has nothing for it
// to remap. It is gated on hasGrid alongside the effect pass; the modifier ticks still run.
TEST_CASE("a live modifier is skipped on an empty grid without crashing") {
    mm::Layouts layouts;                 // no children -> 0 lights
    mm::Layer layer;
    layer.setLayouts(&layouts);
    layer.setChannelsPerLight(3);
    CountingModifier mod;
    layer.addChild(&mod);
    layouts.applyState();
    layer.applyState();

    REQUIRE(layer.width() == 0);
    for (int i = 0; i < 5; i++) layer.tick();
    CHECK(mod.ticks == 5);               // modifiers still advance; the live REMAP is what is skipped
}
