// @module EffectBase
// @also SineEffect, PlasmaEffect, NoiseEffect, DistortionWavesEffect, LavaLampEffect, MetaballsEffect, SpiralEffect, RingsEffect, WaveEffect, RainbowEffect

// Pins the EXACT rendered output of the time-driven effects, so the power-function migration's
// "renders exactly the same" claim is proved rather than asserted.
//
// These ten effects each hand-roll the same BPM phase accumulator, and step 1 of the migration
// replaces that hand-rolled arithmetic with the shared BeatPhase. A behaviour test still passes if
// the replacement is off by one LSB or drifts over frames; only a hash over the frame catches it.
//
// Every hash below was captured from the code BEFORE the migration and must not move. If one does,
// either the migration changed the arithmetic (a bug — the accumulators are meant to be identical)
// or the change was intentional and reviewed, in which case the golden is updated in the same commit
// with the reason in the message.

#include "golden_frame.h"

#include "light/effects/DistortionWavesEffect.h"
#include "light/effects/LavaLampEffect.h"
#include "light/effects/MetaballsEffect.h"
#include "light/effects/NoiseEffect.h"
#include "light/effects/PlasmaEffect.h"
#include "light/effects/RainbowEffect.h"
#include "light/effects/RingsEffect.h"
#include "light/effects/SineEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/WaveEffect.h"

using namespace mm;

// A 2D grid wide enough that a phase error shows as a visible column shift, small enough to stay a
// fast unit test. Eight frames at the real 20 ms cadence exercise the accumulator's carry.
TEST_CASE("time-driven effects render byte-identical frames (migration guard)") {
    SUBCASE("SineEffect")            { SineEffect e;            golden::checkGolden("SineEffect",            golden::renderHash(e, 16, 16, 1), 0x2c252f0721689327ull); }
    SUBCASE("PlasmaEffect")          { PlasmaEffect e;          golden::checkGolden("PlasmaEffect",          golden::renderHash(e, 16, 16, 1), 0x95fd4f682e8d3970ull); }
    SUBCASE("NoiseEffect")           { NoiseEffect e;           golden::checkGolden("NoiseEffect",           golden::renderHash(e, 16, 16, 1), 0xd5ff01c490d4f8f5ull); }
    SUBCASE("DistortionWavesEffect") { DistortionWavesEffect e; golden::checkGolden("DistortionWavesEffect", golden::renderHash(e, 16, 16, 1), 0x74db27f3ee286c48ull); }
    SUBCASE("LavaLampEffect")        { LavaLampEffect e;        golden::checkGolden("LavaLampEffect",        golden::renderHash(e, 16, 16, 1), 0x3c312e8a75b9ac83ull); }
    SUBCASE("MetaballsEffect")       { MetaballsEffect e;       golden::checkGolden("MetaballsEffect",       golden::renderHash(e, 16, 16, 1), 0xdbbd925f7d027080ull); }
    SUBCASE("SpiralEffect")          { SpiralEffect e;          golden::checkGolden("SpiralEffect",          golden::renderHash(e, 16, 16, 1), 0x86c620e64699d69aull); }
    SUBCASE("RingsEffect")           { RingsEffect e;           golden::checkGolden("RingsEffect",           golden::renderHash(e, 16, 16, 1), 0x4c6ccc587c9225a0ull); }
    SUBCASE("WaveEffect")            { WaveEffect e;            golden::checkGolden("WaveEffect",            golden::renderHash(e, 16, 16, 1), 0x4d923b9b9f5ed17dull); }
    SUBCASE("RainbowEffect")         { RainbowEffect e;         golden::checkGolden("RainbowEffect",         golden::renderHash(e, 16, 16, 1), 0x65550fcc4f6af463ull); }
}
