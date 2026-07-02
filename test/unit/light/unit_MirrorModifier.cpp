// @module MirrorModifier

#include "doctest.h"
#include "light/modifiers/MirrorModifier.h"

// MirrorModifier folds the far half of the logical box back onto the near half per
// axis. modifyLogicalSize halves each mirrored axis (rounding up: ceil(size/2) =
// (size+1)/2, so an odd extent keeps its unpaired centre column); modifyLogical then
// reflects any coordinate at or past the half-extent back via `half*2 - 1 - pos`, so a
// far-half physical light and its near-half mirror share one logical source.

// The mirrored (halved) box size for a given physical box.
static mm::Coord3D mirrorSize(mm::MirrorModifier& m, mm::Coord3D box) {
    m.modifyLogicalSize(box);
    return box;
}

// Fold a physical coord to its logical source. `box` is the physical box, run through
// modifyLogicalSize first so the modifier stashes its halved box for the const fold.
static mm::Coord3D fold(mm::MirrorModifier& m, mm::lengthType x, mm::lengthType y,
                        mm::lengthType z, mm::Coord3D box) {
    mm::Coord3D logical = box;
    m.modifyLogicalSize(logical);   // stashes the half box
    mm::Coord3D p{x, y, z};
    m.modifyLogical(p);
    return p;
}

// modifyLogicalSize halves each mirrored axis, rounding up: an even 128 → 64, an odd
// 65 → 33 (the centre column stays unpaired).
TEST_CASE("MirrorModifier halves each mirrored axis, rounding up") {
    mm::MirrorModifier m;   // all three axes mirror by default
    CHECK(mirrorSize(m, {128, 64, 4}) == mm::Coord3D{64, 32, 2});
    CHECK(mirrorSize(m, {65, 65, 65}) == mm::Coord3D{33, 33, 33});
}

// A coord and its mirror across the box centre map to the same logical position: on an
// even 8-wide axis, physical 7 folds to 0, 6 to 1, 5 to 2, 4 to 3, while the near half
// 0..3 passes through unchanged.
TEST_CASE("MirrorModifier maps a coord and its mirror to the same logical position") {
    mm::MirrorModifier m;
    m.mirrorY = m.mirrorZ = false;   // isolate the x fold
    const mm::Coord3D box{8, 1, 1};

    for (mm::lengthType x = 0; x < 4; x++) {
        CHECK(fold(m, x, 0, 0, box).x == x);              // near half passes through
        CHECK(fold(m, 7 - x, 0, 0, box).x == x);          // far half reflects onto it
    }
}

// An odd extent keeps its centre column unpaired: on a 5-wide axis the half-extent is 3,
// so 0,1,2 pass through and the far edge 4→1, 3→2, leaving logical column 2 unpaired.
TEST_CASE("MirrorModifier leaves the centre column unpaired on an odd axis") {
    mm::MirrorModifier m;
    m.mirrorY = m.mirrorZ = false;
    const mm::Coord3D box{5, 1, 1};
    CHECK(mirrorSize(m, box).x == 3);           // (5+1)/2

    CHECK(fold(m, 0, 0, 0, box).x == 0);
    CHECK(fold(m, 1, 0, 0, box).x == 1);
    CHECK(fold(m, 2, 0, 0, box).x == 2);        // unpaired centre column
    CHECK(fold(m, 3, 0, 0, box).x == 2);        // far edge folds onto the centre
    CHECK(fold(m, 4, 0, 0, box).x == 1);        // half*2-1-4 = 3*2-1-4 = 1
}

// A disabled axis is left untouched: with only Y mirrored, X and Z sizes are unchanged
// and X coordinates pass through while Y folds.
TEST_CASE("MirrorModifier leaves a disabled axis untouched") {
    mm::MirrorModifier m;
    m.mirrorX = false; m.mirrorY = true; m.mirrorZ = false;
    const mm::Coord3D box{8, 8, 4};
    CHECK(mirrorSize(m, box) == mm::Coord3D{8, 4, 4});   // only y halves

    CHECK(fold(m, 7, 0, 0, box).x == 7);        // x untouched (not mirrored)
    CHECK(fold(m, 0, 7, 0, box).y == 0);        // y=7 folds onto near half (half*2-1-7 = 0)
    CHECK(fold(m, 3, 3, 2, box).z == 2);        // z untouched
}

// Degenerate axes don't crash: a 1-wide axis stays 1 ((1+1)/2 == 1) and no coordinate
// ever reaches the half-extent to fold; a 0-extent axis stays 0.
TEST_CASE("MirrorModifier handles degenerate grids") {
    mm::MirrorModifier m;
    CHECK(mirrorSize(m, {1, 1, 1}) == mm::Coord3D{1, 1, 1});
    CHECK(fold(m, 0, 0, 0, {1, 1, 1}) == mm::Coord3D{0, 0, 0});   // no fold on a 1x1x1
    CHECK(mirrorSize(m, {0, 0, 0}) == mm::Coord3D{0, 0, 0});      // 0x0x0 doesn't crash
}
