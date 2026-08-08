#pragma once

#include "core/math16.h"      // sin16/cos16, atan16, dist16, isqrt, frac16
#include "light/draw.h"       // pos_t, Canvas, the 2D SDF family
#include "light/Palette.h"

// The shader vocabulary: the small set of operations every per-pixel shader is written out of.
//
// A **shader** is one idea — a function that runs once per pixel and returns a colour, given that
// pixel's position and the time. Everything else is composition. What makes shader code portable
// between people is that it is written in a shared vocabulary (GLSL's `mix`, `clamp`, `step`,
// `smoothstep`, `fract`, `length`, plus the signed-distance operators), so an effect reads the same
// way whether it came from Shadertoy or from here.
//
// This header is that vocabulary in fixed point: the 2D operations a shader composes to produce a
// gradient, a pattern, a warped field or a set of shapes. Rendering 3D scenes by marching rays is a
// technique built on top of it, and lives in `raymarch.h`.
//
// **Everything is 16-bit fixed point on 0..65535**, matching `frac16` in math16.h. That is the
// contract: a value is a fraction of one, an SDF distance is in sub-pixel units, and a colour index
// is a byte because palettes are. The 8-bit forms in math8.h stay for genuinely 8-bit domains.
//
// Prior art: the GLSL built-in function set (the names are deliberately the standard ones so a
// reader who knows shaders needs no translation), and Iñigo Quilez's articles on distance functions
// and their operators (iquilezles.org). Written fresh in fixed point.

namespace mm::shader {

// --- The universal built-ins -------------------------------------------------------------------
//
// These five are in essentially every shader ever written. Having them named means effect code
// states its intent rather than open-coding a clamp or a lerp each time.

/// Clamp to a range.
constexpr int32_t clamp(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Linear interpolation from `a` to `b` by `t` (0..65535). GLSL's `mix`.
constexpr int32_t mix(int32_t a, int32_t b, frac16 t) {
    return a + static_cast<int32_t>((static_cast<int64_t>(b - a) * t) >> 16);
}

/// The fractional part of a 16.16 value — GLSL's `fract`, the primitive behind every repeating
/// pattern: `fract(x * n)` tiles a design n times with no branch and no modulo.
constexpr frac16 fract(int32_t v) {
    return static_cast<frac16>(static_cast<uint32_t>(v) & 0xFFFF);
}

/// 0 below the edge, 65535 at or above it — a hard threshold.
constexpr frac16 step(int32_t edge, int32_t v) {
    return v < edge ? 0 : 65535;
}

/// A smooth 0→1 ramp between two edges, with zero derivative at both ends (the classic 3t²−2t³).
/// This is the anti-aliasing workhorse: wherever a shader would produce a hard jaggy edge, running
/// the value through `smoothstep` softens it over a controllable width.
constexpr frac16 smoothstep(int32_t edge0, int32_t edge1, int32_t v) {
    if (edge1 == edge0) return v < edge0 ? 0 : 65535;
    int64_t t = (static_cast<int64_t>(v - edge0) << 16) / (edge1 - edge0);
    t = t < 0 ? 0 : (t > 65535 ? 65535 : t);
    // t*t*(3 - 2t) in 16.16, evaluated so the intermediates stay inside 64 bits.
    const int64_t t2 = (t * t) >> 16;
    const int64_t t3 = (t2 * t) >> 16;
    return static_cast<frac16>(clamp(static_cast<int32_t>(3 * t2 - 2 * t3), 0, 65535));
}

// --- Vectors -----------------------------------------------------------------------------------

/// Length of a 2D vector — GLSL's `length`, in sub-pixel units.
inline int32_t length(draw::pos_t x, draw::pos_t y) {
    return static_cast<int32_t>(dist16(x, y));
}

/// Rotate a point about the origin by `a`. The building block for every spinning pattern: rotate
/// the COORDINATE and the whole design turns, however complex it is.
inline void rotate(draw::pos_t& x, draw::pos_t& y, angle16 a) {
    const int32_t c = static_cast<int32_t>(cos16(a));   // -32768..32767
    const int32_t s = static_cast<int32_t>(sin16(a));
    const int32_t nx = (static_cast<int64_t>(x) * c - static_cast<int64_t>(y) * s) >> 15;
    const int32_t ny = (static_cast<int64_t>(x) * s + static_cast<int64_t>(y) * c) >> 15;
    x = static_cast<draw::pos_t>(nx);
    y = static_cast<draw::pos_t>(ny);
}

/// Convert a pixel to shader space: centred on the grid, scaled so the SHORT side spans -1..1.
/// Every shader wants this, and getting it wrong is why a design stretches on a non-square panel.
/// Returns coordinates in 16.16 where 65536 == 1.0.
inline void uv(lengthType px, lengthType py, lengthType w, lengthType h,
               int32_t& outX, int32_t& outY) {
    const int32_t shortSide = (w < h ? w : h);
    const int32_t s = shortSide > 0 ? shortSide : 1;
    outX = ((static_cast<int32_t>(px) * 2 - w + 1) * 65536) / s;
    outY = ((static_cast<int32_t>(py) * 2 - h + 1) * 65536) / s;
}

// --- Domain operators --------------------------------------------------------------------------
//
// These transform the COORDINATE before the shape is evaluated, which is how one shape becomes a
// pattern. `repeat` is the reason a shader can draw a thousand identical objects for the price of
// one — the objects do not exist, the space folds.

/// Tile space into cells of `size`, returning the position within the cell (centred on zero).
/// One shape drawn through this becomes an infinite grid of that shape.
constexpr int32_t repeat(int32_t v, int32_t size) {
    if (size <= 0) return v;
    int32_t m = v % size;
    if (m < 0) m += size;
    return m - size / 2;
}

/// Mirror space about the origin: `abs`, which turns any shape into a symmetric pair.
constexpr int32_t mirror(int32_t v) { return v < 0 ? -v : v; }

// --- SDF combination -----------------------------------------------------------------------------
//
// The signed-distance operators. Given two shapes as distances, these produce a third shape — which
// is why an SDF scene is built by composition rather than by drawing. `smin` (in draw.h) is the
// smooth version of `opUnion`.

/// Both shapes (the nearer surface wins).
constexpr int32_t opUnion(int32_t a, int32_t b) { return a < b ? a : b; }

/// Only where the shapes overlap.
constexpr int32_t opIntersect(int32_t a, int32_t b) { return a > b ? a : b; }

/// Shape `b` with shape `a` cut out of it — Quilez's `opSubtraction(d1, d2) = max(-d1, d2)`, whose
/// operand order this follows deliberately. Reversing it (cutting b from a, which reads more
/// naturally) silently inverts every scene transcribed from Shadertoy, so the standard order wins.
constexpr int32_t opSubtract(int32_t a, int32_t b) { return (-a) > b ? (-a) : b; }

/// Hollow out a shape, leaving a shell of the given thickness — the outline operator.
constexpr int32_t opShell(int32_t d, int32_t thickness) {
    return (d < 0 ? -d : d) - thickness;
}

/// Grow a shape outward by `r`, rounding its corners as it goes.
constexpr int32_t opRound(int32_t d, int32_t r) { return d - r; }

// --- More shapes ---------------------------------------------------------------------------------
//
// draw.h carries circle/box/segment because effects needed them; these complete the everyday set.

/// A box with rounded corners.
///
/// This computes the EUCLIDEAN corner distance itself rather than shrinking `draw::sdBox` and adding
/// the radius back. That shortcut is the textbook one for a Euclidean box, but `draw::sdBox` returns
/// the CHEBYSHEV distance outside (max of the two axis overshoots — cheaper, and correct for its
/// own purpose), so the shrink and the add cancel exactly and the corners come out just as sharp.
/// Verified: both forms returned an identical 0/256/512 walking out along the diagonal.
inline int32_t sdRoundBox(draw::pos_t px, draw::pos_t py, draw::pos_t cx, draw::pos_t cy,
                          draw::pos_t bx, draw::pos_t by, draw::pos_t r) {
    const int32_t qx = (px - cx < 0 ? cx - px : px - cx) - (bx - r);
    const int32_t qy = (py - cy < 0 ? cy - py : py - cy) - (by - r);
    const int32_t outX = qx > 0 ? qx : 0;
    const int32_t outY = qy > 0 ? qy : 0;
    if (outX > 0 && outY > 0)                       // past a corner: the true radial distance
        return static_cast<int32_t>(dist16(outX, outY)) - r;
    if (outX > 0 || outY > 0)                       // past one face only
        return (outX > outY ? outX : outY) - r;
    return (qx > qy ? qx : qy) - r;                 // fully inside
}

/// A regular n-sided polygon, via the polar fold: reduce the angle into one wedge and the shape
/// becomes a single edge, repeated by symmetry.
inline int32_t sdPolygon(draw::pos_t px, draw::pos_t py, draw::pos_t cx, draw::pos_t cy,
                         draw::pos_t r, uint8_t sides) {
    if (sides < 3) return draw::sdCircle(px, py, cx, cy, r);
    const int32_t dx = px - cx, dy = py - cy;
    const int32_t dist = static_cast<int32_t>(dist16(dx, dy));
    if (dist == 0) return -r;
    // Fold the angle into one wedge, then measure to that wedge's flat edge.
    const angle16 a = atan16(dy, dx);
    const uint32_t wedge = 65536u / sides;
    uint32_t within = a % wedge;
    if (within > wedge / 2) within = wedge - within;      // mirror to the wedge's half
    const int32_t c = cos16(static_cast<angle16>(within));   // signed, per lib8tion
    if (c <= 0) return dist - r;
    // Distance to the edge is the radius divided by cos(angle from the wedge centre).
    const int32_t edge = static_cast<int32_t>((static_cast<int64_t>(r) * 32768) / c);
    return dist - edge;
}

// --- Projection ------------------------------------------------------------------------------
//
// Putting a 3D point on a 2D panel. Three effects hand-roll this today — StarField's 1/z pinhole,
// GEQ3D's converging foreshortening, RubiksCube's voxel-to-face classification — which is the same
// repeated-pattern evidence that justified pulling out `BeatPhase`.
//
// The whole of perspective is one divide: things further away move less and appear smaller, in
// exact proportion to their distance. Everything else (a camera, a rotation, a depth sort) is
// arrangement around that divide.

/// Project a 3D point onto the screen. `z` is depth from the viewer; `fov` scales the result, so a
/// larger value is a longer lens. Returns 16.16 shader-space coordinates via `outX`/`outY`, and the
/// depth for sorting or fading.
///
/// A point at or behind the viewer has no projection — `z <= 0` returns false rather than dividing
/// by zero or wrapping a point behind the camera round to the front, which is the classic artifact.
inline bool project(int32_t x, int32_t y, int32_t z, int32_t fov,
                    int32_t& outX, int32_t& outY) {
    if (z <= 0) return false;                       // at or behind the viewer: nothing to draw
    outX = static_cast<int32_t>((static_cast<int64_t>(x) * fov) / z);
    outY = static_cast<int32_t>((static_cast<int64_t>(y) * fov) / z);
    return true;
}

/// How bright something at depth `z` should be, given the distance where it fades out entirely.
/// Nearer is brighter — the depth cue that makes a projected scene read as having space in it
/// rather than being a flat scatter.
constexpr uint8_t depthFade(int32_t z, int32_t far) {
    if (z <= 0 || far <= 0) return 255;
    if (z >= far) return 0;
    return static_cast<uint8_t>(((far - z) * 255) / far);
}

// --- Colour ---------------------------------------------------------------------------------------

/// Iñigo Quilez's cosine palette: four coefficients generate a whole smooth colour ramp, so a
/// shader carries its palette as numbers rather than a table. `t` is 0..65535 around the ramp.
///
/// colour = a + b * cos(2π(c·t + d)), per channel.
inline RGB cosPalette(frac16 t, uint8_t aR, uint8_t aG, uint8_t aB,
                      uint8_t bR, uint8_t bG, uint8_t bB,
                      uint8_t cR, uint8_t cG, uint8_t cB,
                      uint8_t dR, uint8_t dG, uint8_t dB) {
    const auto ch = [t](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        const angle16 ang = static_cast<angle16>((static_cast<uint32_t>(t) * c) / 64u
                                                 + (static_cast<uint32_t>(d) << 8));
        const int32_t cosv = static_cast<int32_t>(cos16(ang));   // -32768..32767
        const int32_t v = a + ((b * cosv) >> 15);
        return static_cast<uint8_t>(clamp(v, 0, 255));
    };
    return RGB{ch(aR, bR, cR, dR), ch(aG, bG, cG, dG), ch(aB, bB, cB, dB)};
}

/// Blend two colours by `t` — the colour form of `mix`.
inline RGB mixColor(RGB a, RGB b, frac16 t) {
    const uint8_t f = static_cast<uint8_t>(t >> 8);
    return RGB{static_cast<uint8_t>(a.r + (((b.r - a.r) * f) >> 8)),
               static_cast<uint8_t>(a.g + (((b.g - a.g) * f) >> 8)),
               static_cast<uint8_t>(a.b + (((b.b - a.b) * f) >> 8))};
}

// --- The shader runner ---------------------------------------------------------------------------

/// Run `fn(x, y, t)` for every pixel and write what it returns.
///
/// This is what makes an effect a shader rather than a drawing: the effect supplies ONE function of
/// position and time, and the framework handles the loop, the coordinate mapping and the write. The
/// callback receives 16.16 shader-space coordinates (short side spans -1..1) and returns an RGB.
template <typename ShadeFn>
inline void each(const draw::Canvas& cv, angle16 t, ShadeFn fn) {
    const lengthType w = cv.dims.x, h = cv.dims.y;
    for (lengthType py = 0; py < h; py++) {
        for (lengthType px = 0; px < w; px++) {
            int32_t sx, sy;
            uv(px, py, w, h, sx, sy);
            draw::pixel(cv, {px, py, 0}, fn(sx, sy, t));
        }
    }
}

}  // namespace mm::shader
