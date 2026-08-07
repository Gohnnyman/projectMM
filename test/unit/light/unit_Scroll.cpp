// @module draw
// @also Canvas, FreqMatrixEffect

// `scroll` — the shift register FreqMatrix hand-rolled as a per-pixel copy loop. Two behaviours in
// one primitive: a shift (content falls off the end, the vacated edge goes dark) and a wrap (content
// re-enters the far edge), which is the marquee idiom. The tests pin direction, the vacated edge,
// and that a wrap conserves every light.

#include "doctest.h"
#include "light/draw.h"

using namespace mm;

namespace {
struct Surface {
    Buffer buf;
    draw::Canvas cv;
    Surface(lengthType w, lengthType h, lengthType d = 1, uint8_t cpl = 3) {
        buf.allocate(static_cast<nrOfLightsType>(w) * h * (d > 0 ? d : 1), cpl);
        buf.clear();
        cv = draw::Canvas::of(buf, w, h, d);
    }
    void set(lengthType x, lengthType y, uint8_t v, lengthType z = 0) {
        draw::pixel(cv, {x, y, z}, RGB{v, 0, 0});
    }
    uint8_t at(lengthType x, lengthType y, lengthType z = 0) const {
        return buf.data()[draw::Canvas{cv}.offsetOf({x, y, z})];
    }
    /// Sum of the red channel — a wrap must conserve this, a shift must lose the part that fell off.
    uint32_t total() const {
        uint32_t s = 0;
        for (nrOfLightsType i = 0; i < buf.count(); i++) s += buf.data()[static_cast<size_t>(i) * cv.cpl];
        return s;
    }
};
}  // namespace

TEST_CASE("scrolling right moves content toward increasing x and darkens the vacated edge") {
    Surface s(4, 1);
    s.set(0, 0, 10); s.set(1, 0, 20); s.set(2, 0, 30); s.set(3, 0, 40);
    draw::scroll(s.cv, 0, 1);
    CHECK(s.at(0, 0) == 0);      // vacated
    CHECK(s.at(1, 0) == 10);
    CHECK(s.at(2, 0) == 20);
    CHECK(s.at(3, 0) == 30);     // 40 fell off the end
}

TEST_CASE("scrolling left moves content toward decreasing x") {
    Surface s(4, 1);
    s.set(0, 0, 10); s.set(1, 0, 20); s.set(2, 0, 30); s.set(3, 0, 40);
    draw::scroll(s.cv, 0, -1);
    CHECK(s.at(0, 0) == 20);
    CHECK(s.at(1, 0) == 30);
    CHECK(s.at(2, 0) == 40);
    CHECK(s.at(3, 0) == 0);      // vacated at the far end
}

// The FreqMatrix case: a column shifted one step away from the source end, new content painted at
// the freed row. This is the loop the effect used to write by hand.
TEST_CASE("scrolling down moves each row one step and frees the top row") {
    Surface s(1, 4);
    s.set(0, 0, 10); s.set(0, 1, 20); s.set(0, 2, 30); s.set(0, 3, 40);
    draw::scroll(s.cv, 1, 1);
    CHECK(s.at(0, 0) == 0);      // the row the effect paints into
    CHECK(s.at(0, 1) == 10);
    CHECK(s.at(0, 2) == 20);
    CHECK(s.at(0, 3) == 30);
}

TEST_CASE("scrolling up moves rows toward the top") {
    Surface s(1, 4);
    s.set(0, 0, 10); s.set(0, 1, 20); s.set(0, 2, 30); s.set(0, 3, 40);
    draw::scroll(s.cv, 1, -1);
    CHECK(s.at(0, 0) == 20);
    CHECK(s.at(0, 3) == 0);
}

// A wrap loses nothing: it is a rotation, so the same lights are present in new places.
TEST_CASE("a wrapping scroll conserves every light") {
    Surface s(4, 1);
    s.set(0, 0, 10); s.set(1, 0, 20); s.set(2, 0, 30); s.set(3, 0, 40);
    const uint32_t before = s.total();
    draw::scroll(s.cv, 0, 1, /*wrap=*/true);
    CHECK(s.total() == before);
    CHECK(s.at(0, 0) == 40);     // the far end re-entered
    CHECK(s.at(1, 0) == 10);
    CHECK(s.at(2, 0) == 20);
    CHECK(s.at(3, 0) == 30);
}

TEST_CASE("a wrapping scroll the length of the axis leaves the grid unchanged") {
    Surface s(4, 1);
    s.set(0, 0, 10); s.set(1, 0, 20); s.set(2, 0, 30); s.set(3, 0, 40);
    draw::scroll(s.cv, 0, 4, /*wrap=*/true);
    CHECK(s.at(0, 0) == 10);
    CHECK(s.at(3, 0) == 40);     // a full turn is the identity
}

TEST_CASE("a wrapping column scroll rotates rows") {
    Surface s(1, 4);
    s.set(0, 0, 10); s.set(0, 1, 20); s.set(0, 2, 30); s.set(0, 3, 40);
    draw::scroll(s.cv, 1, 1, /*wrap=*/true);
    CHECK(s.at(0, 0) == 40);
    CHECK(s.at(0, 1) == 10);
    CHECK(s.at(0, 3) == 30);
}

// Overshoot: a shift longer than the axis leaves nothing behind, and must not read out of bounds.
TEST_CASE("a non-wrapping scroll past the end clears the grid") {
    Surface s(4, 2);
    for (lengthType y = 0; y < 2; y++)
        for (lengthType x = 0; x < 4; x++) s.set(x, y, 50);
    draw::scroll(s.cv, 0, 99);
    CHECK(s.total() == 0);
}

TEST_CASE("a scroll of zero steps changes nothing") {
    Surface s(4, 1);
    s.set(0, 0, 10); s.set(3, 0, 40);
    draw::scroll(s.cv, 0, 0);
    CHECK(s.at(0, 0) == 10);
    CHECK(s.at(3, 0) == 40);
}

// Each row scrolls independently: a 2D shift must not drag one row's content into the next.
TEST_CASE("scrolling x on a 2D grid keeps rows separate") {
    Surface s(3, 2);
    s.set(0, 0, 10); s.set(1, 0, 20); s.set(2, 0, 30);   // row 0
    s.set(0, 1, 40); s.set(1, 1, 50); s.set(2, 1, 60);   // row 1
    draw::scroll(s.cv, 0, 1);
    CHECK(s.at(0, 0) == 0);
    CHECK(s.at(1, 0) == 10);
    CHECK(s.at(2, 0) == 20);
    CHECK(s.at(0, 1) == 0);      // row 1 vacated its own edge...
    CHECK(s.at(1, 1) == 40);     // ...and kept its own content
    CHECK(s.at(2, 1) == 50);
}

TEST_CASE("scrolling z moves whole slices in a volume") {
    Surface s(2, 2, 2);
    s.set(0, 0, 10, 0);
    s.set(0, 0, 20, 1);
    draw::scroll(s.cv, 2, 1);
    CHECK(s.at(0, 0, 0) == 0);   // vacated slice
    CHECK(s.at(0, 0, 1) == 10);  // slice 0 moved to slice 1
}

TEST_CASE("scrolling a degenerate axis does nothing") {
    Surface s(4, 1);
    s.set(0, 0, 10);
    draw::scroll(s.cv, 1, 1);    // height is 1: nowhere to move
    CHECK(s.at(0, 0) == 10);
}

// Non-RGB fixtures scroll too; a light must carry exactly its own channels.
TEST_CASE("scrolling a 1-channel fixture moves whole lights") {
    Surface s(4, 1, 1, 1);
    s.buf.data()[0] = 10; s.buf.data()[1] = 20; s.buf.data()[2] = 30; s.buf.data()[3] = 40;
    draw::scroll(s.cv, 0, 1);
    CHECK(s.buf.data()[0] == 0);
    CHECK(s.buf.data()[1] == 10);
    CHECK(s.buf.data()[3] == 30);
}
