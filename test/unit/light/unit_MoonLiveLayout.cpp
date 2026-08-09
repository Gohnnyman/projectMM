// @module MoonLiveLayout
// @also MoonLive, LayoutBase, Layouts

// A scripted layout: where the lights physically are, written as text on a running device instead of
// compiled in as a C++ class. This is the binding that needed `for` — a modifier transforms one
// coordinate because the Layer calls it per light, but a layout has to place N lights itself.
//
// The tests are about the contract a layout owes its container: that lightCount() answers before any
// coordinate is asked for (the Layer sizes its buffer from it), that the count and the coordinates
// agree because they come from the same script, that nothing is allocated to achieve it, and that a
// broken script leaves an empty fixture rather than taking the pipeline down.

#include "doctest.h"
#include "light/moonlive/MoonLiveLayout.h"

#include <cstring>
#include <cstdio>
#include <vector>

using namespace mm;

namespace {
/// Collect what a layout emits, the way the Layer's mapping build does.
std::vector<Coord3D> place(const char* script) {
    MoonLiveLayout l;
    l.defineControls();
    if (script) l.setSource(script);
    l.prepare();

    std::vector<Coord3D> out;
    CoordSink sink{
        [](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
            static_cast<std::vector<Coord3D>*>(ctx)->push_back({x, y, z});
        },
        nullptr, &out};
    l.forEachCoord(sink);
    return out;
}
}  // namespace

TEST_CASE("the default script lays out a grid, one light per cell") {
    // The shape almost every panel is, and the script that ships: a nested loop calling addLight.
    const std::vector<Coord3D> p = place(
        "uint8_t width = 4;  // @control 1..64\n"
        "uint8_t height = 2; // @control 1..64\n"
        "for (yy = 0; yy < height; yy = yy + 1) {"
        "  for (xx = 0; xx < width; xx = xx + 1) { addLight(xx, yy, 0); } }");
    REQUIRE(p.size() == 8);
    CHECK(p[0] == Coord3D{0, 0, 0});
    CHECK(p[3] == Coord3D{3, 0, 0});
    CHECK(p[4] == Coord3D{0, 1, 0});       // the second row starts over at x=0
    CHECK(p[7] == Coord3D{3, 1, 0});
}

TEST_CASE("the light count is known before any coordinate is asked for") {
    // The layout contract: the Layer sizes its buffer from lightCount() and only then walks
    // forEachCoord. A count that came from the walk would arrive too late to be useful.
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("uint8_t width = 5;  // @control 1..64\n"
                "uint8_t height = 3; // @control 1..64\n"
                "for (yy = 0; yy < height; yy = yy + 1) {"
                "  for (xx = 0; xx < width; xx = xx + 1) { addLight(xx, yy, 0); } }");
    l.prepare();
    CHECK(l.lightCount() == 15);           // answered without anyone calling forEachCoord
}

TEST_CASE("the count and the coordinates always agree, because one script produces both") {
    // The property SphereLayout names: count and emit run the same code, so they cannot drift.
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 7; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();

    std::vector<Coord3D> seen;
    CoordSink sink{[](void* ctx, nrOfLightsType, lengthType x, lengthType y, lengthType z) {
                       static_cast<std::vector<Coord3D>*>(ctx)->push_back({x, y, z});
                   }, nullptr, &seen};
    l.forEachCoord(sink);
    CHECK(l.lightCount() == static_cast<nrOfLightsType>(seen.size()));
}

TEST_CASE("a scripted layout allocates nothing, like every other layout") {
    // A 16k-light fixture staged as coordinates would be 48 KB — memory a classic ESP32 does not
    // have. The script calls out per light instead, so the only heap here is the compiled program.
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 4096; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 4096);
    // dynamicBytes is the JIT'd program only — no coordinate storage grows with the light count.
    CHECK(l.dynamicBytes() < 1024);
}

TEST_CASE("a script places lights wherever it likes, which is the point of scripting one") {
    // A strand that runs right to left: one line here, a new C++ class otherwise.
    const std::vector<Coord3D> p = place(
        "uint8_t width = 4; // @control 1..64\n"
        "for (i = 0; i < width; i = i + 1) { addLight(width - 1 - i, 0, 0); }");
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{3, 0, 0});
    CHECK(p[3] == Coord3D{0, 0, 0});
}

TEST_CASE("a script can place a shape no rectangular layout can express") {
    // A diagonal — light i at (i, i).
    const std::vector<Coord3D> p = place("for (i = 0; i < 4; i = i + 1) { addLight(i, i, 0); }");
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{0, 0, 0});
    CHECK(p[3] == Coord3D{3, 3, 0});
}

TEST_CASE("a broken script leaves an empty fixture rather than taking the pipeline down") {
    // Robustness, the hard rule: someone editing a layout mid-show types something wrong. The
    // fixture reports no lights, the module carries the diagnostic, and the device keeps running.
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 4; i = i + 1) { addLight(i, i");   // unclosed
    l.prepare();
    CHECK(l.lightCount() == 0);
    CHECK(l.severity() == MoonModule::Severity::Error);
}

TEST_CASE("editing the script changes the fixture") {
    // The live-edit loop: the same module, a new script, a different physical shape.
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 4; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 4);

    l.setSource("for (i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 2);
}

// Every example in MoonLiveLayout.md must actually compile. A doc that shows a call the language
// does not have sends the reader to a parse error on their first attempt — and it happened here:
// an early draft advertised cos8/sin8, which are not registered built-ins.
TEST_CASE("the scripts the documentation shows all compile") {
    const char* fromDocs[] = {
        // the default
        "uint8_t width = 16;  // @control 1..64\n"
        "uint8_t height = 16; // @control 1..64\n"
        "for (yy = 0; yy < height; yy = yy + 1) {"
        "  for (xx = 0; xx < width; xx = xx + 1) { addLight(xx, yy, 0); } }",
        // right to left
        "uint8_t width = 8; // @control 1..64\n"
        "for (i = 0; i < width; i = i + 1) { addLight(width - 1 - i, 0, 0); }",
        // a diagonal
        "uint8_t width = 8; // @control 1..64\n"
        "for (i = 0; i < width; i = i + 1) { addLight(i, i, 0); }",
        // two rows, stacked
        "uint8_t width = 8; // @control 1..64\n"
        "for (i = 0; i < width; i = i + 1) { addLight(i, 0, 0); addLight(i, 1, 0); }",
        // print wrapping an argument
        "for (i = 0; i < 2; i = i + 1) { addLight(print(i), 0, 0); }",
    };
    for (const char* s : fromDocs) {
        MoonLiveLayout l;
        l.defineControls();
        l.setSource(s);
        l.prepare();
        INFO("script: " << s);
        CHECK(l.severity() != MoonModule::Severity::Error);
        CHECK(l.lightCount() > 0);
    }
}

// The container asks a layout for its count and its coordinates SEPARATELY, and both must work at
// any time — not only immediately after prepare(). Layouts::prepare walks forEachCoord for the
// bounding box, and the Layer asks lightCount() when it sizes its buffer; a layout that answers
// only once reports an empty fixture to whichever asks second.
TEST_CASE("a layout answers count and coordinates every time it is asked") {
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 6; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();

    CHECK(l.lightCount() == 6);
    CHECK(l.lightCount() == 6);            // asking twice must give the same answer

    int emitted = 0;
    CoordSink counting{[](void* ctx, nrOfLightsType, lengthType, lengthType, lengthType) {
                           (*static_cast<int*>(ctx))++;
                       }, nullptr, &emitted};
    l.forEachCoord(counting);
    CHECK(emitted == 6);

    emitted = 0;
    l.forEachCoord(counting);
    CHECK(emitted == 6);                   // and walking twice, too
    CHECK(l.lightCount() == 6);            // count still right after a walk
}


// Subtraction has to produce the WHOLE value, not just a byte that happens to look right.
// `a - b` compiles to `a + (b * -1)`, and if -1 is materialised as 65535 (which it was, on two of
// three targets) the result is correct only modulo 256. A coordinate comparison cannot see that —
// both the right answer and the widened one truncate to the same byte.
//
// A layout's light INDEX can see it: the count comes from how many times addLight ran, so a loop
// bound computed by subtraction that came out ~65k places a wildly different number of lights.
TEST_CASE("a subtraction feeding a loop bound produces the whole value") {
    MoonLiveLayout l;
    l.defineControls();
    // 10 - 4 must be 6 lights. A widened -1 makes the bound enormous and the count is not 6.
    l.setSource("for (i = 0; i < 10 - 4; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 6);

    // And a subtraction inside the placement, where the coordinate is the observable.
    std::vector<Coord3D> p = place("uint8_t width = 4; // @control 1..64\n"
                                   "for (i = 0; i < width; i = i + 1) { addLight(width - 1 - i, 0, 0); }");
    REQUIRE(p.size() == 4);
    CHECK(p[0] == Coord3D{3, 0, 0});      // 4 - 1 - 0
    CHECK(p[3] == Coord3D{0, 0, 0});      // 4 - 1 - 3
}

// A slider you moved has to survive editing the script — that is the live-authoring loop, and the
// control arena delivers it by keeping a slot's value when the control persists across a recompile
// (MoonLive.h, ensureArena). The consequence, which is easy to be surprised by: the arena matches
// controls by OFFSET, so a DIFFERENT script whose first control happens to sit at the same offset
// inherits the value rather than its own initialiser. Two scripts that both open with a `width` are
// the same slot as far as the engine is concerned.
//
// This pins the behaviour so a change to it is a decision rather than an accident. Setting the
// control after the edit is what makes a script's own extent authoritative.
TEST_CASE("a scripted control keeps its live value when the script is edited") {
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("uint8_t width = 16; // @control 1..64\n"
                "for (i = 0; i < width; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 16);

    // A second script declaring width at the same offset inherits the live 16, not its own 8.
    l.setSource("uint8_t width = 8; // @control 1..64\n"
                "for (i = 0; i < width; i = i + 1) { addLight(i, 1, 0); }");
    l.prepare();
    CHECK(l.lightCount() == 16);

    // A script whose first control is a NEW slot gets its own initialiser: nothing to inherit.
    l.setSource("uint8_t width = 16;  // @control 1..64\n"
                "uint8_t height = 3;  // @control 1..64\n"
                "for (yy = 0; yy < height; yy = yy + 1) {"
                "  for (xx = 0; xx < width; xx = xx + 1) { addLight(xx, yy, 0); } }");
    l.prepare();
    CHECK(l.lightCount() == 48);          // 16 inherited, height 3 its own
}
