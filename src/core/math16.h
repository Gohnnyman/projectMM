#pragma once

// 16-bit fixed-point tier — the power-function contract's numeric vocabulary.
//
// Why a 16-bit tier alongside math8.h: an 8-bit result positions to 256 levels, which visibly steps
// on a large fixture (a 12K-light wall shows the staircase in a slow gradient or a slow-moving
// blob). Effects must look smooth at every size, so the contract every effect and script writes
// against is 16-bit; math8.h stays for genuinely 8-bit domains (palette index and hue are mod-256 by
// design) and as the internal fast path.
//
// Cost discipline, measured rather than assumed: interpolating the existing 8-bit `sin8_lut` was
// tried first (zero new bytes) and REJECTED — rounding the endpoints to 8 bits distorts the segments
// the interpolation runs between, giving 1.1% of amplitude, worse than the 0.69% of FastLED's
// classic `lib8tion` sin16. A 130-byte quarter-wave 16-bit table with the same linear interpolation
// measures 0.031% — 22x better than lib8tion at a cost that rounds to nothing. FastLED master's
// `fl::sin32` is near-exact but spends 1040 bytes plus two int64 multiplies per call; this is the
// middle that keeps large-fixture gradients smooth without that. A quadratic core can swap in behind
// this same name if a field-heavy effect ever needs it.

#include "core/math8.h"

#include <cstdint>

namespace mm {

// ---- Angles and fractions --------------------------------------------------
// angle16: 65536 = one full turn. Overflow IS the wrap, so phase arithmetic never needs a modulo.
using angle16 = uint16_t;
// frac16: 0..65535 as a 0..1 fraction (interpolation weights, easing in/out).
using frac16  = uint16_t;

/// Quarter-wave sine table: sin(pi/2 * i/64) * 32767, 65 entries (0..64 inclusive, so the last
/// segment has both endpoints). 130 bytes in flash; the other three quadrants come from symmetry.
inline constexpr int16_t sin16_quarter[65] = {
        0,   804,  1608,  2410,  3212,  4011,  4808,  5602,
     6393,  7179,  7962,  8739,  9512, 10278, 11039, 11793,
    12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530,
    18204, 18868, 19519, 20159, 20787, 21403, 22005, 22594,
    23170, 23731, 24279, 24811, 25329, 25832, 26319, 26790,
    27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956,
    30273, 30571, 30852, 31113, 31356, 31580, 31785, 31971,
    32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757,
    32767
};

/// Sine over a 16-bit angle, returning 0..65535 with 32768 at the zero crossing (unsigned, matching
/// sin8's convention so a caller ports by widening rather than re-reasoning about sign).
///
/// Quarter-wave lookup with linear interpolation: the top 2 bits of the angle pick the quadrant,
/// the next 6 the table entry, the low 8 the position between entries. Mirroring the index in odd
/// quadrants and negating in the upper half reconstructs the full wave from a quarter of the table.
inline uint16_t sin16(angle16 theta) {
    const uint8_t quadrant = static_cast<uint8_t>(theta >> 14);       // 0..3
    const uint16_t pos     = static_cast<uint16_t>(theta & 0x3FFF);   // position within the quadrant
    // Odd quadrants run the quarter wave backwards (sin descends from the peak).
    const uint16_t walk = (quadrant & 1) ? static_cast<uint16_t>(0x4000 - pos) : pos;
    // uint16 for the index, NOT uint8: walk reaches 0x4000 exactly at a quadrant boundary in an odd
    // quadrant, and >>8 is then 64 — which a uint8 holds fine but only by luck of the cast order.
    // Keeping it wide makes the "table has 65 entries so idx==64 is valid" invariant explicit.
    const uint16_t idx  = static_cast<uint16_t>(walk >> 8);           // 0..64
    const uint8_t frac  = static_cast<uint8_t>(walk & 0xFF);
    const int32_t a = sin16_quarter[idx];
    // At idx==64 the wave is exactly at the peak and frac is 0, so the b term contributes nothing;
    // clamping keeps the read inside the 65-entry table without a branch on the hot path.
    const int32_t b = sin16_quarter[idx < 64 ? idx + 1 : 64];
    int32_t v = a + (((b - a) * frac) >> 8);
    if (quadrant >= 2) v = -v;                                        // lower half of the circle
    return static_cast<uint16_t>(v + 32768);
}

/// Cosine: a quarter turn ahead of sine.
inline uint16_t cos16(angle16 theta) { return sin16(static_cast<angle16>(theta + 16384)); }

// ---- Range mapping ---------------------------------------------------------

/// Map `v` from [inLo,inHi] to [outLo,outHi] in 64-bit intermediate, clamped to the input range.
///
/// The fencepost lives HERE, once: six effects hand-rolled this and each carried its own comment
/// about the off-by-one (mapping to `n-1` vs `n` when the output is a grid extent). Callers map to
/// the extent (`0..w`) and the result is clamped to `w-1` by the pixel writer, which is the form
/// that does not lose the last column.
inline int32_t map32(int32_t v, int32_t inLo, int32_t inHi, int32_t outLo, int32_t outHi) {
    if (inHi == inLo) return outLo;                       // zero span: no meaningful ratio
    if (inHi > inLo) { if (v <= inLo) return outLo; if (v >= inHi) return outHi; }
    else             { if (v >= inLo) return outLo; if (v <= inHi) return outHi; }
    const int64_t num = static_cast<int64_t>(v - inLo) * (outHi - outLo);
    return static_cast<int32_t>(outLo + num / (inHi - inLo));
}

// ---- Beat phase ------------------------------------------------------------

/// A resolution- and framerate-independent BPM phase accumulator.
///
/// Nine effects hand-rolled this identically, and the shape is subtle enough to be worth owning
/// once: the per-tick product `dt * bpm * scale / 60000` rounds to ZERO when dt is under a
/// millisecond (every desktop frame, and an ESP32 running a small fixture fast), so the animation
/// silently freezes. The fix all nine converged on is to accumulate the RAW numerator in 64 bits and
/// divide only at the read — which is what this does.
///
/// Usage: one member per animated quantity; call `advance(elapsedMs, rate)` once per frame, then
/// read as often as needed. `rate` is BPM-like: the caller's speed control, whatever its units.
class BeatPhase {
public:
    /// Accumulate this frame's contribution. Safe to call with a rate of 0 (the phase holds).
    /// The first call only establishes the time base, so a large `elapsed` at startup cannot jump
    /// the phase — the same first-tick guard three of the nine effects carried by hand.
    void advance(uint32_t elapsedMs, uint32_t rate) {
        if (!started_) { started_ = true; lastMs_ = elapsedMs; return; }
        const uint32_t dt = elapsedMs - lastMs_;   // unsigned: correct across the millis() wrap
        lastMs_ = elapsedMs;
        num_ += static_cast<uint64_t>(dt) * rate;
    }

    /// The phase scaled by `scale` and divided late — `phase(256)` is the uint8 angle form
    /// (256 = full turn) the effects use, `phase(65536)` the angle16 form.
    /// Returns the raw scaled value; the caller truncates to its angle width, which is where the
    /// free wrap happens.
    uint32_t phase(uint32_t scale) const {
        return static_cast<uint32_t>((num_ * scale) / 60000u);
    }

    /// The undivided numerator, for a caller that scales differently (NoiseEffect multiplies the
    /// rate by the grid width before accumulating; DistortionWaves reads one accumulator at two
    /// scales for its second axis).
    uint64_t numerator() const { return num_; }

    /// Feed a pre-scaled product directly — for the callers whose rate already carries a factor.
    void advanceScaled(uint32_t elapsedMs, uint64_t scaledRate) {
        if (!started_) { started_ = true; lastMs_ = elapsedMs; return; }
        const uint32_t dt = elapsedMs - lastMs_;
        lastMs_ = elapsedMs;
        num_ += static_cast<uint64_t>(dt) * scaledRate;
    }

    void reset() { num_ = 0; lastMs_ = 0; started_ = false; }

private:
    uint64_t num_ = 0;        ///< raw dt·rate numerator; divided by 60000 only at the read
    uint32_t lastMs_ = 0;
    bool     started_ = false;
};

}  // namespace mm
