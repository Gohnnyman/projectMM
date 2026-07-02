// @module TransposeModifier

#include "doctest.h"
#include "light/modifiers/TransposeModifier.h"

// TransposeModifier swaps a pair of axes of the logical box and every coordinate
// folded through it (a matrix transpose: rows become columns), then optionally
// flips each axis back-to-front. modifyLogicalSize swaps the size fields;
// modifyLogical swaps the matching coordinate fields. It never rejects a coord.

// The transposed box for a given box.
static mm::Coord3D transposedSize(mm::TransposeModifier& t, mm::Coord3D box) {
    t.modifyLogicalSize(box);
    return box;
}

// Fold a coord through the modifier (modifyLogicalSize must run first to stash the
// transposed box for the inverse). Returns the folded pos; transpose never rejects.
static mm::Coord3D fold(mm::TransposeModifier& t, mm::lengthType x, mm::lengthType y,
                        mm::lengthType z, mm::Coord3D box) {
    t.modifyLogicalSize(box);   // stashes the transposed box
    mm::Coord3D p{x, y, z};
    bool kept = t.modifyLogical(p);
    CHECK(kept);                // transpose accepts every coordinate
    return p;
}

// Default (XY on): x and y swap on both the box and the coordinate; z is untouched.
TEST_CASE("TransposeModifier default swaps x and y") {
    mm::TransposeModifier t;
    // A wide box becomes a tall box.
    CHECK(transposedSize(t, {128, 64, 4}) == mm::Coord3D{64, 128, 4});
    // A coordinate's x and y swap; z stays put.
    CHECK(fold(t, 3, 7, 2, {128, 64, 4}) == mm::Coord3D{7, 3, 2});
    // Origin is a fixed point of the swap.
    CHECK(fold(t, 0, 0, 0, {128, 64, 4}) == mm::Coord3D{0, 0, 0});
}

// XZ swaps x and z; YZ swaps y and z. Only the selected pair moves.
TEST_CASE("TransposeModifier swaps the selected axis pair") {
    mm::TransposeModifier xz;
    xz.transposeXY = false; xz.transposeXZ = true;
    CHECK(transposedSize(xz, {8, 4, 2}) == mm::Coord3D{2, 4, 8});   // x<->z
    CHECK(fold(xz, 1, 2, 3, {8, 4, 2}) == mm::Coord3D{3, 2, 1});    // x<->z, y kept

    mm::TransposeModifier yz;
    yz.transposeXY = false; yz.transposeYZ = true;
    CHECK(transposedSize(yz, {8, 4, 2}) == mm::Coord3D{8, 2, 4});   // y<->z
    CHECK(fold(yz, 1, 2, 3, {8, 4, 2}) == mm::Coord3D{1, 3, 2});    // y<->z, x kept
}

// inverse flips an axis back-to-front within the TRANSPOSED box: x -> size.x-1-x.
// With the default XY swap, inverse X flips the (post-swap) x axis, whose span is the
// original box height. On a {128,64,z} box the transposed x span is 64.
TEST_CASE("TransposeModifier inverse flips within the transposed box") {
    mm::TransposeModifier t;   // XY swap on by default
    t.inverseX = true;
    // Transposed box is {64, 128, ...}; x span is 64. Coord (5,7) swaps to (7,5),
    // then x flips against the transposed span: 64 - 7 - 1 = 56.
    CHECK(fold(t, 5, 7, 0, {128, 64, 1}) == mm::Coord3D{56, 5, 0});
    // The transposed-box axis lengths are unchanged by the inverse.
    CHECK(transposedSize(t, {128, 64, 1}) == mm::Coord3D{64, 128, 1});

    // No swap, only inverse Y: y reads back-to-front against the box height.
    mm::TransposeModifier inv;
    inv.transposeXY = false; inv.inverseY = true;
    CHECK(fold(inv, 3, 0, 0, {16, 8, 1}) == mm::Coord3D{3, 7, 0});   // 8 - 0 - 1 = 7
    CHECK(fold(inv, 3, 7, 0, {16, 8, 1}) == mm::Coord3D{3, 0, 0});   // 8 - 7 - 1 = 0
}

// Degenerate boxes don't crash and the swap still applies to whatever extent exists.
TEST_CASE("TransposeModifier handles degenerate boxes") {
    mm::TransposeModifier t;   // XY swap
    CHECK(transposedSize(t, {0, 0, 0}) == mm::Coord3D{0, 0, 0});
    CHECK(transposedSize(t, {1, 1, 1}) == mm::Coord3D{1, 1, 1});
    // A 1-wide, taller box transposes to wide-and-1-tall.
    CHECK(transposedSize(t, {1, 8, 1}) == mm::Coord3D{8, 1, 1});
    CHECK(fold(t, 0, 0, 0, {0, 0, 0}) == mm::Coord3D{0, 0, 0});   // no crash on 0x0x0
}
