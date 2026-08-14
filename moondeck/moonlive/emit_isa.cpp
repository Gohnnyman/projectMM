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
#include "platform/desktop/moonlive_asm_host.cpp"
#else
#error "define MM_EMIT_XTENSA, MM_EMIT_RISCV or MM_EMIT_ARM64"
#endif

// The lowerer body, with the emit seam it expects.
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/MoonLiveBuiltins.h"
// The CANONICAL declaration, rather than a copy: this tool once redeclared a three-argument
// lowerToBytes that linked only because it never passes `squeeze`, so the tool's view of the
// backend could drift from the backend's own.
#include "core/moonlive/moonlive_emit.h"

#if defined(MM_EMIT_XTENSA)
#include "platform/esp32/moonlive_lower_xtensa.cpp"
#undef __XTENSA__
#elif defined(MM_EMIT_RISCV)
#include "platform/esp32/moonlive_lower_riscv.cpp"
#undef __riscv
#elif defined(MM_EMIT_ARM64)
#include "platform/desktop/moonlive_lower_host.cpp"
#undef __aarch64__
#endif

#include "core/moonlive/MoonLiveCompiler.h"
#include "light/moonlive/MoonLiveBuiltins_light.h"
using namespace mm;
int main(int argc, char** argv) {
    const char* src = argc > 1 ? argv[1] : "for (i = 0; i < 3; i = i + 1) { addLight(i, 0, 0); }";
    uint8_t buf[4096];
    // Which BINDING to compile as, because the system-variable tables are different vocabularies and
    // not nested supersets: a modifier is handed `x`/`y`/`z`, and a LAYOUT deliberately is not, so it
    // may use those names as ordinary loop counters — which the shipped grid.mlv does. Compiling
    // every script against the widest table therefore refuses exactly the scripts most worth
    // inspecting ("name is a system variable"), which is how this tool came to never see grid.mlv.
    const char* binding = argc > 2 ? argv[2] : "layout";
    const auto sysvars = std::strcmp(binding, "modifier") == 0 ? moonlive::modifierSysVars()
                       : std::strcmp(binding, "effect")   == 0 ? moonlive::effectSysVars()
                                                               : moonlive::layoutSysVars();
    auto r = moonlive::compileSource(src, moonlive::lightBuiltins(), sysvars, buf, sizeof(buf));
    if (!r.ok) { printf("compile failed: %s\n", r.error); return 1; }
    printf("# %s\n# %zu bytes\n", src, r.len);
    for (size_t i = 0; i < r.len; i++) printf("%02x%s", buf[i], (i % 16 == 15) ? "\n" : " ");
    printf("\n");
    return 0;
}
