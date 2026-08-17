// @module MoonLive
// @also MoonLiveLayout, MoonLiveEffect, MoonLiveModifier

// Every script in `moonlive/` has to compile.
//
// Those files are what a user copies into a device, and three of them ship as module defaults — so a
// script that stops parsing is a broken default and a broken example at once. The language is young
// and still gaining syntax; without this, the first sign that a change broke them is someone pasting
// one into a running fixture and getting a parse error.
//
// The scripts live as files rather than as string literals here so they can be read, edited and
// pasted without a rebuild. This test walks the folder, so a new script is covered by adding it.

#include "doctest.h"
#include "../core/moonlive_script_wrap.h"
#include "core/moonlive/MoonLive.h"
#include "platform/platform.h"
#include "core/moonlive/moonlive_emit.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <string>
#include <vector>

using namespace mm;

namespace {
/// The repo's script folder, found relative to this source file so the test does not depend on the
/// working directory a runner happens to use.
std::filesystem::path scriptRoot() {
    std::filesystem::path p = std::filesystem::path(__FILE__).parent_path();   // test/unit/light
    return p.parent_path().parent_path().parent_path() / "moonlive";           // repo root
}

std::vector<std::filesystem::path> scriptsIn(const char* sub) {
    std::vector<std::filesystem::path> out;
    const std::filesystem::path dir = scriptRoot() / sub;
    if (!std::filesystem::exists(dir)) return out;
    for (const auto& e : std::filesystem::directory_iterator(dir))
        if (e.is_regular_file() && e.path().extension() == ".mlv") out.push_back(e.path());
    return out;
}

std::string read(const std::filesystem::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
}  // namespace

TEST_CASE("every script in moonlive/ compiles") {
    int checked = 0;
    for (const char* sub : {"layouts", "effects", "modifiers", "drivers"}) {
        for (const auto& file : scriptsIn(sub)) {
            const std::string src = read(file);
            const std::string label = std::string(sub) + "/" + file.filename().string();

            // Compile it exactly as it ships, against the system variables ITS OWN binding supplies
            // — a layout gets the clock, an effect the grid, a modifier the grid plus a coordinate.
            // Using one shared list here would let a script read a name its module never writes and
            // still pass, which is the silent-zero this per-binding split exists to prevent.
            const moonlive::SysVarTable sys =
                std::string(sub) == "layouts"   ? moonlive::layoutSysVars()   :
                std::string(sub) == "modifiers" ? moonlive::modifierSysVars() :
                                                  moonlive::effectSysVars();
            moonlive::MoonLive engine;
            const bool ok = engine.compile(src.c_str(), moonlive::lightBuiltins(), sys);
            if (!ok) std::printf("FAIL %-28s %s\n", label.c_str(), engine.error());
            // compile() both PARSES and emits native code, and only the second half needs a backend
            // for this host's ISA (MM_MOONLIVE_HAS_HOST_JIT — 0 on x86_64, which is what CI runs).
            // Requiring success there would fail every script for a reason that has nothing to do
            // with the script, so without a backend the only failure allowed is the codegen one.
#if MM_MOONLIVE_HAS_HOST_JIT
            CHECK(ok);
#else
            CHECK((ok || std::string(engine.error()) == moonlive::kCodegenFailed));
#endif
            engine.free();
            checked++;
        }
    }
    MESSAGE("compiled " << checked << " scripts from moonlive/");
    CHECK(checked > 0);            // a silently empty folder would pass without this
}

// Comments are what makes a script in `moonlive/` readable, so the lexer has to treat a plain `//`
// line as whitespace — anywhere, including between the statements of a loop body. The one exception
// is `// @control min..max`, which is not a comment at all but the declaration of a UI slider.
// Each binding supplies the system variables it actually WRITES, and supplying a name is also what
// reserves it. That split is what keeps `x` usable as a loop counter in a layout while still making
// it mean "the light being folded" in a modifier — and what turns a layout reading `width` into an
// error instead of a silent 0 that places no lights and reports success.
// ONE vocabulary for all three roles. A name means the same thing in every script, and the only
// thing a binding decides is which slots it WRITES each frame.
//
// The per-role tables this replaced did not prevent a mistake: a layout reading `width` got a
// compile error, which is the same outcome as reading a value that is always zero. What they did
// create was a trap, because they were different vocabularies rather than nested ones, so a name
// was legal in one role and RESERVED in another. `disasm.py` compiled against the widest table and
// therefore refused `grid.mlv`, the shipped default layout, as "name is a system variable".
TEST_CASE("every script reads the same system-variable vocabulary") {
    struct Case { const char* src; bool ok; const char* what; };
    const Case cases[] = {
        {mmScript("for (y = 0; y < 2; y = y + 1) { for (x = 0; x < 3; x = x + 1) { addLight(x, y, 0); } }"),
         true,  "x and y are ordinary loop counters, in EVERY role: they are the names an author "
                "reaches for, which is why the coordinate is xPos/yPos/zPos instead"},
        {mmScript("for (i = 0; i < width; i = i + 1) { addLight(i, 0, 0); }"),
         true,  "a layout may read width: same name, same meaning, whoever asks"},
        {mmScript("setRGB(width, 0, 0, 0);"),           true,  "an effect reads the layer's width"},
        {mmScript("setXYZ(0, width - 1 - xPos, yPos, zPos);"),
         true,  "a modifier reads its coordinate AND the box it lives in"},
        {mmScript("setRGB(xPos, 0, 0, 0);"),
         true,  "reading a coordinate outside a modifier is legal and reads 0: no binding writes "
                "it, so there is nothing to disagree with"},
        {mmScript("uint8_t width = 16; // @control 1..64\nsetRGB(0, 0, 0, 0);"),
         false, "declaring one is still refused, in every role: that is what keeps a read meaningful"},
        {mmScript("uint8_t xPos = 3;\nsetRGB(0, 0, 0, 0);"),
         false, "the coordinate names are reserved too, so a modifier cannot shadow what it is handed"},
    };
    uint8_t out[2048];
    for (const Case& c : cases) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, moonlive::lightBuiltins(), moonlive::lightSysVars(),
                                         out, sizeof(out));
        // Where a backend exists, a valid script must actually EMIT — accepting kCodegenFailed
        // everywhere would let a codegen regression pass as a pass. Only a host with no assembler
        // for its ISA (x86_64, which is what CI runs) is allowed that answer.
#if MM_MOONLIVE_HAS_HOST_JIT
        if (c.ok) CHECK(r.ok);
#else
        if (c.ok) CHECK((r.ok || std::string(r.error) == moonlive::kCodegenFailed));
#endif
        else      CHECK_FALSE(r.ok);
    }
}

// The three role accessors are aliases of the one table now. Pinned so a future change that
// re-splits them has to say so here rather than silently reintroducing the trap above.
TEST_CASE("the three roles are handed the same table") {
    const auto layout = moonlive::layoutSysVars();
    const auto effect = moonlive::effectSysVars();
    const auto mod    = moonlive::modifierSysVars();
    CHECK(layout.count == effect.count);
    CHECK(effect.count == mod.count);
    CHECK(mod.count == moonlive::lightSysVars().count);
}

TEST_CASE("a script may be commented, and only @control carries meaning") {
    struct Case { const char* src; bool ok; const char* what; };
    const Case cases[] = {
        {mmScript("// leading comment\naddLight(1, 2, 3);"), true, "a comment before the code"},
        {mmScript("addLight(1, 2, 3); // trailing comment"), true, "a comment after the code"},
        {mmScript("for (i = 0; i < 2; i = i + 1) {\n  // inside the body\n  addLight(i, 0, 0);\n}"), true,
         "a comment inside a loop body"},
        {mmScript("// @controlled is a word, not an annotation\naddLight(1, 2, 3);"), true,
         "@control matched as a whole word only"},
        {mmScript("uint8_t n = 4; // @control 1..64\nfor (i = 0; i < n; i = i + 1) { addLight(i, 0, 0); }"),
         true, "an @control declaration"},
        {mmScript("uint8_t n = 4; // @control oops\naddLight(1, 2, 3);"), false,
         "a malformed @control is an error, not a comment"},
    };
    for (const Case& c : cases) {
        moonlive::MoonLive engine;
        const bool ok = engine.compile(c.src, moonlive::lightBuiltins(), moonlive::modifierSysVars());
        INFO(c.what);
        // What this case is about is the LEXER, which runs on every host. Where there is no backend
        // for this ISA a valid script still fails, at codegen — so accept that one diagnostic rather
        // than dropping the coverage. A script expected to FAIL must still fail everywhere.
        if (c.ok) CHECK((ok || std::string(engine.error()) == moonlive::kCodegenFailed));
        else      CHECK(!ok);
        engine.free();
    }
}

TEST_CASE("a comment changes nothing about what a script does") {
    // The stronger claim: commenting a script does not alter the code it produces. Needs a backend —
    // with no code emitted, "same length" is two zeroes and proves nothing.
#if MM_MOONLIVE_HAS_HOST_JIT
    moonlive::MoonLive bare, commented;
    CHECK(bare.compile(mmScript("for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
                       moonlive::lightBuiltins(), moonlive::modifierSysVars()));
    CHECK(commented.compile(mmScript("// place three lights in a row\n"
                            "for (i = 0; i < 3; i = i + 1) {\n"
                            "  addLight(i, 0, 0);   // one per step\n"
                            "}"),
                            moonlive::lightBuiltins(), moonlive::modifierSysVars()));
    CHECK(bare.codeLen() == commented.codeLen());   // byte-for-byte the same program
    CHECK(bare.codeLen() > 0);                      // and a real program, not two zeroes
    bare.free();
    commented.free();
#endif
}


// `t` is the elapsed milliseconds the host passes on every run. Without it a script cannot animate —
// every frame computes the same thing — so it is the difference between a static pattern and an
// effect. It resolves to the argument register the host already fills, costing no instruction and no
// temp: the emitted code reads a3/a0 directly rather than loading a value.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a script reads elapsed time, so it can animate") {
    uint8_t code[2048];
    auto r = moonlive::compileSource(mmScript("setRGB(t, 200, 0, 0);"), moonlive::lightBuiltins(),
                                     moonlive::modifierSysVars(),
                                     code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<moonlive::CtrlFn>(blk);
    uint8_t arena[moonlive::kArenaBytes] = {};

    // The lit light must follow t: a frame at t=3 lights light 3, not light 0.
    for (uint32_t tv : {0u, 3u, 7u}) {
        uint8_t buf[10 * 3] = {};
        fn(buf, 10, 3, tv, arena);
        INFO("t = " << tv);
        CHECK(buf[tv * 3] == 200);                  // the light AT t is lit
        if (tv != 0) CHECK(buf[0] == 0);            // and light 0 is not, so it really moved
    }
    platform::freeExec(blk, r.len);
}
#endif

// `mod` is what turns a moving pattern into a repeating one. `t` grows without bound, so a sweep
// written as `t * speed` runs off the end of the fixture once and never comes back; folding it with
// `mod(…, width)` makes it return to the start and cycle forever. It is a host Call rather than an
// operator because no ISA here has a cheap integer divide — Xtensa has none at all.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("mod wraps a sweep, so an animation repeats instead of running off the end") {
    uint8_t code[4096];
    auto r = moonlive::compileSource(
        mmScript("uint8_t w = 16;   // @control 1..64\n"
        "for (yy = 0; yy < w; yy = yy + 1) { setRGB(yy * w + mod(t, w), 255, 0, 0); }"),
        moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
    REQUIRE(r.ok);
    void* blk = platform::allocExec(r.len);
    REQUIRE(blk);
    platform::writeExec(blk, code, r.len);
    auto fn = reinterpret_cast<moonlive::CtrlFn>(blk);
    uint8_t arena[moonlive::kArenaBytes] = {16};

    // The lit column follows t, and t == width comes back to column 0 rather than off the grid.
    auto columnAt = [&](uint32_t tv) {
        uint8_t buf[256 * 3] = {};
        fn(buf, 256, 3, tv, arena);
        for (int i = 0; i < 256; i++) if (buf[i * 3]) return i % 16;
        return -1;                                  // nothing lit: the sweep left the fixture
    };
    CHECK(columnAt(0)  == 0);
    CHECK(columnAt(7)  == 7);
    CHECK(columnAt(15) == 15);
    CHECK(columnAt(16) == 0);                       // wrapped, not lost
    CHECK(columnAt(33) == 1);                       // and keeps cycling
}
#endif

// A `for` releases its counter's REGISTER when the loop ends, not just its name. Dropping only the
// name left the vreg allocated for the rest of the compile, so every loop a script wrote cost one
// permanently: two sequential loops held two counters even though the first was long dead. That put
// an ordinary two-loop effect one register over the smallest register file (Xtensa has twelve) while
// each loop compiled fine on its own — the confusing part, since neither half looked too big.
TEST_CASE("sequential loops reuse the same register, so a script is not billed per loop") {
    uint8_t code[8192];
    // Four loops, each with a call in the body — comfortably over budget if counters accumulate.
    auto r = moonlive::compileSource(
        mmScript("uint8_t w = 16;   // @control 1..64\n"
        "for (a = 0; a < w; a = a + 1) { setRGB(a, 255, 0, 0); }\n"
        "for (b = 0; b < w; b = b + 1) { setRGB(b, 0, 255, 0); }\n"
        "for (c = 0; c < w; c = c + 1) { setRGB(c, 0, 0, 255); }\n"
        "for (d = 0; d < w; d = d + 1) { setRGB(d, 255, 255, 0); }"),
        moonlive::lightBuiltins(), moonlive::modifierSysVars(), code, sizeof(code));
    if (!r.ok) INFO(r.error);
    // What this pins is REGISTER REUSE, which the front-end does on every host — but proving it
    // needs code to come out, and only a host with an assembler for its ISA emits any
    // (MM_MOONLIVE_HAS_HOST_JIT is 0 on x86_64, which is what CI runs). Requiring success there
    // fails for the one reason that has nothing to do with register reuse.
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(r.ok);
#else
    CHECK((r.ok || std::string(r.error) == moonlive::kCodegenFailed));
#endif
}
