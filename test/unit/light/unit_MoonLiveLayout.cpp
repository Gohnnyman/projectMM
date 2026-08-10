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
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "light/layers/Layer.h"
#include "light/layouts/Layouts.h"

#include <cstring>
#include <cstdio>
#include <vector>
#include <thread>

using namespace mm;


// Every case here compiles a script and runs the emitted native code, so all of them need a JIT
// backend for the host ISA. `MM_MOONLIVE_HAS_HOST_JIT` is 0 on x86_64 — which is what CI runs — and
// there a layout reports zero lights for a reason that has nothing to do with the layout. Gated as a
// block, the same way unit_moonlive_fill / unit_moonlive_ir do it.
#if MM_MOONLIVE_HAS_HOST_JIT

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

// A layout script never fills, so it should not pay for the scratch registers a fill needs. The
// backends reserved them unconditionally, and on the smallest register file (Xtensa, 12) that one
// register was the difference between a nested loop compiling and being refused outright — the
// shipped default script is a nested loop, so the module's own default did not compile there.
//
// This is a register-budget property, and the budget differs per target, so what it pins portably is
// the behaviour: a nested loop places every light of the grid it describes.
TEST_CASE("a nested loop lays out a full grid, on every target's register budget") {
    const std::vector<Coord3D> p = place(
        "for (yy = 0; yy < 3; yy = yy + 1) {"
        "  for (xx = 0; xx < 5; xx = xx + 1) { addLight(xx, yy, 0); } }");
    REQUIRE(p.size() == 15);               // 3 rows x 5 columns, none dropped
    CHECK(p[0]  == Coord3D{0, 0, 0});
    CHECK(p[4]  == Coord3D{4, 0, 0});      // end of the first row
    CHECK(p[5]  == Coord3D{0, 1, 0});      // the inner counter restarted
    CHECK(p[14] == Coord3D{4, 2, 0});
}

// A `for` counter must survive whatever the body does to it. The device backends built a light's
// byte address by multiplying the index register IN PLACE — fine when the index is a throwaway temp,
// wrong when it is the loop counter, which the step and the loop test read again afterwards. A
// gradient (`for (i…) { setRGB(i, …) }`) therefore ran the wrong number of times on Xtensa and
// RISC-V while being correct on the desktop host, which used a scratch register instead.
//
// Pinned through a LAYOUT because the count is the observable: the host executes this test, and the
// arithmetic the backends share is the same. addLight's index is likewise the counter.
TEST_CASE("a loop counter survives the body that uses it") {
    SUBCASE("through a call — addLight") {
        MoonLiveLayout l;
        l.defineControls();
        l.setSource("for (i = 0; i < 6; i = i + 1) { addLight(i, i, 0); }");
        l.prepare();
        CHECK(l.lightCount() == 6);      // a clobbered counter gives some other number
    }
    SUBCASE("through an inline store — setRGB") {
        // The case the bug was actually in: StoreElem folded the byte address into the index
        // register, which for `setRGB(i, …)` is the counter itself. The count above cannot see that
        // — addLight is a Call and takes a different path — so this drives the emitted code and
        // checks every light was written, which is what a wrong counter changes.
        uint8_t code[4096];
        auto r = moonlive::compileSource("for (i = 0; i < 6; i = i + 1) { setRGB(i, 200, 0, 0); }",
                                         moonlive::lightBuiltins(), code, sizeof(code));
        REQUIRE(r.ok);
        void* blk = platform::allocExec(r.len);
        REQUIRE(blk);
        platform::writeExec(blk, code, r.len);
        uint8_t buf[6 * 3] = {};
        uint8_t arena[moonlive::kMaxCtrls] = {};
        reinterpret_cast<moonlive::CtrlFn>(blk)(buf, 6, 3, 0, arena);
        for (int i = 0; i < 6; i++) {
            INFO("light " << i);
            CHECK(buf[i * 3] == 200);    // every one of the six written, none skipped or repeated
        }
        platform::freeExec(blk, r.len);
    }
}

// A malformed script must produce a diagnostic, never a hang. The `for` header's step expression is
// scanned by a small loop that skips to the closing paren; a lexer ERROR is not the END token, and
// the lexer does not move past the offending character, so the scan spun forever on a stray symbol.
// A device compiling that script would wedge with no message at all.
TEST_CASE("a stray character in a for header is rejected, not spun on") {
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("for (i = 0; i < 4; i = i @ 1) { addLight(i, 0, 0); }");
    l.prepare();                                    // must return — a hang fails by timeout
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.lightCount() == 0);
}

// A scripted layout is asked for its lights from more than one thread: the HTTP task runs the script
// when a control is edited, while the render task walks the same layout for the frame. The sink the
// script calls out through used to be one process-wide pair, so one thread cleared it while the other
// was mid-run — the built-in then called a live function pointer with a null context and the render
// core took a null dereference. It presented as an intermittent crash while resizing on an S3.
//
// Two threads walking their own layouts concurrently must each see their own sink.
TEST_CASE("two threads can run scripts at once without stealing each other's sink") {
    auto place = [](int width, int reps) {
        MoonLiveLayout l;
        l.defineControls();
        char src[128];
        std::snprintf(src, sizeof(src), "for (i = 0; i < %d; i = i + 1) { addLight(i, 0, 0); }", width);
        l.setSource(src);
        l.prepare();
        for (int r = 0; r < reps; r++)
            if (l.lightCount() != static_cast<nrOfLightsType>(width)) return false;
        return true;
    };

    bool aOk = false, bOk = false;
    std::thread a([&] { aOk = place(7, 400); });
    std::thread b([&] { bOk = place(23, 400); });
    a.join();
    b.join();
    CHECK(aOk);      // each thread counted ITS OWN script every time
    CHECK(bOk);
}

// The card's memory figure has to be the memory the module actually holds, or it is not worth
// reading. A scripted module owns two heap blocks — the emitted code and the control-values arena —
// and dynamicBytes counted only the first, so every scripted card under-reported. It also read 0
// whenever the script failed to compile, while the arena was still allocated.
TEST_CASE("a scripted layout reports every heap byte it holds, compiled or not") {
    MoonLiveLayout l;
    l.defineControls();
    l.setSource("uint8_t width = 4; // @control 1..64\n"
                "for (i = 0; i < width; i = i + 1) { addLight(i, 0, 0); }");
    l.prepare();
    const size_t compiled = l.dynamicBytes();
    CHECK(compiled > 0);
    CHECK(l.lightCount() == 4);

    // A broken script frees the code but keeps the arena, so the figure drops without reaching zero.
    l.setSource("for (i = 0; i < 4; i = i + 1) { addLight(i, i");   // unclosed
    l.prepare();
    CHECK(l.severity() == MoonModule::Severity::Error);
    CHECK(l.dynamicBytes() < compiled);      // the code block is gone
    CHECK(l.dynamicBytes() > 0);             // the arena is not
}

// The layer builds its mapping in two passes over the layouts: pass A counts destinations, pass B
// scatters driver indices into an array sized by that count. Both passes call forEachCoord — and a
// scripted layout COMPILES lazily inside forEachCoord, so a control edited between the two passes
// makes pass B emit more lights than pass A counted. The scatter then ran past its array and
// corrupted the heap; the crash surfaced later inside an unrelated allocation, which is why resizing
// a scripted layout failed at random rather than pointing anywhere near the layer.
//
// A layout that grows mid-build must cost a dropped destination, never memory.
TEST_CASE("a layout that changes size mid-build cannot overrun the mapping") {
    MoonLiveLayout layout;
    layout.defineControls();
    layout.setSource("uint8_t width = 4; // @control 1..64\n"
                     "for (i = 0; i < width; i = i + 1) { addLight(i, 0, 0); }");
    layout.prepare();

    mm::Layouts group;
    group.addChild(&layout);
    mm::Layer layer;
    layer.setLayouts(&group);
    layer.setChannelsPerLight(3);
    layer.defineControls();
    group.applyState();
    layer.applyState();

    auto setWidth = [&](uint8_t v) {
        const auto& cs = layout.controls();
        for (uint8_t i = 0; i < cs.count(); i++)
            if (cs[i].name && std::strcmp(cs[i].name, "width") == 0)
                *static_cast<uint8_t*>(cs[i].ptr) = v;
    };

    // Grow and shrink repeatedly, rebuilding each time — the resize loop a slider drives.
    for (uint8_t w : {4, 32, 8, 48, 16, 64, 2, 24}) {
        setWidth(w);
        group.applyState();
        layer.applyState();
        INFO("width " << (int)w);
        CHECK(layout.lightCount() == w);
        REQUIRE(layer.buffer().data() != nullptr);
        CHECK(layer.buffer().count() >= w);
    }
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
