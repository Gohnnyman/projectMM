#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_xtensa.h"

// MoonLive Xtensa backend (ESP32 classic / S3): bind the shared lowering to the Xtensa assembler.
// The IR walk itself is core's (moonlive_lower.h); what is Xtensa here is the assembler that
// encodes each instruction (including the windowed-ABI frame it owns), and the register count its
// map exposes, which is the smallest of the four and the one every budget question is decided by.

#if defined(__XTENSA__)

namespace mm::moonlive {

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<XtensaAssembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __XTENSA__
