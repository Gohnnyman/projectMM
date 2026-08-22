#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_host.h"

// MoonLive desktop backend: bind the shared lowering to the host assembler. The IR walk itself is
// core's (moonlive_lower.h); what is host-specific is the assembler and its register count.
//
// arm64 and x86-64 are supported (the assembler is in moonlive_asm_host.cpp). Any other host ISA
// has no assembler, so lowerToBytes returns 0 and a compile fails cleanly: scripted modules render
// dark, the device keeps running.

namespace mm::moonlive {

#if (defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<HostAssembler>(ir, out, cap, squeeze, kRegCount);
}

#else

// No backend on this ISA: refuse rather than emit something that cannot run.
size_t lowerToBytes(IrProgram&, uint8_t*, size_t, const RegBudget*) { return 0; }

#endif

}  // namespace mm::moonlive
