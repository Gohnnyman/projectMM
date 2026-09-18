#pragma once

#include <cstdint>

#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"   // RegBudget, the one thing a backend tells the allocator

// MoonLive register allocation: linear scan with spilling, after Poletto and Sarkar (ACM TOPLAS 1999).
// A program naming more live values than the target has registers is rewritten to park the overflow
// in the call frame, so a script's complexity is a memory question rather than a register-count one.
//
// It lives in core, once. Spilling across a loop back edge is the hardest logic here, and only the
// host backend runs under test, so four copies would leave three permanently untested. Each backend
// supplies a RegBudget and consumes two IR ops; the algorithm appears nowhere in the platform layer.

namespace mm::moonlive {

// A program that already fits is left byte-identical, so a non-spilling script pays nothing.
// Fail rather than miscompile: a wrong interval computes silently with a stale value, where a
// refusal reports a diagnostic and the script runs dark.
// `slotsUsed` counts the program's own locals as well as anything spilled, so it is nonzero even
// when nothing spilled: the locals still need their prologue capacity.
/// Rewrite `ir` so no op names a register the target lacks, parking the overflow in frame slots.
bool spillToBudget(IrProgram& ir, const RegBudget& budget, uint8_t& slotsUsed);

}  // namespace mm::moonlive
