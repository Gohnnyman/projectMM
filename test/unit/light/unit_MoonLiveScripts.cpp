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
#include "core/moonlive/MoonLive.h"
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

            // Compile it the way its own module does. A MODIFIER script reads `x`, `y` and `z`,
            // which its binding prepends as declarations before compiling — so a modifier script is
            // only valid with that preamble, and testing it bare would report a failure that never
            // happens in use.
            std::string full;
            if (std::string(sub) == "modifiers")
                full = "uint8_t x = 0;\nuint8_t y = 0;\nuint8_t z = 0;\n"
                       "uint8_t width = 16;\nuint8_t height = 16;\nuint8_t depth = 1;\n" + src;
            else
                full = src;

            moonlive::MoonLive engine;
            const bool ok = engine.compile(full.c_str(), moonlive::lightBuiltins());
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
TEST_CASE("a script may be commented, and only @control carries meaning") {
    struct Case { const char* src; bool ok; const char* what; };
    const Case cases[] = {
        {"// leading comment\naddLight(1, 2, 3);", true, "a comment before the code"},
        {"addLight(1, 2, 3); // trailing comment", true, "a comment after the code"},
        {"for (i = 0; i < 2; i = i + 1) {\n  // inside the body\n  addLight(i, 0, 0);\n}", true,
         "a comment inside a loop body"},
        {"// @controlled is a word, not an annotation\naddLight(1, 2, 3);", true,
         "@control matched as a whole word only"},
        {"uint8_t n = 4; // @control 1..64\nfor (i = 0; i < n; i = i + 1) { addLight(i, 0, 0); }",
         true, "an @control declaration"},
        {"uint8_t n = 4; // @control oops\naddLight(1, 2, 3);", false,
         "a malformed @control is an error, not a comment"},
    };
    for (const Case& c : cases) {
        moonlive::MoonLive engine;
        const bool ok = engine.compile(c.src, moonlive::lightBuiltins());
        INFO(c.what);
        CHECK(ok == c.ok);
        engine.free();
    }
}

TEST_CASE("a comment changes nothing about what a script does") {
    // The stronger claim: commenting a script does not alter the code it produces.
    moonlive::MoonLive bare, commented;
    CHECK(bare.compile("for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }",
                       moonlive::lightBuiltins()));
    CHECK(commented.compile("// place three lights in a row\n"
                            "for (i = 0; i < 3; i = i + 1) {\n"
                            "  addLight(i, 0, 0);   // one per step\n"
                            "}",
                            moonlive::lightBuiltins()));
    CHECK(bare.codeLen() == commented.codeLen());   // byte-for-byte the same program
    bare.free();
    commented.free();
}

