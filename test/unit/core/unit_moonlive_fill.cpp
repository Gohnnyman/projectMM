// @module MoonLive

#include "doctest.h"
#include "moonlive_script_wrap.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/moonlive_emit.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
#include "platform/platform.h"

#include <cstdint>
#include <string>
#include <thread>
#include <vector>

// MoonLive Stage 1a: the load-bearing slice — emit a fixed-color fill as native code,
// place it in executable memory, and call it over a buffer. These tests run the WHOLE path
// in-process on the desktop host backend (the host ISA's emit + platform::allocExec/
// writeExec + a real call), so they prove emit → exec → call → buffer-write works off
// hardware. The Xtensa backend is validated by the live S3 run, not here.

using namespace mm;

#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("MoonLive emitFill produces a non-empty routine") {
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 1, 2, 3);
    CHECK(n > 0);
    CHECK(n <= sizeof(code));
}
#endif

TEST_CASE("MoonLive emitFill rejects a too-small buffer (degrades, no overrun)") {
    uint8_t tiny[2];
    CHECK(moonlive::emitFill(tiny, sizeof(tiny), 1, 2, 3) == 0);
}

TEST_CASE("MoonLive emitFill/emitAnimatedFill reject a null output buffer (no crash)") {
    CHECK(moonlive::emitFill(nullptr, 256, 1, 2, 3) == 0);   // ample cap, null out → 0, not a deref
    CHECK(moonlive::emitAnimatedFill(nullptr, 256) == 0);
}

// The compile-through-call tests below need a working host JIT (emit blob + assembler); on
// x86_64 desktops (Windows, Linux/macOS Intel) MoonLive::compile fails cleanly and the tests
// would fail on the REQUIRE. Guarded on the emit-header capability macro so they compile
// out where the backend is unimplemented — the same "runs dark" degradation on-device.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("MoonLive compiles and fills a buffer with the chosen color") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(/*r*/ 10, /*g*/ 20, /*b*/ 200));
    REQUIRE(engine.ok());

    // A 5-light, 3-channel buffer pre-filled with a sentinel so a missed write shows.
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    engine.run(buf.data(), 5, 3, /*t*/ 0);

    for (int i = 0; i < 5; i++) {
        CHECK(buf[i*3 + 0] == 10);
        CHECK(buf[i*3 + 1] == 20);
        CHECK(buf[i*3 + 2] == 200);
    }
}

TEST_CASE("MoonLive run on zero lights writes nothing (robust to empty)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 0, 0));
    std::vector<uint8_t> buf(3, 0xAB);
    engine.run(buf.data(), 0, 3, 0);          // nLights == 0
    CHECK(buf[0] == 0xAB);                  // untouched
}

// The native routines write channels +0/+1/+2 per light, so a layer with fewer than 3
// channels per light can't hold RGB — run() must leave it untouched, not overrun it.
TEST_CASE("MoonLive run is a no-op on sub-RGB buffers (cpl 1 and 2)") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(255, 255, 255));
    for (uint8_t cpl : {uint8_t(1), uint8_t(2)}) {
        std::vector<uint8_t> buf(8 * cpl, 0xAB);   // exact size — an RGB write WOULD overrun
        engine.run(buf.data(), 8, cpl, 0);
        for (auto v : buf) CHECK(v == 0xAB);       // every byte untouched, no out-of-bounds
    }
    // null buffer is also a safe no-op.
    engine.run(nullptr, 8, 3, 0);
}

TEST_CASE("MoonLive recompile swaps the color; free returns to !ok") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compile(1, 1, 1));
    std::vector<uint8_t> buf(3, 0);
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 1);

    REQUIRE(engine.compile(9, 8, 7));       // recompile a new color
    engine.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 9); CHECK(buf[1] == 8); CHECK(buf[2] == 7);

    engine.free();
    CHECK_FALSE(engine.ok());
    // run() after free is a safe no-op (no call through null).
    engine.run(buf.data(), 1, 3, 0);
}

TEST_CASE("MoonLive animated fill derives color from the per-frame t") {
    moonlive::MoonLive engine;
    REQUIRE(engine.compileAnimated());
    REQUIRE(engine.ok());
    std::vector<uint8_t> buf(4 * 3, 0);

    // red = (t>>3)&0xFF, green=0, blue=64. Two different t -> two different reds, proving
    // the runtime arg reaches the emitted native code and changes its output.
    engine.run(buf.data(), 4, 3, /*t*/ 0);
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 0);    // t>>3 == 0
        CHECK(buf[i*3 + 1] == 0);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 800);   // 800>>3 = 100
    for (int i = 0; i < 4; i++) {
        CHECK(buf[i*3 + 0] == 100);
        CHECK(buf[i*3 + 2] == 64);
    }

    engine.run(buf.data(), 4, 3, /*t*/ 2048);  // 2048>>3 = 256 -> &0xFF = 0
    CHECK(buf[0] == 0);                          // wraps at 256, as a byte does
}

TEST_CASE("platform allocExec returns usable executable memory, freeExec releases it") {
    void* blk = platform::allocExec(64);
    REQUIRE(blk != nullptr);
    // Copy the emitted fill in via writeExec (the IRAM/cache-safe path) and call it.
    uint8_t code[256];
    size_t n = moonlive::emitFill(code, sizeof(code), 7, 7, 7);
    platform::writeExec(blk, code, n);
    auto fn = reinterpret_cast<moonlive::FillFn>(blk);
    std::vector<uint8_t> buf(3, 0);
    fn(buf.data(), 1, 3);
    CHECK(buf[0] == 7);
    platform::freeExec(blk, 64);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT — the JIT-dependent block ends here; the STAGE 1 CONTROLS below
        // exercise the parser/arena, which also depend on compile() succeeding, so they gate too.

#if MM_MOONLIVE_HAS_HOST_JIT

// STAGE 1 CONTROLS — engine-level arena behaviour. These pin the load-bearing decision: the
// control-values arena is owned by the engine, has a STABLE address across a recompile (grows
// only), seeds new slots from their declared default, preserves a slot's live value across a
// source edit that keeps the control, and is released by free(). The codegen/live-read is pinned
// in unit_moonlive_ir; this is the engine's ownership + lifecycle contract.
static moonlive::BuiltinTable kCtrlTable = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

// A loop counter and its limit are live ACROSS a call whenever the body calls anything — which is
// most real effects. The assembler's contract says it preserves what has to survive; this runs the
// loop and counts, so a backend that clobbered either would show up as a short or runaway loop
// rather than as an argument about which registers are caller-saved.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a loop counter survives a call in the body") {
    moonlive::MoonLive eng;
    // random16 is a Call; `i` and the limit `w` are both live around it.
    REQUIRE(eng.compile(mmScript("uint8_t w = 8;\nfor (i = 0; i < w; i = i + 1) { setRGB(i, random16(200), 200, 0); }"),
                        kCtrlTable, kSys));
    uint8_t buf[8 * 3] = {};
    eng.run(buf, 8, 3, 0);
    int written = 0;
    for (int i = 0; i < 8; i++)
        if (buf[i * 3] || buf[i * 3 + 1] || buf[i * 3 + 2]) written++;
    CHECK(written == 8);     // every iteration ran: the counter was not clobbered by the call
    eng.free();
}
#endif

// `t` is an argument register, not an arena byte — so unlike a control it can be CLOBBERED by a
// callee under the ABI. Every animated script that calls anything reads it after a call, so this
// runs a script that does exactly that and checks the value that comes out is the one passed in.
// (The arm64 backend saves x3 for this reason; the comment there is not evidence, this is.)
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("elapsed time survives a call that happens before it is read") {
    moonlive::MoonLive eng;
    // Two STATEMENTS, so the ordering is the language's, not an argument-evaluation detail: the
    // first call happens, and only then is `t` read. Light 0 burns the call; light 1 reads t.
    REQUIRE(eng.compile(mmScript("setRGB(0, random16(200), 0, 0);\n"
                        "setRGB(1, mod(t, 200), 0, 0);"), kCtrlTable, kSys));
    uint8_t buf[2 * 3] = {};
    eng.run(buf, 2, 3, 12345);
    CHECK(buf[3] == 12345 % 200);   // 145 — the elapsed value the host passed, not a clobbered one
    eng.free();
}
#endif

TEST_CASE("MoonLive controls: declaredControls + controlSlot seeded from the default") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("uint8_t speed = 42; // @control 0..99\nsetRGB(speed, 0, 0, 255);"), kCtrlTable, kSys));
    uint8_t n = 0;
    const moonlive::DeclaredControl* dc = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(dc[0].def == 42); CHECK(dc[0].min == 0); CHECK(dc[0].max == 99); CHECK(dc[0].offset == 0);
    uint8_t* slot = eng.controlSlot(0);
    REQUIRE(slot != nullptr);
    CHECK(*slot == 42);                                  // arena slot seeded to the declared default
    // Offset 1 is a real arena byte (an undeclared control slot), so it resolves. Past the arena
    // — controls plus the host's system variables — there is nothing to point at.
    CHECK(eng.controlSlot(moonlive::kArenaBytes) == nullptr);   // out of range → nullptr (robust)
}

TEST_CASE("MoonLive controls: arena address is STABLE across a recompile and the slot value survives") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("uint8_t speed = 7; // @control 0..15\nsetRGB(speed, 0, 0, 255);"), kCtrlTable, kSys));
    uint8_t* before = eng.controlSlot(0);
    REQUIRE(before != nullptr);
    *before = 12;                                        // a "slider move" — write the live value

    // Edit the source (recompile) but KEEP the control. The grow-only arena must not move, and the
    // live value must survive (a kept control keeps its slider position across a source edit).
    REQUIRE(eng.compile(mmScript("uint8_t speed = 7; // @control 0..15\nsetRGB(speed, 255, 0, 0);"), kCtrlTable, kSys));
    uint8_t* after = eng.controlSlot(0);
    CHECK(after == before);                              // STABLE address — no dangling bound pointer
    CHECK(*after == 12);                                 // value preserved across the recompile

    // Adding a SECOND control keeps the first's value and seeds the new slot from its default.
    REQUIRE(eng.compile(mmScript("uint8_t speed = 7; // @control 0..15\nuint8_t hue = 200; // @control 0..255\nsetRGB(speed, hue, 0, 255);"), kCtrlTable, kSys));
    CHECK(*eng.controlSlot(0) == 12);                    // speed kept its live value
    CHECK(*eng.controlSlot(1) == 200);                   // hue seeded from its default
}

TEST_CASE("MoonLive controls: free() releases the arena (no stale slot after release)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("uint8_t a = 5; // @control 0..9\nfill(0, 0, a);"), kCtrlTable, kSys));
    REQUIRE(eng.controlSlot(0) != nullptr);
    eng.free();
    CHECK_FALSE(eng.ok());
    CHECK(eng.controlSlot(0) == nullptr);                // arena gone — no dangling pointer handed out
    // Recompiling after a full free re-acquires cleanly (add/remove robustness).
    REQUIRE(eng.compile(mmScript("uint8_t a = 5; // @control 0..9\nfill(0, 0, a);"), kCtrlTable, kSys));
    REQUIRE(eng.controlSlot(0) != nullptr);
    CHECK(*eng.controlSlot(0) == 5);                     // re-seeded from default
}

// line(x1, y1, x2, y2, r, g, b), the SEVEN-argument builtin and the widest call a script makes.
// This is the behavioral pin for the args-array call ABI at an arity past the register file:
// all seven values must arrive intact, in order, through the same staging every backend uses.
TEST_CASE("MoonLive line() draws a horizontal segment through the installed canvas") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class L { tick() { line(1, 0, 3, 0, 10, 20, 200); } }", kCtrlTable, kSys));
    REQUIRE(eng.ok());

    // A 5-wide single-row canvas, sentinel-filled so an errant write shows.
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{buf.data(), buf.size(), Coord3D{5, 1, 1}, 3});
    eng.run(buf.data(), 5, 3, /*t*/ 0);
    moonlive::setDrawCanvas({});

    CHECK(buf[0*3] == 0xAB);                             // before the segment: untouched
    for (int i = 1; i <= 3; i++) {
        CHECK(buf[i*3 + 0] == 10);
        CHECK(buf[i*3 + 1] == 20);
        CHECK(buf[i*3 + 2] == 200);
    }
    CHECK(buf[4*3] == 0xAB);                             // after the segment: untouched
}

// No canvas installed (a layout or modifier host) → every draw call must no-op, not write
// through a stale or foreign pointer. The detach after each run is what this pins.
TEST_CASE("MoonLive line() without a canvas draws nothing") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("line(0, 0, 4, 0, 255, 255, 255);"), kCtrlTable, kSys));
    std::vector<uint8_t> buf(5 * 3, 0xAB);
    eng.run(buf.data(), 5, 3, 0);                        // canvas never installed
    for (uint8_t v : buf) CHECK(v == 0xAB);
}

// A binding that installs nothing (a modifier) must not consume one of the two per-thread slots
// just by RUNNING a script that draws. Slots are claimed on install and released on detach, so a
// claim taken on the READ path is never given back. Two such threads exhaust the table, and from
// then on every install silently fails: the render thread's line() goes dark with nothing to see
// but a stopped drawing. Two threads is the smallest case that shows it, because a single thread
// re-uses the slot it already leaked.
TEST_CASE("a script that draws with nothing installed does not consume a per-thread slot") {
    // A std::string, not mmScript's thread-local buffer: the two threads below read this, and
    // handing them a pointer into the MAIN thread's buffer is the kind of sharing this very test
    // exists to catch.
    const std::string kDraws = mmScript("line(0, 0, 4, 0, 255, 255, 255);");
    auto runWithNothingInstalled = [&] {
        moonlive::MoonLive eng;
        REQUIRE(eng.compile(kDraws.c_str(), kCtrlTable, kSys));
        std::vector<uint8_t> buf(5 * 3, 0xAB);
        eng.run(buf.data(), 5, 3, 0);        // no canvas, no sink: the modifier's shape
        for (uint8_t v : buf) CHECK(v == 0xAB);   // and it draws nothing
    };
    // CONCURRENT, not sequential: joined threads can be handed the same identity by the OS, and
    // one identity claims one slot. Two live at once is what fills the table.
    std::thread a(runWithNothingInstalled), b(runWithNothingInstalled);
    a.join();
    b.join();

    // Both slots must still be claimable. A leaked claim from either thread above would make this
    // install a no-op and the canvas would stay at its sentinel.
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(kDraws.c_str(), kCtrlTable, kSys));
    std::vector<uint8_t> canvasBuf(5 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{canvasBuf.data(), canvasBuf.size(), Coord3D{5, 1, 1}, 3});
    eng.run(canvasBuf.data(), 5, 3, 0);
    moonlive::setDrawCanvas({});
    for (int i = 0; i < 5; i++) CHECK(canvasBuf[i*3] == 255);   // the install took effect
}

// Script arithmetic is unsigned, so a coordinate that went "negative" arrives as a huge value.
// The builtin clamps endpoints to the canvas, which keeps the draw instant and on-grid instead
// of sending the line walker on a billions-of-steps march (robustness: any input, degrade
// visibly, never stall the render thread).
TEST_CASE("MoonLive line() clamps out-of-range endpoints to the canvas edge") {
    moonlive::MoonLive eng;
    // x2 = 0 - 1 wraps to the full unsigned word; the clamp pins it to the last column.
    REQUIRE(eng.compile(mmScript("line(0, 0, 0 - 1, 0, 7, 8, 9);"), kCtrlTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0xAB);
    moonlive::setDrawCanvas(draw::Canvas{buf.data(), buf.size(), Coord3D{4, 1, 1}, 3});
    eng.run(buf.data(), 4, 3, 0);
    moonlive::setDrawCanvas({});
    for (int i = 0; i < 4; i++) CHECK(buf[i*3 + 0] == 7);   // the whole row drawn, promptly
}

// ENTRY POINTS DISPATCH: a script with two functions runs the one that was ASKED for.
//
// This is the case a symbol table exists for, and the one that fails silently when it is wrong:
// both functions compile, both are callable, and running the wrong one just draws the wrong thing.
// The two write different colours to different pixels, so which ran is visible in the buffer.
TEST_CASE("a script runs the entry point the host asked for, not whichever came first") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class TwoFns {\n"
                        "  helper() { setRGB(0, 11, 0, 0); }\n"
                        "  tick()   { setRGB(1, 0, 22, 0); }\n"
                        "}\n", kCtrlTable, kSys));
    REQUIRE(eng.entryCount() == 2);

    std::vector<uint8_t> buf(2 * 3, 0);
    eng.run(buf.data(), 2, 3, 0, "tick");
    CHECK(buf[0 * 3 + 0] == 0);      // helper did NOT run
    CHECK(buf[1 * 3 + 1] == 22);     // tick did

    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 2, 3, 0, "helper");
    CHECK(buf[0 * 3 + 0] == 11);     // and now the other way round
    CHECK(buf[1 * 3 + 1] == 0);
}

// Asking for a name the script did not define runs NOTHING. The alternative — falling back to the
// block start — would run some arbitrary function and look like it worked, which is the failure a
// binding cannot diagnose.
TEST_CASE("asking for an entry point a script does not define runs nothing") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class OnlyTick { tick() { setRGB(0, 99, 0, 0); } }", kCtrlTable, kSys));
    CHECK_FALSE(eng.hasEntry("placeLights"));

    std::vector<uint8_t> buf(3, 0xAB);
    eng.run(buf.data(), 1, 3, 0, "placeLights");
    for (uint8_t v : buf) CHECK(v == 0xAB);      // untouched
}

// A NAME IS A MOMENT, not a role. One class may define several entry points, and the host calls
// whichever the moment calls for: `tick` when a frame renders, `modifyLogical` when a coordinate is
// folded. That is what lets an effect also fold coordinates without any feature being added for it,
// and it is why nothing validates which names a class defines.
TEST_CASE("one class can serve several moments, and each is called on its own") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class Both {\n"
                        "  tick()          { setRGB(0, 7, 0, 0); }\n"
                        "  modifyLogical() { setXYZ(0, 3, 4, 5); }\n"
                        "}\n", kCtrlTable, kSys));
    CHECK(eng.hasEntry("tick"));
    CHECK(eng.hasEntry("modifyLogical"));

    // The frame moment paints.
    std::vector<uint8_t> buf(3, 0);
    eng.run(buf.data(), 1, 3, 0, "tick");
    CHECK(buf[0] == 7);

    // The fold moment writes a coordinate into the same three-byte shape, untouched by the above.
    uint8_t pos[3] = {0, 0, 0};
    eng.run(pos, 1, 3, 0, "modifyLogical");
    CHECK(pos[0] == 3); CHECK(pos[1] == 4); CHECK(pos[2] == 5);
}

// A class that defines NEITHER of a binding's moments is not an error: it compiles, and the binding
// simply has nothing to call. The script author decides what their script is for.
TEST_CASE("a class that defines no moment a binding owns is still a valid script") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class Helper { paint() { setRGB(0, 1, 2, 3); } }", kCtrlTable, kSys));
    CHECK(eng.ok());
    CHECK_FALSE(eng.hasEntry("tick"));
    CHECK_FALSE(eng.hasEntry("modifyLogical"));
}

#endif  // MM_MOONLIVE_HAS_HOST_JIT
