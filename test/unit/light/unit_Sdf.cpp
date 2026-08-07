// @module draw
// @also math16

// Signed distance fields: negative inside a shape, zero on its edge, positive outside. The sign and
// the ordering are the contract every consumer relies on — a fill tests `d <= 0`, an outline tests
// `|d| - w`, anti-aliasing ramps across `d`, and `smin` blends two of them. These tests pin that
// contract rather than specific pixel values, because the value is only meaningful relative to zero.

#include "doctest.h"
#include "light/draw.h"

using namespace mm;
using draw::toSub;

// The squared form is the one effects reach for by default (measured: ~14 cycles/pixel against ~108
// for the sqrt on an ESP32-S3), so its sign contract matters most.
TEST_CASE("sdCircleSq is negative inside, zero on the rim, positive outside") {
    const draw::pos_t cx = toSub(10), cy = toSub(10), r = toSub(4);
    CHECK(draw::sdCircleSq(cx, cy, cx, cy, r) < 0);                    // dead centre
    CHECK(draw::sdCircleSq(toSub(13), cy, cx, cy, r) < 0);             // just inside
    CHECK(draw::sdCircleSq(toSub(14), cy, cx, cy, r) == 0);            // exactly on the rim
    CHECK(draw::sdCircleSq(toSub(15), cy, cx, cy, r) > 0);             // outside
}

TEST_CASE("sdCircle returns a true distance in sub-pixel units") {
    const draw::pos_t cx = toSub(20), cy = toSub(20), r = toSub(5);
    CHECK(draw::sdCircle(cx, cy, cx, cy, r) == -r);                    // centre is r from the edge
    CHECK(draw::sdCircle(toSub(25), cy, cx, cy, r) == 0);              // on the rim
    // Three pixels beyond the rim reads as three pixels of distance (allowing integer rounding).
    const int32_t d = draw::sdCircle(toSub(28), cy, cx, cy, r);
    CHECK(d > toSub(3) - 8);
    CHECK(d < toSub(3) + 8);
}

// The dimension-generic claim in concrete form: the same expression is a circle here and a sphere in
// a volume, because only the length term changes. A grid of samples must agree with the radius.
TEST_CASE("a circle SDF describes the same shape a radius test would") {
    const draw::pos_t cx = toSub(8), cy = toSub(8), r = toSub(3);
    for (lengthType y = 0; y < 16; y++)
        for (lengthType x = 0; x < 16; x++) {
            const int dx = x - 8, dy = y - 8;
            const bool insideByRadius = (dx * dx + dy * dy) < 9;       // r = 3 px
            const bool insideBySdf = draw::sdCircleSq(toSub(x), toSub(y), cx, cy, r) < 0;
            CHECK(insideBySdf == insideByRadius);
        }
}

TEST_CASE("sdBox is negative inside and grows with distance outside") {
    const draw::pos_t cx = toSub(10), cy = toSub(10), bx = toSub(4), by = toSub(2);
    CHECK(draw::sdBox(cx, cy, cx, cy, bx, by) < 0);                    // centre
    CHECK(draw::sdBox(toSub(13), cy, cx, cy, bx, by) < 0);             // inside along x
    CHECK(draw::sdBox(toSub(14), cy, cx, cy, bx, by) == 0);            // on the face
    const int32_t near = draw::sdBox(toSub(16), cy, cx, cy, bx, by);
    const int32_t far  = draw::sdBox(toSub(20), cy, cx, cy, bx, by);
    CHECK(near > 0);
    CHECK(far > near);                                                 // monotone with distance
}

// A box's half-extents are independent per axis, which is what makes it a rectangle rather than a
// square — the bar/rect primitives build on this.
TEST_CASE("sdBox respects different half-extents per axis") {
    const draw::pos_t cx = toSub(10), cy = toSub(10), bx = toSub(6), by = toSub(1);
    CHECK(draw::sdBox(toSub(15), cy, cx, cy, bx, by) < 0);             // 5 px along the wide axis: inside
    CHECK(draw::sdBox(cx, toSub(15), cx, cy, bx, by) > 0);             // 5 px along the narrow axis: outside
}

TEST_CASE("sdSegment measures distance to the nearest point on the line") {
    const draw::pos_t ax = toSub(2), ay = toSub(5), bx = toSub(12), by = toSub(5);
    const draw::pos_t th = toSub(1);
    CHECK(draw::sdSegment(toSub(7), toSub(5), ax, ay, bx, by, th) < 0);   // on the line, inside the thickness
    CHECK(draw::sdSegment(toSub(7), toSub(9), ax, ay, bx, by, th) > 0);   // well above it
    // Past the end, the distance is measured to the endpoint (a capsule, not an infinite line).
    CHECK(draw::sdSegment(toSub(20), toSub(5), ax, ay, bx, by, th) > 0);
    const int32_t beyond = draw::sdSegment(toSub(20), toSub(5), ax, ay, bx, by, th);
    const int32_t further = draw::sdSegment(toSub(30), toSub(5), ax, ay, bx, by, th);
    CHECK(further > beyond);
}

TEST_CASE("sdSegment treats a zero-length segment as a point") {
    const draw::pos_t a = toSub(5);
    CHECK(draw::sdSegment(a, a, a, a, a, a, toSub(1)) < 0);             // no divide by zero
    CHECK(draw::sdSegment(toSub(9), a, a, a, a, a, toSub(1)) > 0);
}

// smin is what makes SDFs worth having over a rasteriser: two shapes merge into one form instead of
// simply overlapping.
TEST_CASE("smin with zero blend is a plain minimum") {
    CHECK(draw::smin(10, 20, 0) == 10);
    CHECK(draw::smin(-5, 3, 0) == -5);
}

TEST_CASE("smin pulls two nearby shapes together below either alone") {
    const int32_t a = 100, b = 120, k = toSub(2);
    const int32_t blended = draw::smin(a, b, k);
    CHECK(blended < a);                        // the merge dips below the nearer surface...
    CHECK(blended > a - k);                    // ...but only within the blend radius
}

// A large blend radius is reachable from a control on a big fixture, and the intermediate
// `k * h * (256 - h)` overflows int32 past ~131000 sub-units. An overflow makes smin return MORE
// than both inputs, inverting the blend — so the invariant to pin is that it never exceeds the
// smaller input. Found by review.
// Two equal distances put the blend at its deepest, where `k * h * (256 - h)` is largest — the term
// that overflows int32 once k passes ~131000 sub-units (512 pixels), a radius a control reaches on a
// large fixture. At h = 128 the dip is exactly k/4, so the exact value is what catches a wrap: the
// 32-bit form returned a dip of 9464 at k=300000 where k/4 is 75000, and wandered rather than grew
// (34464, 59464, then back to 18928). Found by review.
TEST_CASE("smin digs exactly a quarter of the blend radius, at any radius") {
    for (int32_t k = 100000; k <= 2000000; k += 100000) {
        CAPTURE(k);
        const int32_t dip = 1000 - draw::smin(1000, 1000, k);
        CHECK(dip == k / 4);                    // the closed form, unaffected by any wrap
    }
}

TEST_CASE("smin leaves distant shapes alone") {
    const int32_t a = 10, b = 100000, k = toSub(1);
    CHECK(draw::smin(a, b, k) == a);           // far apart: no blending, just the nearer one
}

// Coverage is the anti-aliasing an SDF gives for free: the edge pixel is lit in proportion to how
// much of it the shape covers, which is what stops a curve reading as a staircase.
TEST_CASE("coverage ramps across the edge rather than switching") {
    CHECK(draw::coverage(-draw::kSubOne * 2) == 255);       // well inside: fully lit
    CHECK(draw::coverage(draw::kSubOne * 2) == 0);          // well outside: dark
    const uint8_t onEdge = draw::coverage(0);
    CHECK(onEdge > 100);
    CHECK(onEdge < 155);                                    // ~half lit exactly on the boundary
    // Monotone: moving outward never brightens.
    CHECK(draw::coverage(-128) > draw::coverage(0));
    CHECK(draw::coverage(0) > draw::coverage(128));
}

TEST_CASE("coverage with a zero-width edge is a hard threshold") {
    CHECK(draw::coverage(-1, 0) == 255);
    CHECK(draw::coverage(1, 0) == 0);
}

// The composition the SDF family exists for: a shape, an outline of it, and a glow, all read off the
// same distance without a second algorithm.
TEST_CASE("one distance yields a fill, an outline and a falloff") {
    const draw::pos_t cx = toSub(8), cy = toSub(8), r = toSub(4);
    const int32_t dIn = draw::sdCircle(toSub(8), toSub(8), cx, cy, r);   // centre
    const int32_t dEdge = draw::sdCircle(toSub(12), toSub(8), cx, cy, r); // on the rim
    const int32_t dOut = draw::sdCircle(toSub(15), toSub(8), cx, cy, r);  // outside

    CHECK(draw::coverage(dIn) == 255);                       // fill
    CHECK(draw::coverage(dOut) == 0);
    // Outline: |d| - width is negative only in a band around the edge.
    const int32_t w = toSub(1);
    CHECK((dEdge < 0 ? -dEdge : dEdge) - w < 0);             // the rim is in the band
    CHECK((dIn < 0 ? -dIn : dIn) - w > 0);                   // the centre is not
}
