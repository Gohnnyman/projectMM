// @module MoonLive

#include "doctest.h"
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLive.h"
#include "core/moonlive/moonlive_emit.h"   // MM_MOONLIVE_HAS_HOST_JIT — the assembler+emit gate
#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// MoonLive front-end: an expression grammar where any argument may be a nested call, and the
// functions (setRGB / fill / random16) are resolved against a host-registered BuiltinTable
// (the light domain's). The core compiler owns no LED vocabulary — these tests drive it
// through the light table the same way the binding does.

using namespace mm;

static moonlive::BuiltinTable kTable = moonlive::lightBuiltins();
static moonlive::SysVarTable kSys = moonlive::modifierSysVars();

// Compile + run a source on a w-light, 3-channel buffer; returns the rendered buffer.
// Only used by the JIT-gated tests below; guard the definition too so a non-JIT build
// doesn't fire /W4's "unreferenced static function" (which -Werror escalates).
#if MM_MOONLIVE_HAS_HOST_JIT
static std::vector<uint8_t> render(const char* src, int nLights, uint32_t t = 0) {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(src, kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(nLights * 3, 0);
    eng.run(buf.data(), nLights, 3, t);
    return buf;
}
#endif

// The compile-through-run tests need a working host JIT — the assembler (moonlive_asm_host.cpp)
// is arm64-only today, so on x86_64 desktops compileSource returns !ok ("codegen failed") and
// every "should compile" assertion fails. Guarded on the emit-header capability macro so they
// compile out where the backend is unimplemented — the same "runs dark" degradation on-device.
// The malformed-input tests further down don't gate: they assert failure, which succeeds for
// the right reason (parse rejects) on arm64 and for a compatible reason (no codegen) on x86_64.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("compileSource: fill(r,g,b) fills every light") {
    auto buf = render("fill(10, 20, 200);", 8);
    for (int i = 0; i < 8; i++) { CHECK(buf[i*3]==10); CHECK(buf[i*3+1]==20); CHECK(buf[i*3+2]==200); }
}

TEST_CASE("compileSource: setRGB(index, r,g,b) writes one pixel") {
    auto buf = render("setRGB(3, 255, 0, 0);", 8);
    for (int i = 0; i < 8; i++) {
        uint8_t want = (i == 3) ? 255 : 0;
        CHECK(buf[i*3] == want);
    }
}

// REMARK #1: every argument is an expression — random16 in ANY slot.
TEST_CASE("compileSource: random16 works in any argument slot") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("setRGB(random16(8), random16(256), 30, 0);", kTable, kSys));
    REQUIRE(eng.ok());
    for (int run = 0; run < 32; run++) {
        std::vector<uint8_t> buf(8 * 3, 0);
        eng.run(buf.data(), 8, 3, 0);
        int lit = 0;
        for (int i = 0; i < 8; i++) if (buf[i*3] || buf[i*3+1] || buf[i*3+2]) lit++;
        CHECK(lit == 1);   // one random pixel, with a random red — exactly one lit
    }
}

// REMARK #2: a literal / random16 bound may be a uint16 (0..65535), not capped at 255.
TEST_CASE("compileSource: random16 accepts a uint16 bound (>255)") {
    moonlive::MoonLive eng;
    CHECK(eng.compile("setRGB(random16(65535), 0, 0, 255);", kTable, kSys));   // 65535 accepted
    CHECK(eng.compile("setRGB(1000, 0, 0, 255);", kTable, kSys));              // literal index > 255 ok
    uint8_t out[256];
    auto r = moonlive::compileSource("setRGB(70000, 0, 0, 0);", kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);   // 70000 > 65535 → rejected
}

TEST_CASE("compileSource: out-of-range index is bounds-rejected at runtime") {
    auto buf = render("setRGB(5000, 255, 255, 255);", 8);   // 5000 >> 8 lights
    for (auto v : buf) CHECK(v == 0);                       // guarded — nothing written
}

TEST_CASE("compileSource rejects malformed programs with a diagnostic, never crashes") {
    uint8_t out[256];
    // Each of these MUST fail — assert it, so an accidental successful compile is caught (not
    // just "no crash"). Wrong arity, unknown name, unbalanced parens, trailing junk, empty.
    const char* bad[] = {
        "",                                  // empty
        "setRGB(0,0,0,0,0);",                // too many args
        "fill(0,0);",                        // too few args
        "wibble(1);",                        // unknown function
        "setRGB(0, 0, 0",                    // missing ')'  and ';'
        "fill(0,0,0)",                       // missing ';'
        "fill(0,0,0); extra",                // trailing junk
        "setRGB(random8(8), 0, 0, 0);",      // unknown nested function
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);                   // the parser contract: malformed → rejected
        CHECK(std::strlen(r.error) > 0);     // …with a diagnostic
    }
    // A value-returning function used as a void statement IS valid (result discarded).
    CHECK(moonlive::compileSource("random16(8);", kTable, kSys, out, sizeof(out)).ok);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("MoonLive.compile(source) on a bad script leaves the engine !ok with an error") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile("setRGB(oops);", kTable, kSys));
    CHECK_FALSE(eng.ok());
    CHECK(std::strlen(eng.error()) > 0);
    std::vector<uint8_t> buf(3, 0xAB);
    eng.run(buf.data(), 1, 3, 0);
    CHECK(buf[0] == 0xAB);
}

// VREG REUSE: a chain of calls must fit the small device register file. Each argument temp dies
// once its call consumes it and is recycled, so peak register pressure stays low no matter how
// many calls a statement nests — setRGB with all four arguments a random16 still compiles.
#if MM_MOONLIVE_HAS_HOST_JIT
TEST_CASE("a multi-call statement reuses dead vregs and stays within the register budget") {
    moonlive::BuiltinTable t = moonlive::lightBuiltins();
    uint8_t out[768];
    for (const char* s : {
            "setRGB(random16(64), random16(256), 30, 0);",                          // 2 calls
            "setRGB(random16(128), random16(256), random16(256), 0);",              // 3 calls
            "setRGB(random16(128), random16(256), random16(256), random16(256));",  // 4 calls
         }) {
        auto r = moonlive::compileSource(s, t, kSys, out, sizeof(out));
        CHECK(r.ok);          // without vreg reuse the 3-/4-call cases overflow the register file
        CHECK(r.len > 0);
    }
}

// DOMAIN-NEUTRAL: the core compiler owns no function names. With an EMPTY table it knows
// nothing — `setRGB`/`fill`/`random16` are all "unknown function". The LED vocabulary lives
// only in the host's table; a different host registers different names. (Remark #3.)
TEST_CASE("core compiler has no built-in functions of its own (empty table → all unknown)") {
    moonlive::BuiltinTable empty;
    uint8_t out[256];
    for (const char* s : {"setRGB(0,0,0,0);", "fill(0,0,0);", "random16(8);"}) {
        auto r = moonlive::compileSource(s, empty, {}, out, sizeof(out));
        CHECK_FALSE(r.ok);                       // the core doesn't know any of these
        CHECK(std::strlen(r.error) > 0);
    }
    // A host can register an arbitrary name against the same neutral machinery.
    moonlive::BuiltinTable custom;
    custom.add({"paint", 4, false, moonlive::BuiltinKind::Inline, nullptr, moonlive::InlineOp::StoreElem});
    auto r = moonlive::compileSource("paint(2, 9, 8, 7);", custom, {}, out, sizeof(out));
    CHECK(r.ok);                                 // a different name, same core path
}

TEST_CASE("MoonLive recompiling swaps the program live (fill <-> setRGB)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("fill(0,0,255);", kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[0*3+2] == 255); CHECK(buf[3*3+2] == 255);

    REQUIRE(eng.compile("setRGB(1, 255, 0, 0);", kTable, kSys));
    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[1*3+0] == 255); CHECK(buf[0] == 0);
}

// STAGE 1 CONTROLS — parse layer: a `uint8_t name = def; // @control min..max` declaration
// surfaces a DeclaredControl, and a declared name used in a statement resolves to it.
// The DeclaredControl tests also need lowerToBytes to return non-zero — r.ok gates on it.
TEST_CASE("compileSource: a control declaration surfaces a DeclaredControl") {
    uint8_t out[768];
    auto r = moonlive::compileSource(
        "uint8_t speed = 50; // @control 0..99\nsetRGB(speed, 0, 0, 255);", kTable, kSys, out, sizeof(out));
    REQUIRE(r.ok);
    REQUIRE(r.controlCount == 1);
    const auto& c = r.controls[0];
    CHECK(std::strncmp(c.name, "speed", c.nameLen) == 0);
    CHECK(c.nameLen == 5);
    CHECK(c.min == 0); CHECK(c.max == 99); CHECK(c.def == 50); CHECK(c.offset == 0);
    CHECK(c.type == moonlive::CtrlType::Uint8);

    // No annotation → default 0..255; two controls get sequential offsets (each default in range).
    auto r2 = moonlive::compileSource(
        "uint8_t a = 10;\nuint8_t b = 5; // @control 1..7\nsetRGB(a, b, 0, 0);", kTable, kSys, out, sizeof(out));
    REQUIRE(r2.ok);
    REQUIRE(r2.controlCount == 2);
    CHECK(r2.controls[0].max == 255); CHECK(r2.controls[0].offset == 0);   // a: no anno
    CHECK(r2.controls[1].min == 1); CHECK(r2.controls[1].max == 7); CHECK(r2.controls[1].def == 5); CHECK(r2.controls[1].offset == 1);

    // `@control` matches as a whole word: a comment whose first word merely STARTS
    // with "@control" (e.g. "@controlled") is a plain comment, not a malformed
    // annotation — it's skipped, the declaration takes the default 0..255 range.
    auto r3 = moonlive::compileSource(
        "uint8_t speed = 9; // @controlled by the user\nsetRGB(speed, 0, 0, 0);", kTable, kSys, out, sizeof(out));
    REQUIRE(r3.ok);
    REQUIRE(r3.controlCount == 1);
    CHECK(r3.controls[0].min == 0); CHECK(r3.controls[0].max == 255); CHECK(r3.controls[0].def == 9);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

// A system variable is a value the HOST hands the script — the layer's size, the light being
// transformed, the clock. Letting a script declare the same name would shadow the value it is being
// given, silently: an effect declaring `width = 16` on an 8x8 panel draws off the edge, and every
// statement in it still runs perfectly. So the name is refused wherever a name can be introduced,
// which is exactly two places: a control declaration and a `for` loop variable.
TEST_CASE("a script cannot declare a name the engine already defines") {
    uint8_t out[512];
    struct Case { const char* src; const char* what; };
    const Case refused[] = {
        {"uint8_t width = 16; // @control 1..64\nsetRGB(0, 0, 0, 0);", "a control named width"},
        {"uint8_t t = 5;\nsetRGB(0, 0, 0, 0);",                        "a control named t"},
        {"for (xPos = 0; xPos < 4; xPos = xPos + 1) { setRGB(xPos, 0, 0, 0); }",
                                                                        "a loop variable named xPos"},
        {"for (height = 0; height < 4; height = height + 1) { setRGB(0, 0, 0, 0); }",
                                                                        "a loop variable named height"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == "name is a system variable");   // the clash is named, not generic
    }
    // The same names READ fine — refusing the declaration is what keeps the read meaningful.
    auto ok = moonlive::compileSource("setRGB(width, height, depth, t);", kTable, kSys,
                                      out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);                                        // a backend exists: it must emit
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));   // parses; no backend here
#endif
    // A name the host did NOT register is an ordinary control, not a reserved word.
    auto own = moonlive::compileSource("uint8_t cols = 16;\nsetRGB(cols, 0, 0, 0);", kTable, kSys,
                                       out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(own.ok);
#else
    CHECK((own.ok || std::string(own.error) == moonlive::kCodegenFailed));
#endif
}

// Found by review: this compiled cleanly and emitted a program that NEVER RETURNED. The inner loop
// bound a second register to the same name, so its step wrote the register the outer back edge
// tested and the counter never advanced — a hang on the render task, from a script a user can type
// into the editor. Robustness says any input degrades visibly rather than wedging the device.
TEST_CASE("a nested loop cannot reuse the enclosing loop's variable") {
    uint8_t out[512];
    auto r = moonlive::compileSource(
        "for (i = 0; i < 2; i = i + 1) { for (i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } }",
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "loop variable already in use");
    // Distinct names nest fine — the check must not refuse the ordinary case it exists to protect.
    auto ok = moonlive::compileSource(
        "for (yy = 0; yy < 2; yy = yy + 1) { for (xx = 0; xx < 2; xx = xx + 1) { addLight(xx, yy, 0); } }",
        kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));
#endif
    // Sequential loops REUSE a name legitimately: the first has left scope by the time the second
    // binds, so this must still compile (two-rows.mlv is exactly this shape).
    auto seq = moonlive::compileSource(
        "for (i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } for (i = 0; i < 2; i = i + 1) { addLight(i, 1, 0); }",
        kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(seq.ok);
#else
    CHECK((seq.ok || std::string(seq.error) == moonlive::kCodegenFailed));
#endif
}

// The emitted loop tests and advances its OWN counter whatever name the condition and step clauses
// write, so a mistyped name used to compile clean and run as though it said the right thing — a
// wrong fixture with no diagnostic anywhere. Found by review.
TEST_CASE("a for loop's condition and step must name the loop variable") {
    uint8_t out[512];
    struct Case { const char* src; const char* err; const char* what; };
    const Case refused[] = {
        {"for (i = 0; j < 3; i = i + 1) { addLight(i, 0, 0); }",
         "the condition must test the loop variable", "a typo in the condition"},
        {"for (i = 0; i < 3; j = j + 1) { addLight(i, 0, 0); }",
         "the step must advance the loop variable",   "a typo in the step"},
        // Plain names, not x/y: those are system variables in this table and would be refused a
        // step earlier, hiding what this case is about.
        {"for (a = 0; a < 4; a = a + 1) { for (b = 0; a < 4; b = b + 1) { addLight(b, a, 0); } }",
         "the condition must test the loop variable", "an inner loop testing the OUTER variable"},
        // The step is re-lexed from the source it was skipped over, and an expression parser stops
        // at the first token it cannot use — so trailing junk was silently dropped.
        {"for (i = 0; i < 3; i = i + 1 garbage) { addLight(i, 0, 0); }",
         "unexpected token in the for's step", "trailing junk after the step expression"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == c.err);
    }
    // The ordinary loop is untouched.
    auto ok = moonlive::compileSource("for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }",
                                      kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));
#endif
}

// The op array is sized to the script, so `count` is a uint16_t — and every loop over it has to be
// one too. A uint8_t counter wrapped at 256 ops and spun forever, which on a device is a watchdog
// reset from a script that merely got long. Found by bisecting: 60 statements fine, 80 hung.
TEST_CASE("a long script compiles or refuses, but never spins") {
    uint8_t out[16384];
    // A long ARITHMETIC chain, not many statements: each `+ 1` is one cheap op, so this passes 256
    // IR ops while staying inside the code buffer. Repeated statements hit the code ceiling first
    // and return before the wrap, which is why they do not pin this.
    std::string many = "addLight(1";
    for (int i = 0; i < 200; i++) many += " + 1";
    many += ", 0, 0);";
    auto r = moonlive::compileSource(many.c_str(), kTable, kSys, out, sizeof(out));
    // Either answer is fine — what is NOT fine is never returning, which is what this pins.
    CHECK((r.ok || std::strlen(r.error) > 0));

    // And the sanity bound still refuses a runaway rather than trying to allocate for it.
    std::string absurd;
    for (int i = 0; i < 3000; i++) absurd += "addLight(1, 0, 0);";
    auto big = moonlive::compileSource(absurd.c_str(), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(big.ok);
    CHECK(std::string(big.error) == "script too large");
}

TEST_CASE("compileSource: malformed control declarations fail with a diagnostic, never crash") {
    uint8_t out[768];
    const char* bad[] = {
        "uint8_t speed 50; setRGB(0,0,0,0);",                        // missing '='
        "uint8_t speed = 300; setRGB(0,0,0,0);",                     // default > 255
        "uint8_t speed = 50; // @control 99..0\nsetRGB(0,0,0,0);",   // reversed range
        "uint8_t speed = 50; // @control 0..10\nsetRGB(0,0,0,0);",   // default outside @control range
        "uint8_t speed = 50; // @control 5\nsetRGB(0,0,0,0);",       // lexer-level malformed: no `..max` (Tok::Error surfaced, not a generic fall-through)
        "uint8_t speed = 50; // @control 5..\nsetRGB(0,0,0,0);",     // lexer-level malformed: missing max
        "uint8_t random16 = 5; setRGB(0,0,0,0);",                    // name shadows a builtin
        "uint8_t speed = 50;",                                       // no statement
        "uint8_t = 50; setRGB(0,0,0,0);",                            // no name
        "uint8_t s = 1; uint8_t s = 2; setRGB(0,0,0,0);",            // duplicate name
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::strlen(r.error) > 0);
    }
}
