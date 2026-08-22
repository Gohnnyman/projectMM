#include "core/moonlive/moonlive_emit.h"

// The fill routines for a host with NO backend, the third case beside moonlive_emit_arm64.cpp and
// moonlive_emit_x86_64.cpp. See moonlive_asm_noarch.cpp for when this is reached.

#if !((defined(__aarch64__) || defined(__x86_64__) || defined(_M_X64)) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT))

namespace mm::moonlive {

size_t emitFill(uint8_t*, size_t, uint8_t, uint8_t, uint8_t) { return 0; }
size_t emitAnimatedFill(uint8_t*, size_t) { return 0; }

}  // namespace mm::moonlive

#endif
