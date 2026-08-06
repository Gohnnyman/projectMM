// @module math16
// @also math8

// The 16-bit fixed-point tier the power-function contract is written in. What matters here is not
// that the functions compute something, but the three properties large fixtures depend on: sin16 is
// SMOOTH (no 8-bit staircase), map32 does not lose the last column to a fencepost, and BeatPhase
// keeps animating when the frame time is under a millisecond — the failure that silently froze
// hand-rolled accumulators on desktop.

#include "doctest.h"
#include "core/math16.h"

#include <cmath>

using namespace mm;

TEST_CASE("sin16 traces a full sine over one turn") {
    CHECK(sin16(0) == 32768);                      // zero crossing, rising
    CHECK(sin16(16384) > 65000);                   // peak
    CHECK(sin16(32768) == 32768);                  // zero crossing, falling
    CHECK(sin16(49152) < 600);                     // trough
    // The angle wraps for free: one full turn later is the same value.
    CHECK(sin16(1234) == sin16(static_cast<angle16>(1234 + 65536)));
    CHECK(cos16(0) > 65000);                       // cosine peaks where sine crosses
}

// The reason the 16-bit tier exists: on a large fixture an 8-bit sine steps visibly. Sampling finer
// than the 8-bit LUT's resolution must produce intermediate values, not a staircase.
TEST_CASE("sin16 is smooth between LUT entries, where sin8 would step") {
    // Four samples inside ONE 8-bit step (angle 0x1000..0x10C0 all share sin8 index 0x10).
    const uint16_t a = sin16(0x1000), b = sin16(0x1040), c = sin16(0x1080), d = sin16(0x10C0);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(c < d);                                   // strictly rising — a staircase would tie
    CHECK(sin8(0x10) == sin8(0x10));                // (the 8-bit form has one value for all four)
}

// Accuracy against the ideal sine: the claim in the design doc is ~0.2% of amplitude, which is what
// makes the zero-extra-flash implementation acceptable instead of a bigger table.
TEST_CASE("sin16 stays within 0.5% of a true sine") {
    double worst = 0.0;
    for (uint32_t t = 0; t < 65536; t += 7) {       // 7: a stride that hits varied LUT positions
        const double ideal = (std::sin(t * 2.0 * M_PI / 65536.0) * 32767.0) + 32768.0;
        worst = std::max(worst, std::abs(ideal - sin16(static_cast<angle16>(t))));
    }
    CHECK(worst < 65535 * 0.005);
}

TEST_CASE("map32 maps a range and clamps outside it") {
    CHECK(map32(5, 0, 10, 0, 100) == 50);
    CHECK(map32(0, 0, 10, 0, 100) == 0);
    CHECK(map32(10, 0, 10, 0, 100) == 100);
    CHECK(map32(-5, 0, 10, 0, 100) == 0);           // below the input range clamps to outLo
    CHECK(map32(999, 0, 10, 0, 100) == 100);        // above clamps to outHi
    CHECK(map32(5, 0, 0, 7, 100) == 7);             // zero span has no ratio: outLo
    CHECK(map32(5, 10, 0, 0, 100) == 50);           // descending input range
    CHECK(map32(2, 0, 10, 100, 0) == 80);           // descending output range
}

// The fencepost six effects each carried a comment about: mapping an audio band to a grid column
// must be able to reach the LAST column, which the naive (n-1)/(max-1) form loses.
TEST_CASE("map32 can reach the last column of a grid") {
    const int32_t width = 16;
    CHECK(map32(255, 0, 255, 0, width) == width);   // full input reaches the extent
    CHECK(map32(254, 0, 255, 0, width) == 15);      // just under still lands inside
}

TEST_CASE("BeatPhase keeps animating when frames are under a millisecond") {
    BeatPhase p;
    p.advance(1000, 120);                            // first call only sets the time base
    CHECK(p.phase(256) == 0);

    // 1000 frames of a sub-millisecond dt: a per-tick divide would round each to zero and freeze.
    // (Integer ms means some ticks advance 0 and some 1 — the accumulator must survive both.)
    for (uint32_t i = 1; i <= 1000; i++) p.advance(1000 + i / 2, 120);
    CHECK(p.phase(256) > 0);
}

TEST_CASE("BeatPhase advances proportionally to elapsed time and rate") {
    BeatPhase slow, fast;
    slow.advance(0, 60); fast.advance(0, 120);
    slow.advance(1000, 60); fast.advance(1000, 120);
    CHECK(fast.numerator() == 2 * slow.numerator());  // double the rate, double the phase

    BeatPhase p;
    p.advance(0, 60);
    p.advance(500, 60);
    const uint64_t half = p.numerator();
    p.advance(1000, 60);
    CHECK(p.numerator() == 2 * half);                 // double the time, double the phase
}

TEST_CASE("BeatPhase holds still at rate zero and resets to zero") {
    BeatPhase p;
    p.advance(0, 120);
    p.advance(1000, 0);
    CHECK(p.numerator() == 0);

    p.advance(2000, 120);
    CHECK(p.numerator() > 0);
    p.reset();
    CHECK(p.numerator() == 0);
}

// millis() wraps every ~49 days; unsigned subtraction gives the correct delta across the wrap, so a
// long-running device must not see the phase jump backwards or leap.
TEST_CASE("BeatPhase survives the millis wrap") {
    BeatPhase p;
    p.advance(0xFFFFFF00u, 120);
    p.advance(0xFFFFFF00u + 100u, 120);              // wraps past 2^32
    const uint64_t afterWrap = p.numerator();

    BeatPhase q;
    q.advance(1000, 120);
    q.advance(1100, 120);                            // the same 100 ms, no wrap
    CHECK(afterWrap == q.numerator());
}
