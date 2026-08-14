// @module MoonLive

// The Xtensa (ESP32 classic / S3) backend's emitted code, checked on the development machine.
// See moonlive_device_codegen.inc for why this exists.
//
// The backend is `#if defined(__XTENSA__)`, so on this host it normally compiles to nothing.
// Defining the macro and compiling the sources here runs the REAL emitter, and compileSource's
// `lower` seam points the shared front end at it — one function pointer rather than a second copy
// of the compiler. The firmware build never sees this file, so a device image still holds exactly
// one backend definition.

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

namespace mm_xtensa_backend {
// The IR, the spill pass and the platform seam are SHARED — one definition each in the binary.
// Pull them into scope so the backend's unqualified references resolve to those, and only the
// ISA-specific classes below end up local to this namespace.
namespace mm { using namespace ::mm; using namespace ::mm::moonlive;
               namespace moonlive { using namespace ::mm::moonlive;
                                    using ::mm::moonlive::spillToBudget; } }
#define __XTENSA__ 1
#include "platform/esp32/moonlive_asm_xtensa.h"
#include "platform/esp32/moonlive_asm_xtensa.cpp"
#include "platform/esp32/moonlive_lower_xtensa.cpp"
#undef __XTENSA__
}  // namespace mm_xtensa_backend

#define MM_ISA_NAME "Xtensa"
// Golden values, recorded from this backend. See the .inc for what they are and are not.
#define MM_GOLD_GRID_LEN  227u
#define MM_GOLD_FX_LEN    105u
#define MM_GOLD_FILLLOOP_LEN 254u  // fits now: the host arguments left the register file
#define MM_GOLD_FXLOOP_LEN  190u
#define MM_GOLD_FXLOOP_HASH 2197538220u
#define MM_GOLD_FX_HASH   1645855068u
#define MM_ISA_LOWER mm_xtensa_backend::mm::moonlive::lowerToBytes
#include "moonlive_device_codegen.inc"

// A register outside the backend's map is a value written somewhere the program does not own. The
// map is a2..a11: a12/a13 are call scratch and the store8 address register, and a14/a15 carry this
// routine's own retw.n return linkage — using those two as general registers corrupted the return
// path and produced `Guru Meditation (IllegalInstruction)` the moment a scripted layout ran.
// Asserted against the map itself rather than a copy, so the check cannot drift from its subject.
TEST_CASE("the Xtensa vreg map names only registers the windowed ABI leaves free") {
    uint8_t n = 0;
    const uint8_t* map = mm_xtensa_backend::mm::moonlive::xtRegMap(n);
    REQUIRE(n > 0);
    for (uint8_t i = 0; i < n; i++) {
        INFO("vreg R" << int(i) << " -> a" << int(map[i]));
        CHECK(map[i] >= 2);
        CHECK(map[i] <= 11);
    }
}

// A change DETECTOR, not a correctness claim: emitted length is a cheap fingerprint that moves
// whenever codegen does. A failure here means "read the diff and decide", not "you broke it" —
// update the number and say why in the commit. It exists because the alternative way to notice an
// emission change was to flash a board and watch it reset.
