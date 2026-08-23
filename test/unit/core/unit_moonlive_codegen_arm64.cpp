// @module MoonLive

// The arm64 (Apple Silicon desktop) host backend's new signed-value encodings, checked BYTE for
// BYTE. Twin of unit_moonlive_codegen_x86_64.cpp for the same reason it exists: an encoding bug
// in JIT-emitted bytes surfaces as a fault inside anonymous executable memory, and a pinned byte
// sequence turns that into "this word is wrong". Runs only on arm64 hosts, where HostAssembler
// compiles as the arm64 branch of the platform backend; skipped elsewhere.
//
// Scoped to the signed additions (load16S, branchGeS): the pre-existing arm64 encodings are
// covered by every compile-through-run test on this host, which executes them for real.

#include "doctest.h"

#if defined(__aarch64__) && !defined(MM_MOONLIVE_FORCE_NO_HOST_JIT)

#include "platform/desktop/moonlive_asm_host.h"

#include <cstdint>

using namespace mm::moonlive;

namespace {
uint32_t word(const HostAssembler& a, size_t i) {
    return uint32_t(a.bytes()[i]) | (uint32_t(a.bytes()[i + 1]) << 8)
         | (uint32_t(a.bytes()[i + 2]) << 16) | (uint32_t(a.bytes()[i + 3]) << 24);
}
}  // namespace

// ldrsh (signed, opc 11) against ldrh (unsigned, opc 01): the top byte is the whole difference,
// and it is what makes an int16_t member read back negative rather than as 65436.
TEST_CASE("arm64: load16S emits ldrsh where load16 emits ldrh") {
    HostAssembler u; u.load16(R0, R1, 4); u.finalize();
    HostAssembler s; s.load16S(R0, R1, 4); s.finalize();
    REQUIRE(u.size() == 4);
    REQUIRE(s.size() == 4);
    CHECK((word(u, 0) & 0xFFC00000u) == 0x79400000u);   // ldrh  w, [x, #imm]
    CHECK((word(s, 0) & 0xFFC00000u) == 0x79C00000u);   // ldrsh w, [x, #imm]
    // Same halfword-scaled immediate field in both.
    CHECK(((word(s, 0) >> 10) & 0xFFFu) == 2u);
}

// b.ge (cond 0xA) against b.hs (cond 0x2): the condition nibble is what decides whether a
// negative compares below zero or above everything.
TEST_CASE("arm64: branchGeS branches on GE where branchGeU branches on HS") {
    HostAssembler u; { auto l = u.newLabel(); u.branchGeU(R0, R1, l); u.bind(l); u.finalize(); }
    HostAssembler s; { auto l = s.newLabel(); s.branchGeS(R0, R1, l); s.bind(l); s.finalize(); }
    REQUIRE(u.size() == 8);                             // cmp + b.cond
    REQUIRE(s.size() == 8);
    CHECK((word(u, 0) & 0xFFE0001Fu) == 0x6B00001Fu);   // cmp wA, wB (subs wzr): 32-bit, matching
                                                        // the x86-64 compare width
    CHECK((word(u, 4) & 0xFF00000Fu) == 0x54000002u);   // b.hs
    CHECK((word(s, 4) & 0xFF00000Fu) == 0x5400000Au);   // b.ge, SIGNED
}

#else
TEST_CASE("arm64 codegen: skipped (not an arm64 host)") { CHECK(true); }
#endif
