// @module AudioService

// The desktop audioFft kernel (radix-2 Cooley-Tukey, platform_desktop.cpp) pinned against an
// independent naive-DFT reference: same unnormalized magnitude contract the band math
// (AudioBands) consumes, so a kernel change that shifts scale or bin order fails HERE, not as
// a subtle band-shape drift on hardware.

#include "doctest.h"
#include "platform/platform.h"

#include <cmath>
#include <numbers>
#include <random>

namespace {

// The reference: the O(n^2) DFT the kernel replaced, kept test-local on purpose.
void dftReference(const float* x, size_t n, float* outMag) {
    const float twoPiOverN = -2.0f * std::numbers::pi_v<float> / static_cast<float>(n);
    for (size_t k = 0; k < n / 2; k++) {
        float re = 0.0f, im = 0.0f;
        for (size_t t = 0; t < n; t++) {
            const float a = twoPiOverN * static_cast<float>(k) * static_cast<float>(t);
            re += x[t] * std::cos(a);
            im += x[t] * std::sin(a);
        }
        outMag[k] = std::sqrt(re * re + im * im);
    }
}

}  // namespace

// Random vectors: every bin within a tight relative tolerance of the DFT reference.
TEST_CASE("desktop audioFft matches the DFT reference on random input") {
    constexpr size_t n = 512;
    static float x[n], fast[n / 2], ref[n / 2];
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    for (auto& v : x) v = dist(rng);
    mm::platform::audioFft(x, n, fast);
    dftReference(x, n, ref);
    for (size_t k = 0; k < n / 2; k++) {
        CHECK(std::abs(fast[k] - ref[k]) < 0.01f + 0.001f * ref[k]);
    }
}

// A pure tone lands its energy in the right bin, at the DFT's magnitude.
TEST_CASE("desktop audioFft puts a pure tone's energy in the exact bin") {
    constexpr size_t n = 512;
    constexpr size_t bin = 37;
    static float x[n], mag[n / 2];
    for (size_t t = 0; t < n; t++) {
        x[t] = std::sin(2.0f * std::numbers::pi_v<float> * static_cast<float>(bin) *
                        static_cast<float>(t) / static_cast<float>(n));
    }
    mm::platform::audioFft(x, n, mag);
    // Unnormalized: a unit sine at an exact bin frequency measures n/2 there.
    CHECK(mag[bin] == doctest::Approx(n / 2.0f).epsilon(0.01));
    for (size_t k = 0; k < n / 2; k++) {
        if (k >= bin - 1 && k <= bin + 1) continue;
        CHECK(mag[k] < 1.0f);   // leakage floor: everything else near zero
    }
}

// Silence in, zeros out.
TEST_CASE("desktop audioFft maps silence to all-zero bins") {
    constexpr size_t n = 512;
    static float x[n] = {}, mag[n / 2];
    mm::platform::audioFft(x, n, mag);
    for (size_t k = 0; k < n / 2; k++) CHECK(mag[k] == 0.0f);
}
