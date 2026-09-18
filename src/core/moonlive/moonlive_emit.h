#pragma once

#include <cstdint>
#include <cstddef>

// MM_MOONLIVE_HAS_HOST_JIT is defined in platform_config.h (per-platform), so the arch check stays
// behind the platform boundary and this header stays neutral.
//
// It exists for the test harness alone: no shipping code branches on it. A build with no backend
// degrades at run time instead, reporting the failure and rendering dark. A test that calls render()
// cannot be compiled there at all, which is why this is a macro rather than a constexpr: one answers
// the program, the other answers the build.
#include "platform/platform.h"

// MoonLive per-ISA code emitter, the backend seam.
//
// This header is the neutral declaration the engine calls, and the implementation is per-ISA behind
// the platform boundary: Xtensa on the classic and S3, RISC-V on the P4, the host ISA on desktop.
// The engine asks for bytes and runs them, so adding an ISA leaves it and the front-end unchanged.
//
// The emitted routine writes a fixed color to every light and returns. The engine copies the bytes
// into an executable block and calls them through FillFn.

namespace mm::moonlive {

using FillFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl);

// The animated routine also takes a per-frame `t`, so the same native code animates each tick.
using AnimFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t);

// The compiled routine takes a fifth argument: the control-values arena. The host updates a byte
// when a slider moves and the next call reads it, so a control edit needs no recompile.
using CtrlFn = void (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);

// Calling a function that returns nothing through this alias reads whatever the register held, so
// a binding asks hasEntry(name) first.
/// The same emitted function called for its answer, which is how `dimensions()` reports.
using ValueFn = uintptr_t (*)(uint8_t* buf, uint32_t nLights, uint8_t cpl, uint32_t t, const uint8_t* ctrls);

// Returns the byte count, or 0 when `cap` is too small. The codegen reproduces these exact bytes.
size_t emitFill(uint8_t* out, size_t cap, uint8_t r, uint8_t g, uint8_t b);

// Emit a routine deriving its color from the runtime argument `t`, which proves a per-frame host
// value flows into the emitted code. Same path as emitFill with one extra argument.
size_t emitAnimatedFill(uint8_t* out, size_t cap);

struct IrProgram;   // src/core/moonlive/MoonLiveIr.h

// Why the last lowering returned 0, since four distinct refusals once shared one return value.
enum class LowerRefusal : uint8_t { None, Spill, NullCall, AsmOverflow, OverCap };
inline LowerRefusal& lowerRefusal() { thread_local LowerRefusal r = LowerRefusal::None; return r; }
// Which allocator guard fired and the budget it saw, so a device names the cause.
struct SpillDetail { uint8_t guard = 0, avail = 0, temps = 0, vregs = 0, slots = 0, spilled = 0; };
inline SpillDetail& spillDetail() { thread_local SpillDetail d; return d; }

// What one target's register file offers the allocator, which is all the spill pass knows of an ISA.
struct RegBudget {
    uint8_t regs = 0;        // machine registers the vreg map exposes (kRegCount)
    /// Registers the backend keeps for the inline ops this program contains.
    uint8_t reserved = 0;
    uint8_t slots = 0;       // spill slots the backend's frame can address (0 = cannot spill at all)

    // Saturating, so a scratch demand exceeding the file reports no registers rather than wrapping.
    /// Registers left for the allocator once the backend's inline scratch is taken out.
    uint8_t allocatable() const { return regs > reserved ? uint8_t(regs - reserved) : uint8_t(0); }
};

// Returns 0 on overflow, and `ir` is non-const because the allocator rewrites an over-wide program.
/// Lower a typed IR program to machine code for this unit's ISA.
size_t lowerToBytes(IrProgram& ir, uint8_t* out, size_t cap, const RegBudget* squeeze = nullptr);

}  // namespace mm::moonlive
