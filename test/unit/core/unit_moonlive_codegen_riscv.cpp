// @module MoonLive

// The RISC-V (ESP32-P4) backend's emitted code, checked on the development machine.
// See moonlive_device_codegen.inc for why this exists and unit_moonlive_codegen_xtensa.cpp for how
// the target guard is defined so the real emitter runs on this host.

#include "doctest.h"

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
#include "platform/esp32/moonlive_lower_riscv.cpp"
#undef __riscv
}  // namespace mm_riscv_backend

#define MM_ISA_NAME "RISC-V"
// Golden values, recorded from this backend. See the .inc for what they are and are not.
#define MM_GOLD_GRID_LEN  380u
#define MM_GOLD_FX_LEN    152u
#define MM_GOLD_FILLLOOP_LEN 328u  // RISC-V has the registers; Xtensa does not
#define MM_GOLD_FXLOOP_LEN  228u
#define MM_GOLD_FXLOOP_HASH 2017186717u
#define MM_GOLD_FX_HASH   4281239978u
#define MM_ISA_LOWER mm_riscv_backend::mm::moonlive::lowerToBytes
#include "moonlive_device_codegen.inc"
