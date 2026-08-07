// @module shader
// @also draw, math16

// The shader vocabulary: the GLSL built-ins every per-pixel shader is written out of, in fixed
// point. These pin the CONTRACT each one has to satisfy — the endpoints, the monotonicity, the
// symmetry — because a shader composes them and a wrong edge case shows up as a visible seam
// rather than as a crash.

#include "doctest.h"
#include "light/shader.h"

#include <cmath>

using namespace mm;
using namespace mm::shader;

TEST_CASE("clamp holds a value inside its range") {
    CHECK(clamp(5, 0, 10) == 5);
    CHECK(clamp(-5, 0, 10) == 0);
    CHECK(clamp(50, 0, 10) == 10);
}

TEST_CASE("mix interpolates between its endpoints") {
    CHECK(mix(0, 100, 0) == 0);              // all a
    CHECK(mix(0, 100, 65535) == 99);         // all b, to within one step of fixed point
    CHECK(mix(0, 100, 32768) == 50);         // halfway
    CHECK(mix(100, 0, 32768) == 50);         // and symmetric the other way
}

// fract is what makes a pattern repeat: scaling then taking the fraction tiles a design with no
// branch and no modulo, so the same shader draws one shape or a thousand.
TEST_CASE("fract keeps only the fractional part, and wraps") {
    CHECK(fract(0) == 0);
    CHECK(fract(32768) == 32768);            // half
    CHECK(fract(65536) == 0);                // exactly one wraps back to zero
    CHECK(fract(65536 + 32768) == 32768);    // one and a half
    CHECK(fract(65536 * 5) == 0);            // any whole number
}

TEST_CASE("step is a hard threshold at the edge") {
    CHECK(step(100, 99) == 0);
    CHECK(step(100, 100) == 65535);
    CHECK(step(100, 101) == 65535);
}

// smoothstep is the anti-aliasing workhorse: a hard edge run through it becomes a soft one of
// controllable width, which is how a shader avoids jaggies without supersampling.
TEST_CASE("smoothstep ramps smoothly between its edges") {
    CHECK(smoothstep(0, 65536, 0) == 0);
    CHECK(smoothstep(0, 65536, 65536) == 65535);
    const int mid = smoothstep(0, 65536, 32768);
    CHECK(mid > 32000);
    CHECK(mid < 33500);                      // passes through the centre
}

TEST_CASE("smoothstep is flat at both ends, steep in the middle") {
    // The defining property: zero derivative at the edges, which is what removes the visible crease
    // a linear ramp leaves.
    const int nearStart = smoothstep(0, 65536, 6553) - smoothstep(0, 65536, 0);
    const int nearMid   = smoothstep(0, 65536, 36044) - smoothstep(0, 65536, 29491);
    CHECK(nearMid > nearStart * 2);
}

TEST_CASE("smoothstep clamps outside its edges rather than running away") {
    CHECK(smoothstep(1000, 2000, -50000) == 0);
    CHECK(smoothstep(1000, 2000, 50000) == 65535);
    CHECK(smoothstep(500, 500, 400) == 0);       // zero-width edge is a hard step
    CHECK(smoothstep(500, 500, 600) == 65535);
}

TEST_CASE("length measures distance from the origin") {
    CHECK(length(draw::toSub(3), draw::toSub(4)) == draw::toSub(5));   // the 3-4-5 triangle
    CHECK(length(0, 0) == 0);
}

// Rotating the COORDINATE is how a shader spins a whole design, however complex, for one operation.
TEST_CASE("rotate turns a point about the origin") {
    draw::pos_t x = draw::toSub(10), y = 0;
    rotate(x, y, 16384);                     // a quarter turn
    CHECK(x > -300); CHECK(x < 300);         // was on +x, now near zero
    CHECK(y > draw::toSub(9));               // and out on +y
}

TEST_CASE("a full turn returns a point to where it started") {
    draw::pos_t x = draw::toSub(7), y = draw::toSub(3);
    for (int i = 0; i < 4; i++) rotate(x, y, 16384);
    CHECK(std::abs(x - draw::toSub(7)) < 40);
    CHECK(std::abs(y - draw::toSub(3)) < 40);
}

// uv is the mapping every shader starts from; getting it wrong is why a design stretches on a
// non-square panel. The SHORT side spans -1..1 so a circle stays a circle.
TEST_CASE("uv centres the grid and scales by the short side") {
    int32_t x, y;
    uv(8, 8, 16, 16, x, y);                  // centre of a square grid
    CHECK(std::abs(x) < 8000);
    CHECK(std::abs(y) < 8000);

    uv(0, 8, 16, 16, x, y);                  // left edge
    CHECK(x < -60000);
    uv(15, 8, 16, 16, x, y);                 // right edge
    CHECK(x > 60000);
}

TEST_CASE("uv keeps a circle circular on a wide panel") {
    int32_t x1, y1, x2, y2;
    // On a 32x8 panel the SHORT side (8) sets the scale, so y spans -1..1 and x runs wider.
    uv(16, 0, 32, 8, x1, y1);                // top middle
    uv(16, 7, 32, 8, x2, y2);                // bottom middle
    CHECK(y1 < 0);
    CHECK(y2 > 0);
    CHECK(std::abs(y1 + y2) < 8000);         // symmetric about the centre
}

// repeat is the operator that makes one shape into a lattice — the space folds, the objects do not
// multiply, so a thousand of them cost the same as one.
TEST_CASE("repeat tiles space into cells") {
    CHECK(repeat(0, 100) == -50);            // cell-relative, centred on zero
    CHECK(repeat(50, 100) == 0);
    CHECK(repeat(150, 100) == 0);            // the next cell looks identical
    CHECK(repeat(250, 100) == 0);
    CHECK(repeat(-50, 100) == 0);            // and negative space too
}

TEST_CASE("repeat with a zero cell leaves the coordinate alone") {
    CHECK(repeat(1234, 0) == 1234);
}

TEST_CASE("mirror folds space about the origin") {
    CHECK(mirror(-42) == 42);
    CHECK(mirror(42) == 42);
}

// The SDF operators: given two shapes as distances, produce a third. This is why an SDF scene is
// composed rather than drawn.
TEST_CASE("the SDF operators combine shapes") {
    // Two overlapping shapes: a is inside by 10, b is outside by 5.
    CHECK(opUnion(-10, 5) == -10);           // inside either: the nearer surface wins
    CHECK(opIntersect(-10, 5) == 5);         // inside both: only where they overlap
    // Quilez's order: opSubtract(d1, d2) cuts d1 OUT OF d2. Following his order matters because the
    // op* names are borrowed from his catalog — reversing it silently inverts a transcribed scene.
    CHECK(opSubtract(-5, -10) == 5);         // cut the shallow shape out of the deeper one
}

TEST_CASE("opShell hollows a shape into an outline") {
    CHECK(opShell(0, 100) == -100);          // exactly on the surface: inside the shell
    CHECK(opShell(-500, 100) == 400);        // deep inside: outside the shell
    CHECK(opShell(500, 100) == 400);         // far outside: also outside the shell
}

TEST_CASE("opRound grows a shape outward") {
    CHECK(opRound(100, 30) == 70);           // the surface moves out by r
}

TEST_CASE("rounding a box pulls its corner in") {
    const draw::pos_t c = draw::toSub(8), b = draw::toSub(4), r = draw::toSub(2);
    // AT the sharp box's corner (8+4, 8+4) the sharp form reads exactly 0. The rounded form's
    // surface has been pulled inward there, so the same point is now OUTSIDE it.
    const int32_t sharp   = draw::sdBox(draw::toSub(12), draw::toSub(12), c, c, b, b);
    const int32_t rounded = sdRoundBox(draw::toSub(12), draw::toSub(12), c, c, b, b, r);
    CHECK(sharp == 0);
    CHECK(rounded > 0);

    // Along an AXIS the two agree: rounding only affects the corners.
    const int32_t sharpAxis   = draw::sdBox(draw::toSub(12), c, c, c, b, b);
    const int32_t roundedAxis = sdRoundBox(draw::toSub(12), c, c, c, b, b, r);
    CHECK(std::abs(sharpAxis - roundedAxis) < 20);
}

TEST_CASE("a polygon is negative inside and positive outside") {
    const draw::pos_t c = draw::toSub(16), r = draw::toSub(6);
    CHECK(sdPolygon(c, c, c, c, r, 6) < 0);                       // dead centre
    CHECK(sdPolygon(draw::toSub(30), c, c, c, r, 6) > 0);         // well outside
}

TEST_CASE("a polygon with too few sides falls back to a circle") {
    const draw::pos_t c = draw::toSub(10), r = draw::toSub(4);
    CHECK(sdPolygon(draw::toSub(14), c, c, c, r, 2) ==
          draw::sdCircle(draw::toSub(14), c, c, c, r));
}

// The cosine palette carries a whole colour ramp as twelve numbers rather than a table.
TEST_CASE("a cosine palette produces varied colours around its ramp") {
    const RGB a = cosPalette(0, 128, 128, 128, 127, 127, 127, 1, 1, 1, 0, 85, 170);
    const RGB b = cosPalette(32768, 128, 128, 128, 127, 127, 127, 1, 1, 1, 0, 85, 170);
    CHECK((a.r != b.r || a.g != b.g || a.b != b.b));
}

TEST_CASE("mixing colours interpolates each channel") {
    const RGB a{0, 0, 0}, b{200, 100, 50};
    CHECK(mixColor(a, b, 0).r == 0);
    const RGB mid = mixColor(a, b, 32768);
    CHECK(mid.r > 90); CHECK(mid.r < 110);
}

// The runner is what makes an effect a SHADER: one function of position and time, and the framework
// does the loop, the mapping and the write.
TEST_CASE("the shader runner visits every pixel") {
    Buffer buf;
    buf.allocate(8 * 8, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 8, 8, 1);
    int visits = 0;
    each(cv, 0, [&](int32_t, int32_t, angle16) { visits++; return RGB{10, 20, 30}; });
    CHECK(visits == 64);
    for (int i = 0; i < 64; i++) CHECK(buf.data()[i * 3] == 10);
}

TEST_CASE("the shader runner gives each pixel a different coordinate") {
    Buffer buf;
    buf.allocate(4 * 4, 3);
    buf.clear();
    const draw::Canvas cv = draw::Canvas::of(buf, 4, 4, 1);
    int32_t firstX = 0, lastX = 0;
    int n = 0;
    each(cv, 0, [&](int32_t sx, int32_t, angle16) {
        if (n == 0) firstX = sx;
        lastX = sx;
        n++;
        return RGB{1, 1, 1};
    });
    CHECK(firstX < lastX);                   // coordinates advance across the grid
}
