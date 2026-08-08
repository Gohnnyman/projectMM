#pragma once

#include <cstdint>

// Value noise: a smooth, deterministic pseudo-random field, the staple "organic motion"
// source for LED effects. inoise8 returns a 0..255 value that varies smoothly across space,
// so neighbouring coordinates give similar values (unlike a raw hash). Sample it across a
// grid for clouds/plasma/fire-like fields; scroll a coordinate (or pass a time offset) to
// animate. 1D, 2D and 3D variants share one hash + a smoothstep interpolation.
//
// **This is VALUE noise, and FastLED's `inoise8` is GRADIENT noise.** The name is kept because it is
// the one every LED effect author reaches for and the 0..255 contract matches, but the character
// differs: value noise reads blockier and more axis-aligned, and uses the full output range where
// FastLED's compresses toward the middle. A ported effect will therefore LOOK slightly different for
// a reason its author cannot see from the call site, which is why it is stated here rather than left
// to be discovered. Prior art: Ken Perlin's method by way of FastLED; the hash + smoothstep + lerp
// are ours, promoted from NoiseEffect so every effect shares one field generator.
//
// Coordinates are 16.0 fixed scaled however the caller likes — the high byte selects the
// noise CELL, the low byte the interpolation position within it. So a larger coordinate step
// per pixel = finer noise (more cells across the grid); a smaller step = broader, smoother.

namespace mm {
namespace noise {

// Integer hash → 0..255. Three coords (z=0 for 1D/2D) feed one well-mixed avalanche.
constexpr uint8_t hash(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 1619u + y * 31337u + z * 6271u;
    h = (h >> 13) ^ h;
    h = h * (h * h * 60493u + 19990303u) + 1376312589u;
    return static_cast<uint8_t>((h >> 16) & 0xFF);
}

// Smoothstep 3t²−2t³ on 0..255 — turns the linear cell fraction into an eased one so the
// field has no hard creases at cell boundaries (the difference between value noise and a
// blocky grid).
constexpr uint8_t smoothstep(uint8_t t) {
    uint16_t t2 = static_cast<uint16_t>(t) * t / 255;
    uint16_t t3 = static_cast<uint16_t>(t2) * t / 255;
    return static_cast<uint8_t>((3 * t2 - 2 * t3) & 0xFF);
}

// Linear interpolate a→b by t/255.
constexpr uint8_t lerp8(uint8_t a, uint8_t b, uint8_t t) {
    int16_t delta = static_cast<int16_t>(b) - static_cast<int16_t>(a);
    return static_cast<uint8_t>(static_cast<int16_t>(a) + delta * t / 255);
}

/// The 16-bit tier's internals. The 8-bit forms above quantise at EVERY stage — the corner values,
/// the interpolation and the octave sum — so a gradient that should be smooth steps visibly once a
/// fixture is large. These are the same three functions at 16 bits.
constexpr uint16_t hash16(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<uint16_t>((h ^ (h >> 16)) & 0xFFFF);
}

/// Smoothstep at 16 bits: 3t² − 2t³, the standard ease that removes the linear kink between cells.
constexpr uint16_t smoothstep16(uint16_t t) {
    const uint32_t t1 = t;
    const uint32_t t2 = (t1 * t1) >> 16;
    const uint32_t t3 = (t2 * t1) >> 16;
    const uint32_t v = 3u * t2 - 2u * t3;
    return static_cast<uint16_t>(v > 65535u ? 65535u : v);
}

/// Linear interpolate a→b by t/65535.
constexpr uint16_t lerp16(uint16_t a, uint16_t b, uint16_t t) {
    const int32_t delta = static_cast<int32_t>(b) - static_cast<int32_t>(a);
    // 64-bit intermediate: `delta * t` reaches 4.29e9 against an INT32_MAX of 2.15e9, so the 32-bit
    // form was signed overflow (undefined behaviour) on roughly a quarter of samples. It happened to
    // produce the right low bits on wrap-around hardware, which is what kept it invisible.
    return static_cast<uint16_t>(static_cast<int32_t>(a)
                                 + static_cast<int32_t>((static_cast<int64_t>(delta) * t) >> 16));
}

}  // namespace noise

// 1D value noise: x is a 16.0 fixed coordinate (high byte = cell, low byte = position).
constexpr uint8_t inoise8(uint32_t x) {
    const uint32_t ix = x >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    return noise::lerp8(noise::hash(ix, 0, 0), noise::hash(ix + 1, 0, 0), fx);
}

// 2D value noise with bilinear interpolation over the 4 cell corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y) {
    const uint32_t ix = x >> 8, iy = y >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    const uint8_t fy = noise::smoothstep(static_cast<uint8_t>(y & 0xFF));
    const uint8_t v00 = noise::hash(ix,     iy,     0);
    const uint8_t v10 = noise::hash(ix + 1, iy,     0);
    const uint8_t v01 = noise::hash(ix,     iy + 1, 0);
    const uint8_t v11 = noise::hash(ix + 1, iy + 1, 0);
    return noise::lerp8(noise::lerp8(v00, v10, fx), noise::lerp8(v01, v11, fx), fy);
}

// 3D value noise with trilinear interpolation over the 8 cube corners.
constexpr uint8_t inoise8(uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t ix = x >> 8, iy = y >> 8, iz = z >> 8;
    const uint8_t fx = noise::smoothstep(static_cast<uint8_t>(x & 0xFF));
    const uint8_t fy = noise::smoothstep(static_cast<uint8_t>(y & 0xFF));
    const uint8_t fz = noise::smoothstep(static_cast<uint8_t>(z & 0xFF));
    const uint8_t v000 = noise::hash(ix,     iy,     iz);
    const uint8_t v100 = noise::hash(ix + 1, iy,     iz);
    const uint8_t v010 = noise::hash(ix,     iy + 1, iz);
    const uint8_t v110 = noise::hash(ix + 1, iy + 1, iz);
    const uint8_t v001 = noise::hash(ix,     iy,     iz + 1);
    const uint8_t v101 = noise::hash(ix + 1, iy,     iz + 1);
    const uint8_t v011 = noise::hash(ix,     iy + 1, iz + 1);
    const uint8_t v111 = noise::hash(ix + 1, iy + 1, iz + 1);
    const uint8_t z0 = noise::lerp8(noise::lerp8(v000, v100, fx), noise::lerp8(v010, v110, fx), fy);
    const uint8_t z1 = noise::lerp8(noise::lerp8(v001, v101, fx), noise::lerp8(v011, v111, fx), fy);
    return noise::lerp8(z0, z1, fz);
}

// --- 16-bit noise ---------------------------------------------------------------------------
//
// The same value noise at 16 bits. math16.h states why the tier exists: 256 levels band visibly on
// a large fixture, so everything an effect writes against is 16-bit. The 8-bit forms above stay for
// the cases where a byte is what the caller needs anyway (a palette index, a brightness).

/// 1D value noise at 16 bits. `x` is 16.16 fixed point: the whole part selects the cell, the
/// fraction interpolates within it.
constexpr uint16_t inoise16(uint32_t x) {
    const uint32_t ix = x >> 16;
    const uint16_t fx = noise::smoothstep16(static_cast<uint16_t>(x & 0xFFFF));
    return noise::lerp16(noise::hash16(ix, 0, 0), noise::hash16(ix + 1, 0, 0), fx);
}

/// 2D value noise at 16 bits — the common case for a panel.
constexpr uint16_t inoise16(uint32_t x, uint32_t y) {
    const uint32_t ix = x >> 16, iy = y >> 16;
    const uint16_t fx = noise::smoothstep16(static_cast<uint16_t>(x & 0xFFFF));
    const uint16_t fy = noise::smoothstep16(static_cast<uint16_t>(y & 0xFFFF));
    const uint16_t v00 = noise::hash16(ix,     iy,     0);
    const uint16_t v10 = noise::hash16(ix + 1, iy,     0);
    const uint16_t v01 = noise::hash16(ix,     iy + 1, 0);
    const uint16_t v11 = noise::hash16(ix + 1, iy + 1, 0);
    return noise::lerp16(noise::lerp16(v00, v10, fx), noise::lerp16(v01, v11, fx), fy);
}

/// 3D value noise at 16 bits — z is the axis a 2D effect uses as time, so the field evolves in
/// place instead of scrolling past.
constexpr uint16_t inoise16(uint32_t x, uint32_t y, uint32_t z) {
    const uint32_t ix = x >> 16, iy = y >> 16, iz = z >> 16;
    const uint16_t fx = noise::smoothstep16(static_cast<uint16_t>(x & 0xFFFF));
    const uint16_t fy = noise::smoothstep16(static_cast<uint16_t>(y & 0xFFFF));
    const uint16_t fz = noise::smoothstep16(static_cast<uint16_t>(z & 0xFFFF));
    const uint16_t z0 = noise::lerp16(
        noise::lerp16(noise::hash16(ix, iy, iz), noise::hash16(ix + 1, iy, iz), fx),
        noise::lerp16(noise::hash16(ix, iy + 1, iz), noise::hash16(ix + 1, iy + 1, iz), fx), fy);
    const uint16_t z1 = noise::lerp16(
        noise::lerp16(noise::hash16(ix, iy, iz + 1), noise::hash16(ix + 1, iy, iz + 1), fx),
        noise::lerp16(noise::hash16(ix, iy + 1, iz + 1), noise::hash16(ix + 1, iy + 1, iz + 1), fx), fy);
    return noise::lerp16(z0, z1, fz);
}

/// Fractal Brownian motion at 16 bits: `octaves` samples at doubling frequency, halving amplitude.
inline uint16_t fbm16(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 32768;                     // no octaves: flat mid-field
    // The sum is 64-bit so each octave keeps its full 16 bits. Shifting each sample down by 8 to fit
    // a 32-bit accumulator made the result 8-bit wearing a 16-bit type — measured at 195 distinct
    // values over 20,000 samples, low byte never set — which is exactly the banding this tier is for.
    uint64_t sum = 0;
    uint32_t norm = 0, amp = 32768;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint64_t>(inoise16(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    return norm ? static_cast<uint16_t>(sum / norm) : 32768;
}

// --- Field composition ------------------------------------------------------------------------
//
// One noise sample is a smooth blur; the looks people actually recognise come from COMPOSING
// samples. Three standard compositions cover most of it, and each is a few lines over `inoise8`
// rather than a new field generator:
//
//   fbm      — sum octaves at doubling frequency and halving amplitude. Turns the blur into
//              cloud/terrain/smoke structure: large shapes with fine detail on them.
//   turbulence — the same sum over |noise|, whose creases read as billows and flame.
//   warp     — sample noise at a coordinate that noise itself displaced (domain warping). This
//              is the one that produces the flowing, marbled, liquid look; Iñigo Quilez's
//              "warping" article is the canonical description.
//
// Cost is stated per call because it is the thing that decides whether an effect fits: each
// octave is one `inoise8`, so fbm(3) costs three samples, and warp costs its own samples PLUS the
// field it then samples. On a large fixture that multiplies by pixel count — see the per-target
// budget in the power-function docs before reaching for octaves on a 128x128 wall.

/// Fractal Brownian motion: `octaves` samples at doubling frequency, halving amplitude, returned
/// normalised to 0..255. octaves=1 is plain noise; 3-4 is the usual cloud look.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 128;                       // no octaves: flat mid-field
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y)) * amp;
        norm += amp;
        x <<= 1; y <<= 1;                               // double the frequency
        amp >>= 1;                                      // halve the contribution
    }
    return static_cast<uint8_t>(norm ? sum / norm : 128);
}

/// 3D fbm — the same sum with a z axis, so a 2D effect can use z as time for a field that evolves
/// in place rather than scrolling past.
inline uint8_t fbm8(uint32_t x, uint32_t y, uint32_t z, uint8_t octaves) {
    if (octaves == 0) return 128;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        sum  += static_cast<uint32_t>(inoise8(x, y, z)) * amp;
        norm += amp;
        x <<= 1; y <<= 1; z <<= 1;
        amp >>= 1;
    }
    return static_cast<uint8_t>(norm ? sum / norm : 128);
}

/// Turbulence: fbm over |noise - 128|, which creases the field where it crosses the midpoint. The
/// creases are what read as billowing smoke and flame rather than soft cloud.
inline uint8_t turbulence8(uint32_t x, uint32_t y, uint8_t octaves) {
    if (octaves == 0) return 0;
    uint32_t sum = 0, norm = 0, amp = 128;
    for (uint8_t o = 0; o < octaves && amp > 0; o++) {
        const int16_t v = static_cast<int16_t>(inoise8(x, y)) - 128;
        sum  += static_cast<uint32_t>(v < 0 ? -v : v) * 2u * amp;
        norm += amp;
        x <<= 1; y <<= 1;
        amp >>= 1;
    }
    const uint32_t r = norm ? sum / norm : 0;
    return static_cast<uint8_t>(r > 255 ? 255 : r);
}

/// Domain warp: displace the sample coordinate by a noise field, then sample there. `strength` is
/// how far the displacement reaches, in the same fixed-point units as the coordinates.
///
/// This is the primitive behind the flowing/marbled look: the field stops looking like a texture
/// laid on the grid and starts looking like something moving through it. Two extra samples.
inline uint8_t warp8(uint32_t x, uint32_t y, uint16_t strength, uint8_t octaves = 1) {
    // Offset the two probe fields so the x and y displacements are independent rather than equal
    // (sampling the same field twice would displace everything along one diagonal).
    const int32_t dx = (static_cast<int32_t>(inoise8(x, y)) - 128) * strength / 128;
    const int32_t dy = (static_cast<int32_t>(inoise8(x + 0x9E37u, y + 0x7C15u)) - 128) * strength / 128;
    return fbm8(static_cast<uint32_t>(static_cast<int32_t>(x) + dx),
                static_cast<uint32_t>(static_cast<int32_t>(y) + dy), octaves);
}

}  // namespace mm
