// @module CircleModifier

#include "doctest.h"
#include "light/modifiers/CircleModifier.h"

// CircleModifier folds every physical light to its Euclidean distance from the box
// centre: a coord becomes (0, distance, 0), so a 2D box collapses to a single column
// of concentric rings. modifyLogicalSize runs the box's own far corner through that
// same fold and then grows every axis by one. Integer centre offsets (box/2), float
// distance truncated back to lengthType — MoonLight's exact geometry.

// Fold a physical coord through a Circle built on `box`; returns the folded coord.
static mm::Coord3D fold(mm::CircleModifier& c, mm::lengthType x, mm::lengthType y,
                        mm::lengthType z, mm::Coord3D box) {
    c.modifyLogicalSize(box);          // stashes the box so the fold reads the centre
    mm::Coord3D p{x, y, z};
    c.modifyLogical(p);
    return p;
}

// The logical size for a given physical box.
static mm::Coord3D circleSize(mm::CircleModifier& c, mm::Coord3D box) {
    c.modifyLogicalSize(box);
    return box;
}

// The box centre folds to the origin (distance 0), and every coord collapses onto the
// single x=0/z=0 column — the box becomes a 1D radial run.
TEST_CASE("CircleModifier folds the centre to the origin and collapses to one column") {
    mm::CircleModifier c;
    const mm::Coord3D box{8, 8, 1};     // centre is (4, 4, 0)

    CHECK(fold(c, 4, 4, 0, box) == mm::Coord3D{0, 0, 0});   // centre → radius 0
    // Every folded coord lives on x=0, z=0 whatever it was.
    for (mm::lengthType y = 0; y < 8; y++)
        for (mm::lengthType x = 0; x < 8; x++) {
            const mm::Coord3D p = fold(c, x, y, 0, box);
            CHECK(p.x == 0);
            CHECK(p.z == 0);
        }
}

// A light's ring is its integer-truncated Euclidean distance from the centre, so two
// lights equidistant from the centre map to the SAME radius — the defining circle
// property. Centre (4,4): (7,4) and (4,7) are both 3 away; (7,7) is sqrt(18)→4.
TEST_CASE("CircleModifier maps equidistant lights to the same ring") {
    mm::CircleModifier c;
    const mm::Coord3D box{8, 8, 1};     // centre (4, 4, 0)

    CHECK(fold(c, 7, 4, 0, box) == mm::Coord3D{0, 3, 0});   // dx=3 → radius 3
    CHECK(fold(c, 4, 7, 0, box) == mm::Coord3D{0, 3, 0});   // dy=3 → radius 3 (same ring)
    CHECK(fold(c, 7, 7, 0, box) == mm::Coord3D{0, 4, 0});   // sqrt(9+9)=4.24 → truncated 4
    CHECK(fold(c, 4, 4, 0, box) == mm::Coord3D{0, 0, 0});   // centre → radius 0
}

// modifyLogicalSize folds the far corner to its distance, then grows every axis by one:
// the logical box is a (radius+1)-tall column with x=1 and z=1. Corner (8,8,1) off
// centre (4,4,0) is sqrt(16+16+1)=5.74→5, so the size is (0+1, 5+1, 0+1).
TEST_CASE("CircleModifier logical size is a radius-tall single column") {
    mm::CircleModifier c;
    CHECK(circleSize(c, {8, 8, 1}) == mm::Coord3D{1, 6, 1});
}

// Effects-run-at-every-grid-size hard rule: a degenerate box never crashes and still
// yields a valid single-column size. 0×0×0 folds to (0,0,0)→+1 = (1,1,1); 1×1×1's
// corner is sqrt(3)→1, so size (1, 2, 1).
TEST_CASE("CircleModifier survives degenerate grids") {
    mm::CircleModifier c;
    CHECK(circleSize(c, {0, 0, 0}) == mm::Coord3D{1, 1, 1});
    CHECK(circleSize(c, {1, 1, 1}) == mm::Coord3D{1, 2, 1});
    CHECK(fold(c, 0, 0, 0, {1, 1, 1}) == mm::Coord3D{0, 0, 0});   // sole light, no crash
}
