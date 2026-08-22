#pragma once

#include <cstdint>

#include "core/moonlive/MoonLiveIr.h"
#include "core/moonlive/moonlive_emit.h"   // RegBudget — the one thing a backend tells the allocator

// MoonLive register allocation — linear scan with spilling (Poletto & Sarkar, "Linear Scan Register
// Allocation", ACM TOPLAS 1999). The pass that makes a script's complexity a MEMORY question rather
// than a register-count one: a program naming more live values than the target has registers is
// rewritten so the overflow lives in the call frame, instead of being refused.
//
// It lives in CORE, once. Correct spilling across a loop back edge is the hardest logic in this
// compiler, and only the HOST backend is ever executed by tests (arm64 or x86-64, whichever the
// machine is) — four copies would leave three permanently untested (CLAUDE.md Principle 3). Every
// backend supplies a RegBudget and consumes two new IR ops; the algorithm appears nowhere in the
// platform layer.

namespace mm::moonlive {

/// Rewrite `ir` so no op names a vreg the target does not have, parking the overflow in frame slots
/// (Spill/Reload). A program that already fits is left byte-identical — a non-spilling script pays
/// nothing, not even a renumbering.
///
/// False when even the spilled form does not fit: fewer registers than the fixed ABI vregs plus the
/// reload temps need, more slots than the frame can address, or a branch structure the interval
/// analysis cannot prove properly nested. FAIL, NEVER MISCOMPILE — the caller reports a diagnostic
/// and the script runs dark, which is recoverable; a wrong interval silently computes with a stale
/// value, which is not.
///
/// `slotsUsed` receives how many frame slots the rewritten program needs, so the backend can size
/// its prologue: the program's own locals (`ir.localSlots`) plus anything this pass spilled. It is
/// therefore NOT zero when nothing spills — the locals still need their prologue capacity.
bool spillToBudget(IrProgram& ir, const RegBudget& budget, uint8_t& slotsUsed);

}  // namespace mm::moonlive
