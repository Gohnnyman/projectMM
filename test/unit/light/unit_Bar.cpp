// @module draw
// @also Canvas, GEQEffect, AudioSpectrumEffect

// `bar`, `fillRect` and `rect` — the audio-meter staple. Four effects hand-rolled a bar loop before
// this existed, and what they disagreed on is exactly what these tests pin: which end is the floor,
// how color varies along the run, and what a bar longer than the grid does.

#include "doctest.h"
#include "light/draw.h"

using namespace mm;

namespace {
struct Surface {
    Buffer buf;
    draw::Canvas cv;
    Surface(lengthType w, lengthType h, uint8_t cpl = 3) {
        buf.allocate(static_cast<nrOfLightsType>(w) * h, cpl);
        buf.clear();
        cv = draw::Canvas::of(buf, w, h, 1);
    }
    uint8_t at(lengthType x, lengthType y, uint8_t ch = 0) const {
        return buf.data()[(static_cast<size_t>(y) * cv.dims.x + x) * cv.cpl + ch];
    }
    /// How many lights carry any light at all — the "did it draw the right amount" check.
    int litCount() const {
        int n = 0;
        for (nrOfLightsType i = 0; i < buf.count(); i++) {
            const size_t off = static_cast<size_t>(i) * cv.cpl;
            if (buf.data()[off] || buf.data()[off + 1] || buf.data()[off + 2]) n++;
        }
        return n;
    }
};
const RGB kRed{200, 0, 0};
}  // namespace

// The VU-meter direction: a horizontal bar lit left-to-right, the AudioSpectrum level row.
TEST_CASE("a bar growing right lights exactly its length from the origin") {
    Surface s(8, 4);
    draw::bar(s.cv, 2, 1, 3, draw::Grow::Right, kRed);
    CHECK(s.at(2, 1) == 200);
    CHECK(s.at(3, 1) == 200);
    CHECK(s.at(4, 1) == 200);
    CHECK(s.at(5, 1) == 0);      // stops at len
    CHECK(s.at(1, 1) == 0);      // nothing before the origin
    CHECK(s.litCount() == 3);
}

// The GEQ direction, and the one most likely to be got backwards: row 0 is the TOP of the grid, so a
// bar rising from the floor walks toward DECREASING y.
TEST_CASE("a bar growing up rises toward row zero") {
    Surface s(4, 8);
    draw::bar(s.cv, 1, 7, 3, draw::Grow::Up, kRed);      // from the bottom row
    CHECK(s.at(1, 7) == 200);
    CHECK(s.at(1, 6) == 200);
    CHECK(s.at(1, 5) == 200);
    CHECK(s.at(1, 4) == 0);
    CHECK(s.litCount() == 3);
}

TEST_CASE("bars grow down and left from their origin") {
    Surface down(4, 8);
    draw::bar(down.cv, 0, 1, 3, draw::Grow::Down, kRed);
    CHECK(down.at(0, 1) == 200);
    CHECK(down.at(0, 3) == 200);
    CHECK(down.litCount() == 3);

    Surface left(8, 2);
    draw::bar(left.cv, 5, 0, 3, draw::Grow::Left, kRed);
    CHECK(left.at(5, 0) == 200);
    CHECK(left.at(3, 0) == 200);
    CHECK(left.at(2, 0) == 0);
    CHECK(left.litCount() == 3);
}

// The reason the color is a callback: every real call site varies color ALONG the bar. The index is
// the distance from the origin, which is the number those formulas already computed.
TEST_CASE("a bar colors each cell by its distance from the origin") {
    Surface s(8, 2);
    draw::bar(s.cv, 0, 0, 4, draw::Grow::Right,
              [](lengthType i) { return RGB{static_cast<uint8_t>(10 * (i + 1)), 0, 0}; });
    CHECK(s.at(0, 0) == 10);
    CHECK(s.at(1, 0) == 20);
    CHECK(s.at(2, 0) == 30);
    CHECK(s.at(3, 0) == 40);
}

// Robustness: a magnitude that maps past the grid must not write outside it. This is the case a
// hand-rolled loop gets wrong when the band value is at full scale.
TEST_CASE("a bar longer than the grid stops at the edge") {
    Surface s(4, 4);
    draw::bar(s.cv, 0, 0, 99, draw::Grow::Right, kRed);
    CHECK(s.litCount() == 4);                    // the row, and no more

    Surface up(4, 4);
    draw::bar(up.cv, 0, 3, 99, draw::Grow::Up, kRed);
    CHECK(up.litCount() == 4);                   // the column, and no more
}

TEST_CASE("a bar of zero or negative length draws nothing") {
    Surface s(4, 4);
    draw::bar(s.cv, 1, 1, 0, draw::Grow::Right, kRed);
    draw::bar(s.cv, 1, 1, -3, draw::Grow::Up, kRed);
    CHECK(s.litCount() == 0);                    // silence leaves the grid dark
}

TEST_CASE("a filled rectangle covers its whole area") {
    Surface s(8, 8);
    draw::fillRect(s.cv, 2, 3, 3, 2, kRed);
    CHECK(s.litCount() == 6);                    // 3x2
    CHECK(s.at(2, 3) == 200);
    CHECK(s.at(4, 4) == 200);
    CHECK(s.at(5, 3) == 0);                      // just past the right edge
    CHECK(s.at(2, 5) == 0);                      // just past the bottom
}

TEST_CASE("a filled rectangle colors by row") {
    Surface s(4, 4);
    draw::fillRect(s.cv, 0, 0, 2, 3,
                   [](lengthType row) { return RGB{static_cast<uint8_t>(50 + 50 * row), 0, 0}; });
    CHECK(s.at(0, 0) == 50);
    CHECK(s.at(1, 1) == 100);
    CHECK(s.at(0, 2) == 150);
}

TEST_CASE("a rectangle outline draws its border and leaves the middle dark") {
    Surface s(8, 8);
    draw::rect(s.cv, 1, 1, 4, 3, kRed);
    // Perimeter of a 4x3 box = 2*4 + 2*(3-2) = 10 cells, each written once.
    CHECK(s.litCount() == 10);
    CHECK(s.at(1, 1) == 200);                    // corners
    CHECK(s.at(4, 1) == 200);
    CHECK(s.at(1, 3) == 200);
    CHECK(s.at(4, 3) == 200);
    CHECK(s.at(2, 2) == 0);                      // hollow middle
    CHECK(s.at(3, 2) == 0);
}

// The degenerate rectangles a control value can produce at its extremes.
TEST_CASE("thin rectangles stay single bars rather than doubling back") {
    Surface oneRow(6, 3);
    draw::rect(oneRow.cv, 0, 1, 4, 1, kRed);
    CHECK(oneRow.litCount() == 4);               // one row, not drawn twice

    Surface oneCol(3, 6);
    draw::rect(oneCol.cv, 1, 0, 1, 4, kRed);
    CHECK(oneCol.litCount() == 4);               // one column

    Surface empty(4, 4);
    draw::rect(empty.cv, 1, 1, 0, 5, kRed);
    CHECK(empty.litCount() == 0);                // no width, no rectangle
}

// The dimension-generic rule: a meter on a 1D strip is a normal case, not an error.
TEST_CASE("a bar works on a single-row strip") {
    Surface s(16, 1);
    draw::bar(s.cv, 0, 0, 5, draw::Grow::Right, kRed);
    CHECK(s.litCount() == 5);
}

// Meters run on non-RGB fixtures too; a bar must write only the channels a light has.
TEST_CASE("a bar never spills into the next light on a 1-channel fixture") {
    Surface s(4, 1, 1);
    for (size_t i = 0; i < s.buf.bytes(); i++) s.buf.data()[i] = 0x5A;
    draw::bar(s.cv, 1, 0, 1, draw::Grow::Right, RGB{10, 20, 30});
    CHECK(s.buf.data()[1] == 10);                // the light took what it could hold
    CHECK(s.buf.data()[2] == 0x5A);              // its neighbour is untouched
}
