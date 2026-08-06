// @module EffectBase
// @also SineEffect, PlasmaEffect, NoiseEffect, DistortionWavesEffect, LavaLampEffect, MetaballsEffect, SpiralEffect, RingsEffect, WaveEffect, RainbowEffect

// Pins the EXACT rendered output of the time-driven effects, so the power-function migration's
// "renders exactly the same" claim is proved rather than asserted.
//
// These ten effects each hand-roll the same BPM phase accumulator, and step 1 of the migration
// replaces that hand-rolled arithmetic with the shared BeatPhase. A behaviour test still passes if
// the replacement is off by one LSB or drifts over frames; only a hash over the frame catches it.
//
// Five hashes were UPDATED on 2026-08-06 with the BeatPhase migration — Sine, Plasma,
// DistortionWaves, Spiral and Metaballs — all for the SAME reason, deliberately: the
// hand-rolled accumulator started from `lastElapsed_ = 0`, so on the very first tick it added
// `now * bpm` and the startup phase depended on how long the device had been running — the same
// pattern would make an effect start at a different point in its animation on every boot. BeatPhase
// uses the first call as the time base only. Verified as the SOLE cause by reproducing the old
// first-tick behaviour on top of BeatPhase and watching the original hash return.
//
// The control cases prove it: WaveEffect and NoiseEffect ALREADY carried that guard, and their
// hashes did NOT move across the same migration. So a moved hash here means "this effect gained the
// guard", not "the migration drifted".
//
// Every other hash below was captured from the code BEFORE the migration and must not move. If one does,
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
#include "light/effects/StarSkyEffect.h"
#include "light/effects/SpiralEffect.h"
#include "light/effects/WaveEffect.h"

using namespace mm;

// A 2D grid wide enough that a phase error shows as a visible column shift, small enough to stay a
// fast unit test. Eight frames at the real 20 ms cadence exercise the accumulator's carry.
TEST_CASE("time-driven effects render byte-identical frames (migration guard)") {
    SUBCASE("SineEffect")            { SineEffect e;            golden::checkGolden("SineEffect",            golden::renderHash(e, 16, 16, 1), 0xe96c6fd2da1b264bull); }
    SUBCASE("PlasmaEffect")          { PlasmaEffect e;          golden::checkGolden("PlasmaEffect",          golden::renderHash(e, 16, 16, 1), 0xfe821e9102099b93ull); }
    SUBCASE("NoiseEffect")           { NoiseEffect e;           golden::checkGolden("NoiseEffect",           golden::renderHash(e, 16, 16, 1), 0xdeb42f569f324cebull); }
    SUBCASE("DistortionWavesEffect") { DistortionWavesEffect e; golden::checkGolden("DistortionWavesEffect", golden::renderHash(e, 16, 16, 1), 0xe4cd8111e8159133ull); }
    SUBCASE("LavaLampEffect")        { LavaLampEffect e;        golden::checkGolden("LavaLampEffect",        golden::renderHash(e, 16, 16, 1), 0x3c312e8a75b9ac83ull); }
    SUBCASE("MetaballsEffect")       { MetaballsEffect e;       golden::checkGolden("MetaballsEffect",       golden::renderHash(e, 16, 16, 1), 0x96a26bf931ad8341ull); }
    SUBCASE("SpiralEffect")          { SpiralEffect e;          golden::checkGolden("SpiralEffect",          golden::renderHash(e, 16, 16, 1), 0xe12038a7e327b81dull); }
    SUBCASE("RingsEffect")           { RingsEffect e;           golden::checkGolden("RingsEffect",           golden::renderHash(e, 16, 16, 1), 0x302e3697c01f41f0ull); }
    SUBCASE("WaveEffect")            { WaveEffect e;            golden::checkGolden("WaveEffect",            golden::renderHash(e, 16, 16, 1), 0xa1150376dd23bea1ull); }
    SUBCASE("StarSkyEffect")         { StarSkyEffect e;         golden::checkGolden("StarSkyEffect",         golden::renderHash(e, 16, 16, 1), 0xa7ff8aab806be9ffull); }
    SUBCASE("RainbowEffect")         { RainbowEffect e;         golden::checkGolden("RainbowEffect",         golden::renderHash(e, 16, 16, 1), 0x75a2b1be1db07979ull); }
}
