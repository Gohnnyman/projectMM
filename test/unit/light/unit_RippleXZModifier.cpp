// @module RippleXZModifier

#include "doctest.h"
#include "light/modifiers/RippleXZModifier.h"

// RippleXZModifier collapses one axis of the logical box to a single plane, so a
// lower-dimensional effect maps identically onto every slice of the physical box.
// modifyLogicalSize sets the collapsed axis extent to 1; modifyLogical folds every
// coordinate on that axis to 0 and never rejects. Defaults: shrink=true, towardsX=true,
// towardsZ=false — X flattened, Z untouched. Y is never collapsed.

// Fold a coord through the modifier; the returned bool is modifyLogical's accept flag.
static bool fold(const mm::RippleXZModifier& r, mm::Coord3D& p) {
    return r.modifyLogical(p);
}

// The size after collapse for a given physical box.
static mm::Coord3D collapsedSize(mm::RippleXZModifier& r, mm::Coord3D box) {
    r.modifyLogicalSize(box);
    return box;
}

// Default collapses X only: size.x becomes 1, Y and Z keep their extent.
TEST_CASE("RippleXZModifier default collapses the X axis to one plane") {
    mm::RippleXZModifier r;
    CHECK(collapsedSize(r, {16, 8, 4}) == mm::Coord3D{1, 8, 4});   // x flattened, y/z kept

    // Every X folds to x=0; Y and Z pass through unchanged; nothing is rejected.
    mm::Coord3D p{7, 3, 2};
    CHECK(fold(r, p));
    CHECK(p == mm::Coord3D{0, 3, 2});
    p = {15, 0, 3};
    CHECK(fold(r, p));
    CHECK(p == mm::Coord3D{0, 0, 3});
}

// towardsZ collapses Z instead; both flags collapse X and Z, leaving Y as the only axis.
TEST_CASE("RippleXZModifier collapses Z, and both X and Z together") {
    mm::RippleXZModifier z;
    z.towardsX = false; z.towardsZ = true;
    CHECK(collapsedSize(z, {16, 8, 4}) == mm::Coord3D{16, 8, 1});   // z flattened, x/y kept
    mm::Coord3D p{5, 3, 2};
    CHECK(fold(z, p));
    CHECK(p == mm::Coord3D{5, 3, 0});   // z folds to 0, x/y pass through

    mm::RippleXZModifier both;
    both.towardsX = true; both.towardsZ = true;
    CHECK(collapsedSize(both, {16, 8, 4}) == mm::Coord3D{1, 8, 1});  // only y survives
    p = {9, 6, 3};
    CHECK(fold(both, p));
    CHECK(p == mm::Coord3D{0, 6, 0});   // x and z both fold to 0
}

// shrink=false is the identity: no axis collapses, no coordinate folds.
TEST_CASE("RippleXZModifier with shrink off is the identity") {
    mm::RippleXZModifier r;
    r.shrink = false;   // towardsX still true, but shrink gates everything
    CHECK(collapsedSize(r, {16, 8, 4}) == mm::Coord3D{16, 8, 4});   // unchanged

    mm::Coord3D p{7, 3, 2};
    CHECK(fold(r, p));
    CHECK(p == mm::Coord3D{7, 3, 2});   // coord untouched
}

// Degenerate boxes don't crash: a 0x0x0 box collapses its X to 1, and folding at the
// origin still accepts and folds x to 0.
TEST_CASE("RippleXZModifier handles a degenerate 0x0x0 box") {
    mm::RippleXZModifier r;
    CHECK(collapsedSize(r, {0, 0, 0}) == mm::Coord3D{1, 0, 0});   // x floored to the plane

    mm::Coord3D p{0, 0, 0};
    CHECK(fold(r, p));
    CHECK(p == mm::Coord3D{0, 0, 0});
}
