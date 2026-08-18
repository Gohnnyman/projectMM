#include "core/moonlive/moonlive_lower.h"   // the one IR walk, shared by every backend
#include "moonlive_asm_riscv.h"

// MoonLive RISC-V backend (ESP32-P4 / S31): bind the shared lowering to the RV32 assembler.
// The IR walk itself is core's (moonlive_lower.h); what is RISC-V here is the assembler that
// encodes each instruction, and the register count its map exposes.

#if defined(__riscv)

namespace mm::moonlive {

size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze) {
    return lowerWith<RiscvAssembler>(ir, out, cap, squeeze, kRegCount);
}

}  // namespace mm::moonlive

#endif  // __riscv
