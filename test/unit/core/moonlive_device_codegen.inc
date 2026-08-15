// The checks a DEVICE backend's emitted code must pass, written once and run per ISA.
//
// Why this exists: only the host (arm64) backend is ever EXECUTED by tests, so an Xtensa or RISC-V
// codegen defect used to ship silently and be debugged by flashing a board. That is what happened —
// a scripted layout reset every S3, and five plausible theories read off the source were each
// falsified by the device before the disassembler found the real cause. The backends are ordinary
// C++ behind a target guard, so defining the guard and compiling the sources runs the REAL emitter
// on the development machine, and a defect becomes a failing test instead of a bench session.
//
// ONE TRANSLATION UNIT PER ISA, and this file is the body they share. Two backends cannot live in
// one TU: they both define `Reg`, `Label` and an assembler class, and the include guards would give
// the second one the first one's types. Each ISA's .cpp defines MM_ISA_NAME + the target macro,
// includes its backend and the front end above it, then includes this — so the unqualified
// `lowerToBytes` inside compileSource resolves to that backend. The firmware build never sees these
// files, so a device image still holds exactly one backend definition. Same mechanism as
// moondeck/moonlive/emit_xtensa.cpp, which reads encodings without a flash.
//
// Expects, from the including TU: MM_ISA_NAME (a string literal) and the backend + compiler already
// included in the enclosing namespace.

#include "light/moonlive/MoonLiveBuiltins_light.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <cstdint>
#include <vector>

namespace {

// The shipped default layout: a nested loop whose body calls addLight. The script that crashed
// every S3, so the one worth pinning hardest.
const char* kGridLayout =
    "uint8_t cols = 16;  // @control 1..64\n"
    "uint8_t rows = 16;  // @control 1..64\n"
    "for (y = 0; y < rows; y = y + 1) {\n"
    "  for (x = 0; x < cols; x = x + 1) {\n"
    "    addLight(x, y, 0);\n"
    "  }\n"
    "}\n";

// The two shapes an effect takes: straight-line, and a loop over the grid.
const char* kEffectSimple = "setRGB(0, 255, 0, 0);\n";
const char* kEffectLoop   = "for (i = 0; i < 64; i = i + 1) { setRGB(i, i, 255 - i, 60); }\n";

/// Compile for THIS TU's backend. `sysvars` picks the binding's vocabulary — they are different
/// sets, not nested ones: a LAYOUT may use `x`/`y` as loop counters precisely because it is not
/// handed them, which is what the shipped grid.mlv does.
std::vector<uint8_t> emitBytes(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    // Sized the way production sizes it, from the script's own token count — a test that always
    // allocated the sanity bound would pass while a right-sized caller overflowed.
    std::vector<uint8_t> out(mm::moonlive::codeCapFor(mm::moonlive::countTokens(src)));
    auto r = mm::moonlive::compileSource(src, mm::moonlive::lightBuiltins(), sysvars,
                                         out.data(), out.size(), nullptr, MM_ISA_LOWER);
    ok = r.ok;
    out.resize(r.ok ? r.len : 0);
    return out;
}

size_t emitLen(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    return emitBytes(src, sysvars, ok).size();
}

/// FNV-1a over the emitted bytes — the project's idiom for "did this change".
///
/// Only meaningful for a script with NO host call. A `Call` bakes the builtin's ADDRESS into the
/// instruction stream and ASLR moves that every process, so the bytes of a calling script genuinely
/// differ run to run and no golden value can pin them. Those scripts are covered by the structural
/// checks and by their emitted LENGTH instead, which is stable.
uint32_t emitHash(const char* src, const mm::moonlive::SysVarTable& sysvars, bool& ok) {
    const auto code = emitBytes(src, sysvars, ok);
    uint32_t h = 2166136261u;
    for (uint8_t byte : code) { h ^= byte; h *= 16777619u; }
    return h;
}

}  // namespace

// The shipped default layout has to COMPILE for every target. It stopped compiling on Xtensa when
// the register file shrank to the ten registers the windowed ABI actually leaves free, and a script
// that is refused reads to a user exactly like one that is broken.
TEST_CASE("the shipped grid layout compiles for " MM_ISA_NAME) {
    bool ok = false;
    CHECK(emitLen(kGridLayout, mm::moonlive::layoutSysVars(), ok) > 0);
    CHECK(ok);
}

TEST_CASE("a scripted effect compiles for " MM_ISA_NAME ", straight-line and looped") {
    bool ok = false;
    CHECK(emitLen(kEffectSimple, mm::moonlive::effectSysVars(), ok) > 0);
    CHECK(ok);
    // A LOOPED effect is the case that did not fit Xtensa's ten registers until call arguments were
    // staged through the frame: a four-operand setRGB used to need four registers live at once, plus
    // whatever its operands took to compute, and the allocator ran out. Now only one argument holds
    // a register at a time.
    CHECK(emitLen(kEffectLoop, mm::moonlive::effectSysVars(), ok) > 0);
    CHECK(ok);
}

// `fill` plus a loop was the shape that did NOT fit the smallest register file: FillElems needs two
// scratch registers above the program's, and on Xtensa that left eight where the five fixed ABI
// vregs plus four reload temps needed nine — one short, so the golden here was 0 (REFUSED). Moving
// the host arguments into frame slots freed those five, and it now emits on every backend.
// Still pinned by length: it is the shape most sensitive to the register budget, so a regression
// that pushes any backend back over the edge shows up here first.
TEST_CASE("fill plus a loop on " MM_ISA_NAME " emits what it currently can") {
    bool ok = false;
    CHECK(emitLen("fill(0,0,0);\nfor (y = 0; y < height; y = y + 1) { setRGB(y, 255, 0, 0); }",
                  mm::moonlive::effectSysVars(), ok) == MM_GOLD_FILLLOOP_LEN);
}

// A scripted MODIFIER is the third binding, and the one handed x/y/z.
TEST_CASE("a scripted modifier compiles for " MM_ISA_NAME) {
    bool ok = false;
    CHECK(emitLen("setXYZ(0, x, y, z);\n", mm::moonlive::modifierSysVars(), ok) > 0);
    CHECK(ok);
}

// The GOLDEN check: the exact machine code for the shipped scripts.
//
// A change DETECTOR, not a correctness claim. It fails on any emission change so the diff gets read
// deliberately, instead of the change being discovered by flashing a board and watching it reset.
// When a change is intended, update the constants and say why in the commit.
//
// Hashed rather than length-checked: altering a frame offset or a register field changes what
// EXECUTES without changing how many bytes it takes — which is precisely the defect class this file
// exists to catch, and a length alone sails straight past it.
TEST_CASE("the " MM_ISA_NAME " machine code for the shipped scripts is what it was") {
    bool ok = false;
    // The grid CALLS addLight, so its bytes carry a host address that moves each process — length
    // only. The effect is pure inline code, so it is pinned byte for byte.
    CHECK(emitLen(kGridLayout, mm::moonlive::layoutSysVars(), ok) == MM_GOLD_GRID_LEN);
    CHECK(emitLen(kEffectSimple, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FX_LEN);
    CHECK(emitHash(kEffectSimple, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FX_HASH);
    CHECK(emitLen(kEffectLoop, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FXLOOP_LEN);
    CHECK(emitHash(kEffectLoop, mm::moonlive::effectSysVars(), ok) == MM_GOLD_FXLOOP_HASH);
}

// Degrade, never miscompile: a script the backend cannot encode must fail cleanly. The emitters
// signal that by returning zero bytes, which compileSource turns into a diagnostic — the device
// then shows the error and keeps rendering, instead of executing whatever the truncation produced.
TEST_CASE("an unencodable script is refused, not truncated, on " MM_ISA_NAME) {
    bool ok = true;
    // A loop nest far past what any label/fixup table holds.
    std::string deep;
    for (int i = 0; i < 40; i++) deep += "for (i" + std::to_string(i) + " = 0; i" +
                                        std::to_string(i) + " < 2; i" + std::to_string(i) +
                                        " = i" + std::to_string(i) + " + 1) {";
    deep += "addLight(0, 0, 0);";
    for (int i = 0; i < 40; i++) deep += "}";
    const size_t n = emitLen(deep.c_str(), mm::moonlive::layoutSysVars(), ok);
    CHECK(n == 0);      // refused
    CHECK_FALSE(ok);    // and said so
}

// Every script we SHIP has to compile on every backend, or a user's board choice silently decides
// which effects exist. `plasma.mlv` is why this test is here: it ran on an S3 and on desktop and was
// refused on an S31, because one `kCodeCap` constant was sized to the densest ISA while RISC-V emits
// up to 1.9x more for identical source. The buffer is now sized per script, and this pins that every
// shipped script still fits on every backend.
//
// Read from moonlive/ rather than copied inline: a copy is a second source of truth that drifts the
// first time a script is edited, and it is exactly the widest scripts — the ones worth checking —
// that would silently stop being the ones under test.
TEST_CASE("every shipped script compiles for " MM_ISA_NAME) {
    int checked = 0;
    for (const char* sub : {"layouts", "effects", "modifiers", "drivers"}) {
        const std::filesystem::path dir =
            std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path()
            / "moonlive" / sub;
        if (!std::filesystem::exists(dir)) continue;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".mlv") continue;
            std::ifstream f(entry.path());
            std::ostringstream ss;
            ss << f.rdbuf();
            const std::string src = ss.str();
            // Each binding supplies a DIFFERENT system-variable vocabulary, not a nested superset —
            // a layout may use x/y as loop counters precisely because it is not handed them.
            const std::string kind = sub;
            const auto& sysvars = kind == "layouts"   ? mm::moonlive::layoutSysVars()
                                : kind == "modifiers" ? mm::moonlive::modifierSysVars()
                                                      : mm::moonlive::effectSysVars();
            bool ok = false;
            const size_t n = emitLen(src.c_str(), sysvars, ok);
            INFO("script: ", kind, "/", entry.path().filename().string(), " on " MM_ISA_NAME);
            CHECK(ok);
            CHECK(n > 0);
            checked++;
        }
    }
    CHECK(checked > 0);   // a silently empty folder would pass without this
}
