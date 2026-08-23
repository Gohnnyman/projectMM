// @module MoonLive

// The RISC-V (ESP32-P4) backend's emitted code, checked on the development machine.
// See moonlive_device_codegen.inc for why this exists and unit_moonlive_codegen_xtensa.cpp for how
// the target guard is defined so the real emitter runs on this host.

#include "doctest.h"
#include "moonlive_script_wrap.h"

// System and standard headers FIRST, at global scope. The backend below is wrapped in a namespace,
// and anything it includes for the first time would otherwise be declared INSIDE that namespace —
// which under GCC breaks both `std::memcpy` (not found where the backend calls it) and the system
// `ssize_t` typedef that <cstdio> drags in. Including them here means the namespace only ever wraps
// OUR code, which is all it is meant to wrap.
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>


// The SHARED front end first, at real `mm::moonlive` scope: the backend below drags in the IR
// headers, and once their include guards are set the compiler header would otherwise resolve its
// types inside the wrapper namespace instead.
#include "core/moonlive/MoonLiveCompiler.h"
#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"
#include "core/moonlive/MoonLiveSpill.h"

namespace mm_riscv_backend {
// The IR, the spill pass and the platform seam are SHARED — one definition each in the binary.
// Pull them into scope so the backend's unqualified references resolve to those, and only the
// ISA-specific classes below end up local to this namespace.
namespace mm { using namespace ::mm; using namespace ::mm::moonlive;
               namespace moonlive { using namespace ::mm::moonlive;
                                    using ::mm::moonlive::spillToBudget; } }
#define __riscv 1
#include "platform/esp32/moonlive_asm_riscv.h"
#include "platform/esp32/moonlive_asm_riscv.cpp"
#undef __riscv
}  // namespace mm_riscv_backend

#define MM_ISA_NAME "RISC-V"
// Golden values, recorded from this backend. See the .inc for what they are and are not.
#define MM_GOLD_GRID_LEN  388u
#define MM_GOLD_FX_LEN    160u
#define MM_GOLD_FILLLOOP_LEN 336u  // fits on every backend since the host args moved to the frame
#define MM_GOLD_FXLOOP_LEN  236u
#define MM_GOLD_FXLOOP_HASH 3370938968u
#define MM_GOLD_FX_HASH   1088665379u
#define MM_ISA_LOWER mm_riscv_backend::mm::moonlive::lowerToBytes
// The assembler type itself, so the stack-budget check can measure the object the compile path
// puts on a 12 KB task rather than re-deriving its layout from the constants.
#define MM_ISA_ASM   mm_riscv_backend::mm::moonlive::RiscvAssembler
#include "moonlive_device_codegen.inc"



// The signed 16-bit load is lh (funct3 1) where the unsigned is lhu (funct3 5); the signed
// branch is bge (funct3 5) where the unsigned is bgeu (funct3 7). One field each, asserted on
// the encoder: a script-level test cannot tell these apart until a negative value flows, and by
// then the symptom is a picture, not a diff.
TEST_CASE("RISC-V load16S emits lh and branchGeS emits bge, one funct3 apart from unsigned") {
    using Asm = mm_riscv_backend::mm::moonlive::RiscvAssembler;
    using mm_riscv_backend::mm::moonlive::R0;
    using mm_riscv_backend::mm::moonlive::R1;
    auto word = [](const Asm& a, size_t i) {
        return uint32_t(a.bytes()[i]) | (uint32_t(a.bytes()[i+1]) << 8)
             | (uint32_t(a.bytes()[i+2]) << 16) | (uint32_t(a.bytes()[i+3]) << 24);
    };
    Asm lu(64); lu.load16(R0, R1, 4);
    Asm ls(64); ls.load16S(R0, R1, 4);
    REQUIRE(lu.size() == 4);
    REQUIRE(ls.size() == 4);
    CHECK((word(lu, 0) & 0x7f) == 0x03);           // load opcode
    CHECK(((word(lu, 0) >> 12) & 7) == 5);         // lhu
    CHECK(((word(ls, 0) >> 12) & 7) == 1);         // lh, sign-extending
    Asm bu(64); { auto l = bu.newLabel(); bu.branchGeU(R0, R1, l); bu.bind(l); bu.finalize(); }
    Asm bs(64); { auto l = bs.newLabel(); bs.branchGeS(R0, R1, l); bs.bind(l); bs.finalize(); }
    CHECK((word(bu, 0) & 0x7f) == 0x63);           // branch opcode
    CHECK(((word(bu, 0) >> 12) & 7) == 7);         // bgeu
    CHECK(((word(bs, 0) >> 12) & 7) == 5);         // bge, SIGNED
}
