// @module BlockModifier

#include "doctest.h"
#include "light/modifiers/BlockModifier.h"

// BlockModifier is a 1D->2D remap: every physical light folds to its CHEBYSHEV
// (block) distance from the floor-biased box centre — max(|dx|,|dy|) — written into
// y (pos becomes {0, distance, 0}), so a 1D effect painted along y draws concentric
// SQUARE rings. Z plays no part. modifyLogicalSize folds the box itself the same way
// and grows each axis by one, yielding the {1, maxDistance + 1, 1} logical box.

// The logical box a BlockModifier produces for a given physical box.
static mm::Coord3D blockSize(mm::BlockModifier& b, mm::Coord3D box) {
    b.modifyLogicalSize(box);
    return box;
}

// Fold a physical coord through the modifier. Stashes the box first (modifyLogicalSize
// saves the physical box the const fold reads for its centre), then folds (x,y,z).
static mm::Coord3D fold(mm::BlockModifier& b, mm::lengthType x, mm::lengthType y,
                        mm::lengthType z, mm::Coord3D box) {
    b.modifyLogicalSize(box);   // stashes the physical box
    mm::Coord3D p{x, y, z};
    b.modifyLogical(p);         // never rejects — returns true, we assert the coord
    return p;
}

// The centre light of the box folds to distance 0 (the innermost ring, y=0); a corner
// folds to the largest distance, and z is always cleared to 0.
TEST_CASE("BlockModifier folds to Chebyshev distance from the box centre") {
    mm::BlockModifier b;
    // On a 4x4 box the floor-biased centre is (1,1). The centre light maps to y=0.
    CHECK(fold(b, 1, 1, 0, {4, 4, 1}) == mm::Coord3D{0, 0, 0});
    // Corner (0,0): dx=dy=1 -> distance 1.
    CHECK(fold(b, 0, 0, 0, {4, 4, 1}) == mm::Coord3D{0, 1, 0});
    // Far corner (3,3): dx=dy=2 -> distance 2 (the outermost square ring).
    CHECK(fold(b, 3, 3, 0, {4, 4, 1}) == mm::Coord3D{0, 2, 0});
}

// The distance is the MAXIMUM of |dx| and |dy| (a square ring), not the sum or the
// Euclidean length: an off-diagonal light sits on the ring of its larger axis delta.
TEST_CASE("BlockModifier uses max(|dx|,|dy|) so rings are axis-aligned squares") {
    mm::BlockModifier b;
    // 5x5 box, centre (2,2). Light (4,2): dx=2, dy=0 -> distance 2.
    CHECK(fold(b, 4, 2, 0, {5, 5, 1}) == mm::Coord3D{0, 2, 0});
    // Light (4,3): dx=2, dy=1 -> max is 2, same ring as (4,2) — a square edge, not a
    // circle (Euclidean would differ, sum-of-deltas would give 3).
    CHECK(fold(b, 4, 3, 0, {5, 5, 1}) == mm::Coord3D{0, 2, 0});
    // z is not part of the distance — a light off the plane still folds by x/y only.
    CHECK(fold(b, 4, 2, 3, {5, 5, 4}) == mm::Coord3D{0, 2, 0});
}

// modifyLogicalSize collapses the box to one column, its height = the max block
// distance + 1 (rings from centre to the far corner, inclusive), depth 1.
TEST_CASE("BlockModifier logical box is {1, maxDistance + 1, 1}") {
    mm::BlockModifier b;
    // 4x4: centre (1,1), far corner distance max(|4-1|,|4-1|)=3 -> height 3+1=4.
    CHECK(blockSize(b, {4, 4, 1}) == mm::Coord3D{1, 4, 1});
    // 3x3: centre (1,1), distance max(|3-1|,|3-1|)=2 -> height 3.
    CHECK(blockSize(b, {3, 3, 1}) == mm::Coord3D{1, 3, 1});
    // A wide box takes its larger axis: 8x2 -> centre (3,0), distance max(5,2)=5 -> 6.
    CHECK(blockSize(b, {8, 2, 1}) == mm::Coord3D{1, 6, 1});
}

// Degenerate grids never crash and stay well-formed: 0x0x0 and 1x1x1 both fold and
// size without dividing by zero or producing a zero-height box (the Effects hard rule).
TEST_CASE("BlockModifier survives degenerate grids") {
    mm::BlockModifier b;
    // 1x1x1: single light is the centre -> distance 0; logical box {1,1,1}.
    CHECK(fold(b, 0, 0, 0, {1, 1, 1}) == mm::Coord3D{0, 0, 0});
    CHECK(blockSize(b, {1, 1, 1}) == mm::Coord3D{1, 1, 1});
    // 0x0x0: no crash; the fold and size are still finite (each axis grows by one).
    CHECK_NOTHROW(fold(b, 0, 0, 0, {0, 0, 0}));
    CHECK(blockSize(b, {0, 0, 0}).x >= 1);
    CHECK(blockSize(b, {0, 0, 0}).y >= 1);
    CHECK(blockSize(b, {0, 0, 0}).z >= 1);
}
