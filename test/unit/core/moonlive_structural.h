// One decoded instruction, as the structural checker sees it (moonlive_structural.inc).
//
// Split from the .inc because each ISA's decoder must be DECLARED before the checks that call it
// and DEFINED after — both need this type, so it cannot live in either.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mm_structural {

// A field is "absent" when its `has` flag is false; an instruction that names no frame offset
// (most of them) simply reports none.
struct Decoded {
    uint8_t  len = 0;            // 0 means "could not decode" — the walk stops and the test fails
    bool     hasFrameOff = false;
    uint32_t frameOff = 0;       // byte offset from the frame pointer
    bool     hasTarget = false;
    int32_t  target = 0;         // absolute byte offset of the branch/jump destination
    // The frame size this instruction ALLOCATES, when it is the prologue. Read from the emitted
    // instruction rather than recomputed by the test: a checker that models the frame with its own
    // copy of the formula agrees with the backend even when the backend is wrong, which is exactly
    // how the first version of this check passed against the bug it was written for.
    bool     hasFrameAlloc = false;
    uint32_t frameAlloc = 0;

    // For the call-clobber check. A windowed call rotates the register file, so every register the
    // rotation covers holds the callee's leftovers afterwards unless the caller saved and restored
    // it. `readsMask`/`writesMask` are bit N = register N.
    bool     isCall = false;
    uint32_t readsMask = 0;
    uint32_t writesMask = 0;
};

}  // namespace mm_structural
