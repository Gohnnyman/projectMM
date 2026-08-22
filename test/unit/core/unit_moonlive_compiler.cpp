// @module MoonLive

#include "doctest.h"
#include "moonlive_script_wrap.h"
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
    auto buf = render(mmScript("fill(10, 20, 200);"), 8);
    for (int i = 0; i < 8; i++) { CHECK(buf[i*3]==10); CHECK(buf[i*3+1]==20); CHECK(buf[i*3+2]==200); }
}

TEST_CASE("compileSource: setRGB(index, r,g,b) writes one pixel") {
    auto buf = render(mmScript("setRGB(3, 255, 0, 0);"), 8);
    for (int i = 0; i < 8; i++) {
        uint8_t want = (i == 3) ? 255 : 0;
        CHECK(buf[i*3] == want);
    }
}

// A function the script calls is handed the same lights and the same controls as the one calling
// it. Both halves matter and they fail differently: without the buffer the helper writes nowhere
// visible, and without the controls arena its first control read dereferences a null pointer.
//
// Found on hardware, not here: an S3 running a three-function effect died with LoadProhibited and
// EXCVADDR 0x9: offset 9 into a null `ctrls`. Every backend had it, including this one, because a
// local call emitted the call instruction alone while each function's prologue parks the host
// arguments out of the argument registers into its own frame. The callee therefore parked whatever
// the caller had last left in them. The whole-block tests could not see it: they check that a
// script COMPILES, and this is a script that compiles perfectly and then reads the wrong memory.
TEST_CASE("a function the script calls can light pixels and read the script's controls") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  uint8_t level = 200;\n"
                        "  paint() { setRGB(1, level, 0, 0); }\n"
                        "  tick()  { setRGB(0, 7, 8, 9); paint(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    // Named, as a binding does: an unnamed run enters the block at its FIRST function, which for a
    // class is whichever the script declared first rather than the entry point a role asks for.
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 7);        // the caller's own write still lands
    CHECK(buf[3] == 200);      // and the callee reached both the buffer and the control's value
}

// A helper that calls a helper: the arguments have to survive being passed on twice, not just once.
// One level of nesting would pass even if a call clobbered what it forwards.
TEST_CASE("arguments reach a function two calls deep") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  inner()  { setRGB(2, 44, 0, 0); }\n"
                        "  outer()  { setRGB(1, 33, 0, 0); inner(); }\n"
                        "  tick()   { setRGB(0, 22, 0, 0); outer(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 22);
    CHECK(buf[3] == 33);
    CHECK(buf[6] == 44);
}

// UNBOUNDED recursion must degrade visibly and keep the device rendering. A fixed render-task
// stack means the alternative is a reset, which the robustness rule forbids. A reset is what
// this script produced before the depth guard: ~176 bytes of frame per activation against a 12 KB
// main task is a device that reboots at roughly 64 deep, mid-frame, with no diagnostic.
//
// The guard lives in the callee's prologue and refuses by returning, so the recursion stops at
// kMaxCallDepth and everything above it still runs. What the user sees is a picture that is wrong
// where the recursion bottomed out, on a device that is still running.
TEST_CASE("a script that recurses without end keeps rendering instead of resetting") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  forever() { setRGB(1, 200, 0, 0); forever(); }\n"
                        "  tick()    { setRGB(0, 50, 0, 0); forever(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());                       // it COMPILES: whether it terminates is not decidable
    std::vector<uint8_t> buf(4 * 3, 0);
    // The assertion is that this RETURNS at all. Without the guard it recurses until the stack is
    // gone: a segfault here on the host, a reset on a board.
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 50);        // the entry function ran
    CHECK(buf[3] == 200);       // and so did the recursion, as far as it was allowed
    // Runnable AGAIN, at full depth: the counter unwinds with the frames, so hitting the limit in
    // one frame must not shrink the next. A leaked level per frame would silently reduce every
    // later frame's budget until nothing recursed at all.
    std::vector<uint8_t> second(4 * 3, 0);
    eng.run(second.data(), 4, 3, 0, "tick");
    CHECK(second[0] == 50);
    CHECK(second[3] == 200);
}

// A function with an EMPTY body still balances the recursion counter.
//
// The depth guard is emitted at a function's first real op, because it has to follow the host
// arguments being parked into the frame. A function whose whole body is that parking has no first
// real op, so it got no increment while its epilogue still decremented: every call to it drove the
// counter DOWN, two calls wrapped the byte past zero, and the next legal call was refused as too
// deep. What a user saw was a call that silently did nothing, in a script with no recursion in it.
TEST_CASE("an empty function does not consume the recursion budget") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  nop()  { }\n"
                        "  draw() { setRGB(0, 255, 0, 0); }\n"
                        "  tick() { nop(); nop(); draw(); }\n"
                        "}\n", kTable, kSys));
    REQUIRE(eng.ok());
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0, "tick");
    CHECK(buf[0] == 255);   // draw() ran: the two empty calls left the budget where they found it
}

// An over-long function name is REFUSED, not truncated. The engine copies entry names into a
// fixed buffer, so a longer one would be clipped there: and two functions sharing a 23-character
// prefix would then land under the same name, with `entry()` returning whichever came first. A
// call would dispatch to the wrong function and nothing would say so.
TEST_CASE("a function name too long to store is refused, not silently truncated") {
    uint8_t out[2048];
    std::string longName(mm::moonlive::kMaxEntryName + 1, 'a');
    const std::string src = "class T {\n  " + longName + "() { }\n  tick() { }\n}\n";
    auto r = moonlive::compileSource(src.c_str(), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::strlen(r.error) > 0);
    // One character shorter is fine, so the limit is the limit and not an off-by-one.
    const std::string ok = "class T {\n  " + longName.substr(1) + "() { }\n  tick() { }\n}\n";
    auto r2 = moonlive::compileSource(ok.c_str(), kTable, kSys, out, sizeof(out));
    CHECK(r2.ok);
}

// REMARK #1: every argument is an expression — random16 in ANY slot.
TEST_CASE("compileSource: random16 works in any argument slot") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("setRGB(random16(8), random16(256), 30, 0);"), kTable, kSys));
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
    CHECK(eng.compile(mmScript("setRGB(random16(65535), 0, 0, 255);"), kTable, kSys));   // 65535 accepted
    CHECK(eng.compile(mmScript("setRGB(1000, 0, 0, 255);"), kTable, kSys));              // literal index > 255 ok
    uint8_t out[256];
    auto r = moonlive::compileSource(mmScript("setRGB(70000, 0, 0, 0);"), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);   // 70000 > 65535 → rejected
}

TEST_CASE("compileSource: out-of-range index is bounds-rejected at runtime") {
    auto buf = render(mmScript("setRGB(5000, 255, 255, 255);"), 8);   // 5000 >> 8 lights
    for (auto v : buf) CHECK(v == 0);                       // guarded — nothing written
}

TEST_CASE("compileSource rejects malformed programs with a diagnostic, never crashes") {
    uint8_t out[256];
    // Each of these MUST fail — assert it, so an accidental successful compile is caught (not
    // just "no crash"). Wrong arity, unknown name, unbalanced parens, trailing junk, empty.
    const char* bad[] = {
        "",                                  // empty
        mmScript("setRGB(0,0,0,0,0);"),                // too many args
        mmScript("fill(0,0);"),                        // too few args
        "wibble(1);",                        // unknown function
        mmScript("setRGB(0, 0, 0"),                    // missing ')'  and ';'
        mmScript("fill(0,0,0)"),                       // missing ';'
        mmScript("fill(0,0,0); extra"),                // trailing junk
        mmScript("setRGB(random8(8), 0, 0, 0);"),      // unknown nested function
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);                   // the parser contract: malformed → rejected
        CHECK(std::strlen(r.error) > 0);     // …with a diagnostic
    }
    // A value-returning function used as a void statement IS valid (result discarded).
    CHECK(moonlive::compileSource(mmScript("random16(8);"), kTable, kSys, out, sizeof(out)).ok);
}
#endif  // MM_MOONLIVE_HAS_HOST_JIT

TEST_CASE("MoonLive.compile(source) on a bad script leaves the engine !ok with an error") {
    moonlive::MoonLive eng;
    CHECK_FALSE(eng.compile(mmScript("setRGB(oops);"), kTable, kSys));
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
            mmScript("setRGB(random16(64), random16(256), 30, 0);"),                          // 2 calls
            mmScript("setRGB(random16(128), random16(256), random16(256), 0);"),              // 3 calls
            mmScript("setRGB(random16(128), random16(256), random16(256), random16(256));"),  // 4 calls
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
    for (const char* s : {mmScript("setRGB(0,0,0,0);"), mmScript("fill(0,0,0);"), "random16(8);"}) {
        auto r = moonlive::compileSource(s, empty, {}, out, sizeof(out));
        CHECK_FALSE(r.ok);                       // the core doesn't know any of these
        CHECK(std::strlen(r.error) > 0);
    }
    // A host can register an arbitrary name against the same neutral machinery.
    moonlive::BuiltinTable custom;
    custom.add({"paint", 4, false, moonlive::BuiltinKind::Inline, nullptr, moonlive::InlineOp::StoreElem});
    auto r = moonlive::compileSource(mmScript("paint(2, 9, 8, 7);"), custom, {}, out, sizeof(out));
    CHECK(r.ok);                                 // a different name, same core path
}

TEST_CASE("MoonLive recompiling swaps the program live (fill <-> setRGB)") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile(mmScript("fill(0,0,255);"), kTable, kSys));
    std::vector<uint8_t> buf(4 * 3, 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[0*3+2] == 255); CHECK(buf[3*3+2] == 255);

    REQUIRE(eng.compile(mmScript("setRGB(1, 255, 0, 0);"), kTable, kSys));
    std::fill(buf.begin(), buf.end(), 0);
    eng.run(buf.data(), 4, 3, 0);
    CHECK(buf[1*3+0] == 255); CHECK(buf[0] == 0);
}

// CONTROLS: a declaration is a member, and `addUint8("name", name, lo, hi)` in defineControls
// A control is declared by CALLING addUint8 inside defineControls, the same call a compiled module
// makes. The declaration alone is a member: state the script owns, which the UI never sees unless
// the script asks for it. That split is the whole point, so both halves are checked here.
//
// Engine-level rather than compileSource-level, because a control now exists because a function
// RAN: compileSource emits the code, and runDefineControls executes it.
TEST_CASE("a control is declared by calling addUint8, and a plain member is not") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  uint8_t speed = 50;\n"
                        "  uint8_t hidden = 7;\n"
                        "  defineControls() { addUint8(\"speed\", speed, 0, 99); }\n"
                        "  tick() { setRGB(0, speed, hidden, 255); }\n"
                        "}\n", kTable, kSys));
    moonlive::runDefineControls(eng);

    uint8_t n = 0;
    const auto* c = eng.declaredControls(n);
    REQUIRE(n == 1);                                  // `hidden` is a member, not a control
    CHECK(std::strcmp(c[0].name, "speed") == 0);
    CHECK(c[0].min == 0); CHECK(c[0].max == 99);
    CHECK(c[0].def == 50);                            // from the member's initializer
    CHECK(c[0].type == moonlive::CtrlType::Uint8);

    // Both members hold their declared values, whether or not a control surfaces them: the
    // initializer seeds the arena, which is what makes a member state rather than a constant.
    uint8_t buf[3] = {};
    eng.run(buf, 1, 3, 0, "tick");
    CHECK(buf[0] == 50);                              // `speed`, which the UI also shows
    CHECK(buf[1] == 7);                               // `hidden`, read by tick, never on the UI
}

// A control's range is an ORDINARY EXPRESSION, like every other argument in the language. Making
// addUint8 the one call whose arguments must be literals would be a special case wearing a
// disguise, so this pins that it is not one.
TEST_CASE("a control's range can be computed, not just written as a literal") {
    moonlive::MoonLive eng;
    REQUIRE(eng.compile("class T {\n"
                        "  uint8_t base = 10;\n"
                        "  uint8_t speed = 20;\n"
                        "  defineControls() { addUint8(\"speed\", speed, base, base * 4 + 5); }\n"
                        "  tick() { setRGB(0, speed, 0, 0); }\n"
                        "}\n", kTable, kSys));
    moonlive::runDefineControls(eng);
    uint8_t n = 0;
    const auto* c = eng.declaredControls(n);
    REQUIRE(n == 1);
    CHECK(c[0].min == 10);        // base
    CHECK(c[0].max == 45);        // base * 4 + 5
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
        {mmScript("uint8_t width = 16;\nsetRGB(0, 0, 0, 0);"), "a control named width"},
        {mmScript("uint8_t t = 5;\nsetRGB(0, 0, 0, 0);"),                        "a control named t"},
        {mmScript("for (xPos = 0; xPos < 4; xPos = xPos + 1) { setRGB(xPos, 0, 0, 0); }"),
                                                                        "a loop variable named xPos"},
        {mmScript("for (height = 0; height < 4; height = height + 1) { setRGB(0, 0, 0, 0); }"),
                                                                        "a loop variable named height"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == "name is a system variable");   // the clash is named, not generic
    }
    // The same names READ fine — refusing the declaration is what keeps the read meaningful.
    auto ok = moonlive::compileSource(mmScript("setRGB(width, height, depth, t);"), kTable, kSys,
                                      out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);                                        // a backend exists: it must emit
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));   // parses; no backend here
#endif
    // A name the host did NOT register is an ordinary control, not a reserved word.
    auto own = moonlive::compileSource(mmScript("uint8_t cols = 16;\nsetRGB(cols, 0, 0, 0);"), kTable, kSys,
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
        mmScript("for (i = 0; i < 2; i = i + 1) { for (i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } }"),
        kTable, kSys, out, sizeof(out));
    CHECK_FALSE(r.ok);
    CHECK(std::string(r.error) == "loop variable already in use");
    // Distinct names nest fine — the check must not refuse the ordinary case it exists to protect.
    auto ok = moonlive::compileSource(
        mmScript("for (yy = 0; yy < 2; yy = yy + 1) { for (xx = 0; xx < 2; xx = xx + 1) { addLight(xx, yy, 0); } }"),
        kTable, kSys, out, sizeof(out));
#if MM_MOONLIVE_HAS_HOST_JIT
    CHECK(ok.ok);
#else
    CHECK((ok.ok || std::string(ok.error) == moonlive::kCodegenFailed));
#endif
    // Sequential loops REUSE a name legitimately: the first has left scope by the time the second
    // binds, so this must still compile (two-rows.mll is exactly this shape).
    auto seq = moonlive::compileSource(
        mmScript("for (i = 0; i < 2; i = i + 1) { addLight(i, 0, 0); } for (i = 0; i < 2; i = i + 1) { addLight(i, 1, 0); }"),
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
        {mmScript("for (i = 0; j < 3; i = i + 1) { addLight(i, 0, 0); }"),
         "the condition must test the loop variable", "a typo in the condition"},
        {mmScript("for (i = 0; i < 3; j = j + 1) { addLight(i, 0, 0); }"),
         "the step must advance the loop variable",   "a typo in the step"},
        // Plain names, not x/y: those are system variables in this table and would be refused a
        // step earlier, hiding what this case is about.
        {mmScript("for (a = 0; a < 4; a = a + 1) { for (b = 0; a < 4; b = b + 1) { addLight(b, a, 0); } }"),
         "the condition must test the loop variable", "an inner loop testing the OUTER variable"},
        // The step is re-lexed from the source it was skipped over, and an expression parser stops
        // at the first token it cannot use — so trailing junk was silently dropped.
        {mmScript("for (i = 0; i < 3; i = i + 1 garbage) { addLight(i, 0, 0); }"),
         "unexpected token in the for's step", "trailing junk after the step expression"},
    };
    for (const Case& c : refused) {
        INFO(c.what);
        auto r = moonlive::compileSource(c.src, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::string(r.error) == c.err);
    }
    // The ordinary loop is untouched.
    auto ok = moonlive::compileSource(mmScript("for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }"),
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
    std::string body = "addLight(1";
    for (int i = 0; i < 200; i++) body += " + 1";
    body += ", 0, 0);";
    const std::string many = mmScript(body.c_str());
    auto r = moonlive::compileSource(many.c_str(), kTable, kSys, out, sizeof(out));
    // Either answer is fine — what is NOT fine is never returning, which is what this pins.
    CHECK((r.ok || std::strlen(r.error) > 0));

    // And the sanity bound still refuses a runaway rather than trying to allocate for it.
    // Built directly rather than through mmScript: 3000 statements is far past any fixed buffer,
    // which is the whole point of the case.
    std::string absurd = "class Runaway {\n  tick() {\n";
    for (int i = 0; i < 3000; i++) absurd += "addLight(1, 0, 0);";
    absurd += "\n  }\n}\n";
    auto big = moonlive::compileSource(absurd.c_str(), kTable, kSys, out, sizeof(out));
    CHECK_FALSE(big.ok);
    CHECK(std::string(big.error) == "script too large");
}

TEST_CASE("compileSource: malformed control declarations fail with a diagnostic, never crash") {
    uint8_t out[768];
    const char* bad[] = {
        mmScript("uint8_t speed 50; setRGB(0,0,0,0);"),                        // missing '='
        mmScript("uint8_t speed = 300; setRGB(0,0,0,0);"),                     // default > 255
        // The range cases moved to defineControls, where a range now lives. A comment cannot be
        // malformed any more, because a comment no longer declares anything.
        "class T {\n  uint8_t s = 5;\n  defineControls() { addUint8(\"s\", nope, 0, 9); }\n"
        "  tick() { setRGB(0,0,0,0); }\n}\n",                                 // binds an undeclared member
        "class T {\n  uint8_t s = 5;\n  defineControls() { addUint8(s, s, 0, 9); }\n"
        "  tick() { setRGB(0,0,0,0); }\n}\n",                                 // name is not a string
        mmScript("uint8_t random16 = 5; setRGB(0,0,0,0);"),                    // name shadows a builtin
        "uint8_t speed = 50;",                                       // not even a class
        mmScript("uint8_t = 50; setRGB(0,0,0,0);"),                            // no name
        mmScript("uint8_t s = 1; uint8_t s = 2; setRGB(0,0,0,0);"),            // duplicate member name
    };
    for (auto s : bad) {
        auto r = moonlive::compileSource(s, kTable, kSys, out, sizeof(out));
        CHECK_FALSE(r.ok);
        CHECK(std::strlen(r.error) > 0);
    }
}

// The SYMBOL TABLE: which functions a class defined, and where each one's code starts.
//
// A binding asks for an entry by name and gets an address inside the single emitted block, which is
// how a compiler and a linker have always worked: one code section, a name-to-offset map over it.
// With one function the map is trivially right (its body is the whole program), so the case that
// proves anything is TWO: the second must start after the first, not at zero.
TEST_CASE("a class reports every function it defined, and where each one starts") {
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        "class TwoFns {\n"
        "  helper() { setRGB(1, 10, 20, 30); }\n"
        "  tick()   { setRGB(2, 40, 50, 60); }\n"
        "}\n", kTable, kSys, out, sizeof(out));
    // The entry table is published only by a SUCCESSFUL compile: compileSource returns at codegen
    // failure, before it copies the table, which is the same rule the declared controls follow (a
    // failed compile must not advertise the shape of code that is not running). So a host with no
    // MoonLive backend has nothing to check here.
#if !MM_MOONLIVE_HAS_HOST_JIT
    return;
#endif
    REQUIRE(r.ok);
    REQUIRE(r.entryCount == 2);

    // In source order, and named as written.
    CHECK(std::string(r.entries[0].name, r.entries[0].nameLen) == "helper");
    CHECK(std::string(r.entries[1].name, r.entries[1].nameLen) == "tick");

    // The first entry opens the block; the second is further in. Without the offset map both would
    // read 0, and a binding calling `tick` would run `helper` instead: silently, since both compile.
    CHECK(r.entries[1].offset > r.entries[0].offset);
    CHECK(r.entries[1].offset < r.len);
}

// A script may define functions the host does not know about. They are still recorded, because
// which names exist is what decides a script's ROLE once the per-role entry points arrive.
TEST_CASE("a function the host has no name for is still reported") {
    uint8_t out[4096];
    auto r = moonlive::compileSource(
        "class Helpers {\n  paint() { setRGB(0, 1, 2, 3); }\n}\n", kTable, kSys, out, sizeof(out));
#if !MM_MOONLIVE_HAS_HOST_JIT
    return;                       // no backend: no successful compile, so no table to report
#endif
    REQUIRE(r.ok);
    REQUIRE(r.entryCount == 1);
    CHECK(std::string(r.entries[0].name, r.entries[0].nameLen) == "paint");
}

#if MM_MOONLIVE_HAS_HOST_JIT
// The '/' and '%' operators. Both lower to a host call (no ISA here has a divide), so what needs
// pinning is not the arithmetic but the GRAMMAR: a hand-written precedence-climbing parser gets
// binding wrong silently, and a wrong answer here is indistinguishable from a working effect.
// The rule the parser must not get backwards: `/` and `%` bind tighter than `+`, and equally with
// `*`, so a chain runs left to right. `12 / 2 * 3` is 18; grouping it as 12 / (2 * 3) gives 2.
TEST_CASE("division binds tighter than addition and left to right with multiplication") {
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[0] == 18);
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[1] == 5);
    CHECK(render(mmScript("setRGB(0, 12 / 2 * 3, 2 + 12 / 4, 2 + 20 % 7);"), 1)[2] == 8);
}

// Parentheses override the precedence, which is what makes the operators usable at all.
TEST_CASE("parentheses group an expression ahead of division") {
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[0] == 2);
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[1] == 20);
    CHECK(render(mmScript("setRGB(0, (2 + 12) / 7, (3 + 1) * 5, 3 + 1 * 5);"), 1)[2] == 8);
}

// A script must degrade, never fault. Dividing by zero is the one input the hardware would trap
// on, and it reaches the host helper as an ordinary value.
TEST_CASE("dividing by zero yields zero rather than faulting") {
    CHECK(render(mmScript("setRGB(0, 100 / 0, 100 % 0, 0);"), 1)[0] == 0);
    CHECK(render(mmScript("setRGB(0, 100 / 0, 100 % 0, 0);"), 1)[1] == 0);
}
#endif
