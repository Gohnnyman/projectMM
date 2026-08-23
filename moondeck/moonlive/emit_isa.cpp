#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>

// Compile a MoonLive script through ONE REAL BACKEND on the development machine, and print the
// bytes it emits. Each backend is guarded by its target macro (`__XTENSA__`, `__riscv`,
// `__aarch64__`) so on this host it normally compiles to nothing; defining the macro and including
// the sources is what lets the host run a device emitter. The firmware build never sees this file,
// so there is still exactly one definition of each backend in the shipped binary.
//
// Which backend is selected by -DMM_EMIT_<ISA> from disasm.py. One file rather than one per ISA:
// the whole body below is identical for every backend, and only the includes differ.

#if defined(MM_EMIT_XTENSA)
#define __XTENSA__ 1
#include "platform/esp32/moonlive_asm_xtensa.h"
#include "platform/esp32/moonlive_asm_xtensa.cpp"
#elif defined(MM_EMIT_RISCV)
// The RISC-V backend tests `__riscv` only, so defining it is enough to expose the whole file.
#define __riscv 1
#include "platform/esp32/moonlive_asm_riscv.h"
#include "platform/esp32/moonlive_asm_riscv.cpp"
#elif defined(MM_EMIT_ARM64)
#define __aarch64__ 1
#include "platform/desktop/moonlive_asm_host.h"
#include "platform/desktop/moonlive_asm_arm64.cpp"
#elif defined(MM_EMIT_X86_64)
// No macro to define: the desktop backend's x86-64 branch is selected by the HOST's own
// `__x86_64__` / `_M_X64`, so this ISA reads what the machine already compiles. That also means
// it only works ON an x86-64 host: the arm64 branch above it wins on an Apple Silicon machine,
// and a native architecture macro cannot be undefined. The device ISAs cross-emit because they
// are gated on macros no host defines; this one does not, which is the honest trade for having
// the same file serve as both the host backend and a tool target.
//
// REFUSED rather than silently wrong on any other host. Without this, an arm64 machine compiles
// the arm64 branch, disasm.py hands the bytes to objdump as i386:x86-64, and the output is a
// plausible-looking x86-64 listing of arm64 instructions: the exact "the tool answered, and the
// answer was fiction" failure this tool exists to end.
#if !defined(__x86_64__) && !defined(_M_X64)
#error "MM_EMIT_X86_64 requires an x86-64 host; run --isa x86_64 on an x86-64 machine"
#endif
#include "platform/desktop/moonlive_asm_host.h"
#include "platform/desktop/moonlive_asm_x86_64.cpp"
#else
#error "define MM_EMIT_XTENSA, MM_EMIT_RISCV, MM_EMIT_ARM64 or MM_EMIT_X86_64"
#endif

// The lowerer body, with the emit seam it expects.
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveBuiltins.h"
// The CANONICAL declaration, rather than a copy: this tool once redeclared a three-argument
// lowerToBytes that linked only because it never passes `squeeze`, so the tool's view of the
// backend could drift from the backend's own.
#include "core/moonlive/moonlive_emit.h"

// Each backend now carries its own lowerToBytes, INSIDE its arch guard and beside the assembler
// it names, so including the asm file above already brought it in. There is nothing more to
// include here; only the arch macros this file forced still have to come back off.
#if defined(MM_EMIT_XTENSA)
#undef __XTENSA__
#elif defined(MM_EMIT_RISCV)
#undef __riscv
#elif defined(MM_EMIT_ARM64)
#undef __aarch64__
#endif

#include "core/moonlive/MoonLiveCompiler.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
using namespace mm;
int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }";
    // Sized the way the ENGINE sizes it, from the script's own token count. A fixed 4 KB refused
    // ripples.mle and rose.mll on RISC-V, which emits ~1.3x Xtensa: the tool reported "codegen
    // failed (too large)" for scripts a device compiles without trouble, so the one place that
    // measures emitted size was lying about the two largest scripts.
    static uint8_t buf[moonlive::kCodeCap];
    // Which BINDING to compile as, because the system-variable tables are different vocabularies and
    // not nested supersets: a modifier is handed `x`/`y`/`z`, and a LAYOUT deliberately is not, so it
    // may use those names as ordinary loop counters — which the shipped grid.mll does. Compiling
    // every script against the widest table therefore refuses exactly the scripts most worth
    // inspecting ("name is a system variable"), which is how this tool came to never see grid.mll.
    const char* binding = argc > 2 ? argv[2] : "layout";
    const auto sysvars = std::strcmp(binding, "modifier") == 0 ? moonlive::modifierSysVars()
                       : std::strcmp(binding, "effect")   == 0 ? moonlive::effectSysVars()
                                                               : moonlive::layoutSysVars();
    // A string pool, as the engine supplies one: `addUint8("name", ...)` interns its label there
    // and the emitted code carries a pointer to it. Static so the pointers stay valid while the
    // bytes below are dumped.
    static char strings[moonlive::CompileResult::kStringPool];
    auto r = moonlive::compileSource(src, moonlive::lightBuiltins(), sysvars, buf, moonlive::codeCapFor(moonlive::countTokens(src)),
                                     nullptr, nullptr, strings, sizeof(strings));
    if (!r.ok) { printf("compile failed: %s\n", r.error); return 1; }
    printf("# %s\n# %zu bytes\n", src, r.len);
    for (size_t i = 0; i < r.len; i++) printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n");
    return 0;
}
