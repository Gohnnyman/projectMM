#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveIr.h"

// The lowering for a host with NO assembler, and only that: the third case beside
// moonlive_asm_arm64.cpp and moonlive_asm_x86_64.cpp. "noarch" because that is exactly what it
// serves, an architecture we have no backend for.
//
// Two ways to land here. An ISA nobody has written an assembler for, and a deliberate --no-jit
// build (MM_MOONLIVE_FORCE_NO_HOST_JIT), which is a pre-merge gate: it is how a contributor sees
// what an unsupported desktop sees. Both must LINK and run everything that is not a script.
//
// Unlike the ESP32 side, which fails the build for an unknown ISA (every ESP32 is Xtensa or
// RISC-V, so a third is an unfinished port), a desktop legitimately has this case.

#if !((defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT))

namespace mm::moonlive {

// Refusing rather than emitting something that cannot run: MoonLive::compile then reports a
// failure and a scripted module renders dark, the same path a too-large or unparseable script
// takes.
size_t lowerToBytes(IrProgram&, uint8_t*, size_t, const RegBudget*) { return 0; }

}  // namespace mm::moonlive

#endif
